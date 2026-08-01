#include "media_test_fixtures.hpp"
#include "media_test_support.hpp"
#include "palmier/audio/wasapi_environment_probe.hpp"
#include "palmier/windows/preview_presentation_controller.hpp"
#include "palmier/windows/project_load_coordinator.hpp"

#include <QDebug>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTest>
#include <QThread>
#include <QtConcurrent/QtConcurrentRun>

#include <Windows.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using palmier::preview::PreviewPresentationOutcome;
using palmier::preview::PreviewPresentationReceipt;
using palmier::preview::PreviewPresentationState;

template <typename Function>
auto awaitBackground(Function&& function) {
    using Result = std::invoke_result_t<std::decay_t<Function>>;
    auto future = QtConcurrent::run(std::forward<Function>(function));
    QFutureWatcher<Result> watcher;
    QEventLoop loop;
    QObject::connect(&watcher, &QFutureWatcher<Result>::finished, &loop, &QEventLoop::quit);
    watcher.setFuture(future);
    if (!future.isFinished()) loop.exec();
    return future.result();
}

struct ProjectPackageFixture final {
    std::shared_ptr<palmier::media::test_support::TemporaryDirectory> owner;
    std::filesystem::path package;
};

void writeFixtureFile(
    const std::filesystem::path& destination,
    const std::vector<std::uint8_t>& bytes
) {
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    palmier::media::test_support::require(
        output.is_open(),
        "project preview fixture could not be opened"
    );
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size())
    );
    palmier::media::test_support::require(
        output.good(),
        "project preview fixture could not be written"
    );
}

void writeFixtureFile(
    const std::filesystem::path& destination,
    std::string_view contents
) {
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    palmier::media::test_support::require(
        output.is_open(),
        "project preview manifest could not be opened"
    );
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    palmier::media::test_support::require(
        output.good(),
        "project preview manifest could not be written"
    );
}

ProjectPackageFixture createProjectPackageFixture() {
    auto owner = std::make_shared<palmier::media::test_support::TemporaryDirectory>();
    const auto package = owner->path() / "warp-project-preview.palmier";
    const auto mediaDirectory = package / "media";
    std::filesystem::create_directories(mediaDirectory);
    writeFixtureFile(package / "project.json", R"json({
  "timelines": [
    {
      "id": "timeline-main",
      "name": "Main",
      "fps": 10,
      "width": 16,
      "height": 16,
      "settingsConfigured": true,
      "tracks": [
        {
          "id": "track-v1",
          "type": "video",
          "muted": false,
          "hidden": false,
          "syncLocked": true,
          "clips": [
            {
              "id": "clip-h264-aac",
              "mediaRef": "media-h264-aac",
              "mediaType": "video",
              "sourceClipType": "video",
              "startFrame": 0,
              "durationFrames": 5,
              "speed": 1,
              "opacity": 0.75,
              "transform": {
                "centerX": 0.5,
                "centerY": 0.5,
                "width": 1,
                "height": 1,
                "rotation": 0,
                "flipHorizontal": false,
                "flipVertical": false
              },
              "effects": [{
                "id": "effect-exposure",
                "type": "color.exposure",
                "enabled": true,
                "params": {"ev": {"value": 1}}
              }],
              "blendMode": "normal"
            }
          ]
        }
      ]
    }
  ],
  "activeTimelineId": "timeline-main",
  "openTimelineIds": ["timeline-main"]
})json");
    writeFixtureFile(package / "media.json", R"json({
  "version": 2,
  "entries": [
    {
      "id": "media-h264-aac",
      "name": "H.264 AAC fixture",
      "type": "video",
      "source": {"project": {"relativePath": "media/h264-aac.mp4"}},
      "duration": 0.5,
      "sourceWidth": 16,
      "sourceHeight": 16,
      "sourceFPS": 10,
      "hasAudio": true
    }
  ],
  "folders": []
})json");
    writeFixtureFile(
        mediaDirectory / "h264-aac.mp4",
        palmier::media::test_support::decodeBase64(
            palmier::media::test_fixtures::h264Aac
        )
    );
    return {std::move(owner), package};
}

bool waitForPreviewTerminal(
    palmier::windows::PreviewPresentationController& controller,
    int timeoutMilliseconds
) {
    const auto terminal = [&controller] {
        return controller.state() == QStringLiteral("completed")
            || !controller.errorCode().isEmpty();
    };
    if (terminal()) return true;
    QSignalSpy stateChanges(
        &controller,
        &palmier::windows::PreviewPresentationController::stateChanged
    );
    if (terminal()) return true;
    QElapsedTimer elapsed;
    elapsed.start();
    while (!terminal() && elapsed.elapsed() < timeoutMilliseconds) {
        const auto remaining = timeoutMilliseconds - elapsed.elapsed();
        if (!stateChanges.wait(static_cast<int>(remaining))) break;
    }
    return terminal();
}

bool drainPreview(
    palmier::windows::ProjectLoadCoordinator& project,
    palmier::windows::PreviewPresentationController& controller,
    std::unique_ptr<QObject>& root
) {
    QSignalSpy projectReady(
        &project,
        &palmier::windows::ProjectLoadCoordinator::shutdownReady
    );
    QSignalSpy previewReady(
        &controller,
        &palmier::windows::PreviewPresentationController::shutdownReady
    );
    bool projectDrained = project.requestShutdown();
    bool previewDrained = controller.requestShutdown();
    if (!projectDrained && projectReady.count() == 0) {
        static_cast<void>(projectReady.wait(10000));
    }
    if (!previewDrained && previewReady.count() == 0) {
        static_cast<void>(previewReady.wait(10000));
    }
    projectDrained = projectDrained || projectReady.count() != 0;
    previewDrained = previewDrained || previewReady.count() != 0;
    const bool drained = projectDrained && previewDrained
        && controller.shutdownComplete();
    root.reset();
    return drained;
}

bool releaseFixture(ProjectPackageFixture& fixture) {
    auto owner = std::move(fixture.owner);
    return awaitBackground([owner = std::move(owner)]() mutable {
        const auto path = owner->path();
        owner.reset();
        std::error_code error;
        const bool remains = std::filesystem::exists(path, error);
        return !error && !remains;
    });
}

PreviewPresentationReceipt resizedReceipt() {
    PreviewPresentationReceipt receipt;
    receipt.state = PreviewPresentationState::idle;
    receipt.outcome = PreviewPresentationOutcome::changed;
    return receipt;
}

PreviewPresentationReceipt closedReceipt() {
    PreviewPresentationReceipt receipt;
    receipt.state = PreviewPresentationState::closed;
    receipt.outcome = PreviewPresentationOutcome::noOp;
    return receipt;
}

PreviewPresentationReceipt playingReceipt(std::uint64_t generation) {
    PreviewPresentationReceipt receipt;
    receipt.generation = generation;
    receipt.state = PreviewPresentationState::playing;
    receipt.outcome = PreviewPresentationOutcome::changed;
    return receipt;
}

PreviewPresentationReceipt completedReceipt(std::uint64_t generation) {
    PreviewPresentationReceipt receipt;
    receipt.generation = generation;
    receipt.state = PreviewPresentationState::completed;
    receipt.outcome = PreviewPresentationOutcome::noOp;
    return receipt;
}

struct FakeSessionState final {
    std::mutex mutex;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> sizes;
    std::vector<palmier::windows::PreviewMediaCandidateProjection> candidates;
    std::vector<std::thread::id> threads;
    QSemaphore firstResizeEntered;
    QSemaphore releaseFirstResize;
    QSemaphore tickEntered;
    QSemaphore releaseTick;
    bool gateFirstResize{};
    bool gateTick{};
    bool staleTick{};
    bool tickCancellationObserved{};
    bool failClose{};
    bool destroyed{};
    HWND window{};
    bool windowAliveAtDestruction{};
    std::uint64_t generation{};
    std::size_t tickCalls{};
    std::size_t cancelCalls{};
    std::size_t playingTicks{};
};

class FakeSession final : public palmier::windows::detail::QtPreviewSessionPort {
public:
    explicit FakeSession(std::shared_ptr<FakeSessionState> state)
        : state_(std::move(state)) {
        const std::lock_guard lock(state_->mutex);
        state_->threads.push_back(std::this_thread::get_id());
    }

    ~FakeSession() override {
        const std::lock_guard lock(state_->mutex);
        state_->threads.push_back(std::this_thread::get_id());
        state_->windowAliveAtDestruction = IsWindow(state_->window) != FALSE;
        state_->destroyed = true;
    }

    PreviewPresentationReceipt resize(
        std::uint32_t width,
        std::uint32_t height,
        std::stop_token cancellation
    ) override {
        bool gate;
        {
            const std::lock_guard lock(state_->mutex);
            gate = state_->gateFirstResize && state_->sizes.empty();
            state_->sizes.emplace_back(width, height);
            state_->threads.push_back(std::this_thread::get_id());
        }
        if (gate) {
            state_->firstResizeEntered.release();
            state_->releaseFirstResize.acquire();
        }
        if (cancellation.stop_requested()) {
            auto receipt = resizedReceipt();
            receipt.outcome = PreviewPresentationOutcome::cancelled;
            return receipt;
        }
        return resizedReceipt();
    }

    PreviewPresentationReceipt play(
        const palmier::windows::PreviewMediaCandidateProjection& candidate,
        std::stop_token cancellation
    ) override {
        const std::lock_guard lock(state_->mutex);
        state_->candidates.push_back(candidate);
        state_->threads.push_back(std::this_thread::get_id());
        if (cancellation.stop_requested()) {
            auto receipt = playingReceipt(state_->generation);
            receipt.state = PreviewPresentationState::cancelled;
            receipt.outcome = PreviewPresentationOutcome::cancelled;
            return receipt;
        }
        ++state_->generation;
        return playingReceipt(state_->generation);
    }

    PreviewPresentationReceipt tick(
        std::uint64_t expectedGeneration,
        std::stop_token cancellation
    ) override {
        bool gate;
        {
            const std::lock_guard lock(state_->mutex);
            ++state_->tickCalls;
            state_->threads.push_back(std::this_thread::get_id());
            gate = state_->gateTick;
            if (gate) state_->gateTick = false;
        }
        if (gate) {
            state_->tickEntered.release();
            std::stop_callback cancellationCallback(cancellation, [this] {
                state_->releaseTick.release();
            });
            state_->releaseTick.acquire();
            if (cancellation.stop_requested()) {
                const std::lock_guard lock(state_->mutex);
                state_->tickCancellationObserved = true;
                auto receipt = completedReceipt(expectedGeneration);
                receipt.state = PreviewPresentationState::cancelled;
                receipt.outcome = PreviewPresentationOutcome::cancelled;
                return receipt;
            }
        }
        {
            const std::lock_guard lock(state_->mutex);
            if (state_->staleTick) {
                auto receipt = playingReceipt(expectedGeneration + 1);
                receipt.outcome = PreviewPresentationOutcome::stale;
                return receipt;
            }
            if (state_->playingTicks > 0) {
                --state_->playingTicks;
                return playingReceipt(expectedGeneration);
            }
        }
        return completedReceipt(expectedGeneration);
    }

    PreviewPresentationReceipt cancel(std::uint64_t expectedGeneration) override {
        const std::lock_guard lock(state_->mutex);
        ++state_->cancelCalls;
        state_->threads.push_back(std::this_thread::get_id());
        auto receipt = completedReceipt(expectedGeneration);
        receipt.state = PreviewPresentationState::cancelled;
        receipt.outcome = PreviewPresentationOutcome::cancelled;
        return receipt;
    }

    PreviewPresentationReceipt close() override {
        const std::lock_guard lock(state_->mutex);
        state_->threads.push_back(std::this_thread::get_id());
        auto receipt = closedReceipt();
        if (state_->failClose) receipt.outcome = PreviewPresentationOutcome::failed;
        return receipt;
    }

private:
    std::shared_ptr<FakeSessionState> state_;
};

std::unique_ptr<QObject> createPreviewWindow(
    QQmlEngine& engine,
    palmier::windows::PreviewPresentationController& controller,
    int width = 320,
    int height = 180
) {
    engine.rootContext()->setContextProperty(
        QStringLiteral("nativePreviewWindow"),
        controller.window()
    );
    QQmlComponent component(&engine);
    QEventLoop componentLoad;
    QObject::connect(
        &component,
        &QQmlComponent::statusChanged,
        &componentLoad,
        [&componentLoad](QQmlComponent::Status status) {
            if (status != QQmlComponent::Loading) componentLoad.quit();
        }
    );
    const auto source = QStringLiteral(R"(
        import QtQuick
        Window {
            width: %1
            height: %2
            visible: true
            WindowContainer {
                anchors.fill: parent
                window: nativePreviewWindow
            }
        }
    )").arg(width).arg(height).toUtf8();
    component.setData(source, QUrl(QStringLiteral("inline:preview-window.qml")));
    if (component.isLoading()) componentLoad.exec();
    if (component.isError()) qWarning().noquote() << component.errorString();
    return std::unique_ptr<QObject>(component.create());
}

std::size_t resizeCount(const std::shared_ptr<FakeSessionState>& state) {
    const std::lock_guard lock(state->mutex);
    return state->sizes.size();
}

std::size_t playCount(const std::shared_ptr<FakeSessionState>& state) {
    const std::lock_guard lock(state->mutex);
    return state->candidates.size();
}

std::size_t tickCount(const std::shared_ptr<FakeSessionState>& state) {
    const std::lock_guard lock(state->mutex);
    return state->tickCalls;
}

std::size_t cancelCount(const std::shared_ptr<FakeSessionState>& state) {
    const std::lock_guard lock(state->mutex);
    return state->cancelCalls;
}

palmier::windows::ProjectPreviewProjection availablePreview(std::string mediaId) {
    return {
        palmier::windows::PreviewCandidateAvailability::available,
        {},
        palmier::windows::PreviewMediaCandidateProjection{
            std::filesystem::path(L"C:\\fixture.mp4"),
            {
                1920,
                1080,
                30,
                "timeline",
                "video-track",
                "clip-" + mediaId,
                std::move(mediaId),
                0,
                30,
                0,
                {},
                1,
                std::nullopt,
            },
            true,
        },
    };
}

bool sessionDestroyed(const std::shared_ptr<FakeSessionState>& state) {
    const std::lock_guard lock(state->mutex);
    return state->destroyed;
}

bool tickCancellationObserved(const std::shared_ptr<FakeSessionState>& state) {
    const std::lock_guard lock(state->mutex);
    return state->tickCancellationObserved;
}

class QtPreviewTests final : public QObject {
    Q_OBJECT

private slots:
    void nativeChildUsesOneBackgroundSessionOwner() {
        const auto uiThread = std::this_thread::get_id();
        auto state = std::make_shared<FakeSessionState>();
        palmier::windows::PreviewPresentationController controller(
            [state](HWND) { return std::make_unique<FakeSession>(state); },
            nullptr
        );
        QQmlEngine engine;
        auto root = createPreviewWindow(engine, controller);
        QVERIFY(root != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(controller.ready(), 5000);

        const auto child = reinterpret_cast<HWND>(controller.window()->winId());
        QVERIFY(IsWindow(child));
        QVERIFY(GetParent(child) != nullptr);
        QVERIFY((GetWindowLongPtrW(child, GWL_STYLE) & WS_CHILD) != 0);
        {
            const std::lock_guard lock(state->mutex);
            QVERIFY(!state->threads.empty());
            for (const auto thread : state->threads) {
                QVERIFY(thread != uiThread);
                QVERIFY(thread == state->threads.front());
            }
        }

        QSignalSpy shutdownReady(
            &controller,
            &palmier::windows::PreviewPresentationController::shutdownReady
        );
        QVERIFY(!controller.requestShutdown());
        QTRY_COMPARE_WITH_TIMEOUT(shutdownReady.count(), 1, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(controller.shutdownComplete(), 5000);
        {
            const std::lock_guard lock(state->mutex);
            QVERIFY(state->destroyed);
            for (const auto thread : state->threads) {
                QVERIFY(thread == state->threads.front());
            }
        }
        root.reset();
    }

    void resizeBurstKeepsOnlyLatestPhysicalSize() {
        auto state = std::make_shared<FakeSessionState>();
        state->gateFirstResize = true;
        palmier::windows::PreviewPresentationController controller(
            [state](HWND) { return std::make_unique<FakeSession>(state); },
            nullptr
        );
        QQmlEngine engine;
        auto root = createPreviewWindow(engine, controller);
        QVERIFY(root != nullptr);
        QTRY_COMPARE_WITH_TIMEOUT(state->firstResizeEntered.available(), 1, 5000);
        auto* rootWindow = qobject_cast<QQuickWindow*>(root.get());
        QVERIFY(rootWindow != nullptr);
        for (const auto size : {QSize(400, 220), QSize(500, 260), QSize(640, 360)}) {
            rootWindow->resize(size);
            QCoreApplication::processEvents();
        }
        state->releaseFirstResize.release();
        QTRY_COMPARE_WITH_TIMEOUT(resizeCount(state), std::size_t{2}, 5000);
        {
            const std::lock_guard lock(state->mutex);
            QCOMPARE(state->sizes.back().first, std::uint32_t{640});
            QCOMPARE(state->sizes.back().second, std::uint32_t{360});
        }
        static_cast<void>(controller.requestShutdown());
        QTRY_VERIFY_WITH_TIMEOUT(controller.shutdownComplete(), 5000);
        root.reset();
    }

    void projectCandidateTicksOnceAndStopsAtCompletion() {
        const auto uiThread = std::this_thread::get_id();
        auto state = std::make_shared<FakeSessionState>();
        palmier::windows::PreviewPresentationController controller(
            [state](HWND) { return std::make_unique<FakeSession>(state); },
            nullptr
        );
        QQmlEngine engine;
        auto root = createPreviewWindow(engine, controller);
        QVERIFY(root != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(controller.ready(), 5000);

        controller.replaceProjectPreview(1, availablePreview("media-a"));
        QTRY_COMPARE_WITH_TIMEOUT(playCount(state), std::size_t{1}, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(tickCount(state), std::size_t{1}, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), QStringLiteral("completed"), 5000);
        const auto playbackReceipt = controller.latestPlaybackReceipt();
        QCOMPARE(playbackReceipt.generation, std::uint64_t{1});
        QVERIFY(playbackReceipt.state == PreviewPresentationState::completed);
        QCoreApplication::processEvents();
        QCOMPARE(tickCount(state), std::size_t{1});
        controller.replaceProjectPreview(1, availablePreview("media-duplicate"));
        controller.replaceProjectPreview(0, availablePreview("media-invalid"));
        QCoreApplication::processEvents();
        QCOMPARE(playCount(state), std::size_t{1});
        {
            const std::lock_guard lock(state->mutex);
            QCOMPARE(
                state->candidates.front().renderLayer.mediaId,
                std::string("media-a")
            );
            for (const auto thread : state->threads) {
                QVERIFY(thread != uiThread);
                QVERIFY(thread == state->threads.front());
            }
        }

        static_cast<void>(controller.requestShutdown());
        QTRY_VERIFY_WITH_TIMEOUT(controller.shutdownComplete(), 5000);
        root.reset();
    }

    void projectCandidateWaitsForActiveSurfaceAttach() {
        auto state = std::make_shared<FakeSessionState>();
        state->gateFirstResize = true;
        palmier::windows::PreviewPresentationController controller(
            [state](HWND) { return std::make_unique<FakeSession>(state); },
            nullptr
        );
        QQmlEngine engine;
        auto root = createPreviewWindow(engine, controller);
        QVERIFY(root != nullptr);
        QTRY_COMPARE_WITH_TIMEOUT(state->firstResizeEntered.available(), 1, 5000);

        controller.replaceProjectPreview(1, availablePreview("media-during-attach"));
        state->releaseFirstResize.release();
        QTRY_COMPARE_WITH_TIMEOUT(playCount(state), std::size_t{1}, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), QStringLiteral("completed"), 5000);
        {
            const std::lock_guard lock(state->mutex);
            QCOMPARE(
                state->candidates.front().renderLayer.mediaId,
                std::string("media-during-attach")
            );
        }

        static_cast<void>(controller.requestShutdown());
        QTRY_VERIFY_WITH_TIMEOUT(controller.shutdownComplete(), 5000);
        root.reset();
    }

    void resizeDuringGatedTickResumesBoundedCadence() {
        auto state = std::make_shared<FakeSessionState>();
        state->gateTick = true;
        state->playingTicks = 1;
        palmier::windows::PreviewPresentationController controller(
            [state](HWND) { return std::make_unique<FakeSession>(state); },
            nullptr
        );
        QQmlEngine engine;
        auto root = createPreviewWindow(engine, controller);
        QVERIFY(root != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(controller.ready(), 5000);

        controller.replaceProjectPreview(1, availablePreview("media-resize"));
        QTRY_COMPARE_WITH_TIMEOUT(state->tickEntered.available(), 1, 5000);
        auto* rootWindow = qobject_cast<QQuickWindow*>(root.get());
        QVERIFY(rootWindow != nullptr);
        rootWindow->resize(QSize(640, 360));
        QCoreApplication::processEvents();
        state->releaseTick.release();
        QTRY_COMPARE_WITH_TIMEOUT(resizeCount(state), std::size_t{2}, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(tickCount(state), std::size_t{2}, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), QStringLiteral("completed"), 5000);

        static_cast<void>(controller.requestShutdown());
        QTRY_VERIFY_WITH_TIMEOUT(controller.shutdownComplete(), 5000);
        root.reset();
    }

    void staleTickCancelsWithoutRetryLoop() {
        auto state = std::make_shared<FakeSessionState>();
        state->staleTick = true;
        palmier::windows::PreviewPresentationController controller(
            [state](HWND) { return std::make_unique<FakeSession>(state); },
            nullptr
        );
        QQmlEngine engine;
        auto root = createPreviewWindow(engine, controller);
        QVERIFY(root != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(controller.ready(), 5000);

        controller.replaceProjectPreview(1, availablePreview("media-stale"));
        QTRY_COMPARE_WITH_TIMEOUT(controller.errorCode(), QStringLiteral("previewStale"), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(cancelCount(state), std::size_t{1}, 5000);
        QCoreApplication::processEvents();
        QCOMPARE(playCount(state), std::size_t{1});
        QCOMPARE(tickCount(state), std::size_t{1});
        QCOMPARE(controller.state(), QStringLiteral("failed"));

        static_cast<void>(controller.requestShutdown());
        QTRY_VERIFY_WITH_TIMEOUT(controller.shutdownComplete(), 5000);
        root.reset();
    }

    void replacementCancelsGatedTickBeforePublishingOffline() {
        auto state = std::make_shared<FakeSessionState>();
        state->gateTick = true;
        palmier::windows::PreviewPresentationController controller(
            [state](HWND) { return std::make_unique<FakeSession>(state); },
            nullptr
        );
        QQmlEngine engine;
        auto root = createPreviewWindow(engine, controller);
        QVERIFY(root != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(controller.ready(), 5000);

        controller.replaceProjectPreview(1, availablePreview("media-old"));
        QTRY_COMPARE_WITH_TIMEOUT(state->tickEntered.available(), 1, 5000);
        QCOMPARE(tickCount(state), std::size_t{1});
        controller.replaceProjectPreview(2, {
            palmier::windows::PreviewCandidateAvailability::offline,
            "mediaFileUnavailable",
            std::nullopt,
        });
        QTRY_VERIFY_WITH_TIMEOUT(tickCancellationObserved(state), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(cancelCount(state), std::size_t{1}, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), QStringLiteral("offline"), 5000);
        QCOMPARE(controller.errorCode(), QStringLiteral("mediaFileUnavailable"));
        QCOMPARE(playCount(state), std::size_t{1});

        static_cast<void>(controller.requestShutdown());
        QTRY_VERIFY_WITH_TIMEOUT(controller.shutdownComplete(), 5000);
        root.reset();
    }

    void shutdownCancelsGatedTickBeforeSessionDestruction() {
        auto state = std::make_shared<FakeSessionState>();
        state->gateTick = true;
        palmier::windows::PreviewPresentationController controller(
            [state](HWND) { return std::make_unique<FakeSession>(state); },
            nullptr
        );
        QQmlEngine engine;
        auto root = createPreviewWindow(engine, controller);
        QVERIFY(root != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(controller.ready(), 5000);

        controller.replaceProjectPreview(1, availablePreview("media-closing"));
        QTRY_COMPARE_WITH_TIMEOUT(state->tickEntered.available(), 1, 5000);
        QVERIFY(!controller.requestShutdown());
        QTRY_VERIFY_WITH_TIMEOUT(tickCancellationObserved(state), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(controller.shutdownComplete(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(sessionDestroyed(state), 5000);
        QCOMPARE(controller.state(), QStringLiteral("closed"));
        root.reset();
    }

    void warpChildSurfaceLifecycle() {
        palmier::windows::PreviewPresentationController controller(
            palmier::render::D3d11PreviewDriver::warp,
            nullptr
        );
        QQmlEngine engine;
        auto root = createPreviewWindow(engine, controller, 160, 90);
        QVERIFY(root != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(
            controller.ready() || !controller.errorCode().isEmpty(),
            10000
        );
        QVERIFY2(controller.ready(), qPrintable(controller.errorCode()));
        static_cast<void>(controller.requestShutdown());
        QTRY_VERIFY_WITH_TIMEOUT(controller.shutdownComplete(), 10000);
        root.reset();
    }

    void projectPackageDrivesWarpPresentationOrReportsAudioUnavailable() {
        palmier::windows::ProjectLoadCoordinator project;
        palmier::windows::PreviewPresentationController controller(
            palmier::render::D3d11PreviewDriver::warp,
            nullptr
        );
        QObject::connect(
            &project,
            &palmier::windows::ProjectLoadCoordinator::projectCommitted,
            &controller,
            [&project, &controller] {
                controller.replaceProjectPreview(
                    project.committedGeneration(),
                    project.committedPreview()
                );
            }
        );
        QQmlEngine engine;
        auto root = createPreviewWindow(engine, controller, 160, 90);
        QVERIFY(root != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(
            controller.ready() || !controller.errorCode().isEmpty(),
            10000
        );
        QVERIFY2(controller.ready(), qPrintable(controller.errorCode()));

        auto fixture = awaitBackground(createProjectPackageFixture);
        QSignalSpy committed(
            &project,
            &palmier::windows::ProjectLoadCoordinator::projectCommitted
        );
        project.openFolder(QUrl::fromLocalFile(
            QString::fromStdWString(fixture.package.wstring())
        ));
        const bool projectLoaded = committed.count() == 1 || committed.wait(10000);
        const bool previewTerminal = projectLoaded && waitForPreviewTerminal(controller, 20000);
        const auto receipt = controller.latestPlaybackReceipt();
        const bool presentedToEof = previewTerminal
            && controller.state() == QStringLiteral("completed")
            && receipt.state == PreviewPresentationState::completed
            && receipt.generation != 0
            && receipt.renderSerial != 0
            && receipt.presentSerial != 0;
        std::optional<palmier::audio::WasapiEnvironmentProbeResult> environment;
        if (previewTerminal && !presentedToEof) {
            environment = awaitBackground(
                palmier::audio::probeDefaultWasapiRenderEndpoint
            );
        }

        const bool drained = drainPreview(project, controller, root);
        const bool fixtureReleased = releaseFixture(fixture);
        QVERIFY2(projectLoaded, qPrintable(project.errorMessage()));
        QVERIFY2(previewTerminal, "project preview did not reach a terminal state");
        QVERIFY2(drained, "project preview did not drain cleanly");
        QVERIFY2(fixtureReleased, "project preview fixture cleanup failed");
        if (presentedToEof) {
            QVERIFY(receipt.hasTargetTimelineFrame);
            QCOMPARE(receipt.targetTimelineFrame, std::int64_t{0});
            QVERIFY(receipt.hasSourcePresentationTimestamp);
            return;
        }

        QVERIFY(environment.has_value());
        if (
            receipt.stage == palmier::preview::PreviewPresentationStage::startPlayback
            && receipt.failure
                == palmier::preview::PreviewPresentationFailureCode::playbackFailure
            && receipt.audioFailure
                == palmier::media::AudioPlaybackFailureCode::deviceUnavailable
            && receipt.mediaFailureCode == -1
            && environment->status
                == palmier::audio::WasapiProbeStatus::unavailable
            && environment->hresult == receipt.hresult
        ) {
            const auto message = QStringLiteral(
                "Partial: project route reached playback, but WASAPI is unavailable "
                "at %1 (HRESULT 0x%2); no WARP present/EOF proof"
            ).arg(
                QString::fromStdString(environment->stage),
                QString::number(
                    static_cast<qulonglong>(
                        static_cast<std::uint32_t>(environment->hresult)
                    ),
                    16
                ).rightJustified(8, QLatin1Char('0'))
            );
            const auto encoded = message.toUtf8();
            QSKIP(encoded.constData());
        }
        const auto failure = QStringLiteral(
            "project playback failed with state=%1 outcome=%2 stage=%3 failure=%4 "
            "audioFailure=%5 mediaFailure=%6 HRESULT=0x%7 while WASAPI probe "
            "status=%8 stage=%9 HRESULT=0x%10"
        ).arg(QString::number(static_cast<int>(receipt.state)))
        .arg(QString::number(static_cast<int>(receipt.outcome)))
        .arg(QString::number(static_cast<int>(receipt.stage)))
        .arg(QString::number(static_cast<int>(receipt.failure)))
        .arg(QString::number(static_cast<int>(receipt.audioFailure)))
        .arg(QString::number(receipt.mediaFailureCode))
        .arg(QString::number(
                static_cast<qulonglong>(static_cast<std::uint32_t>(receipt.hresult)),
                16
            ).rightJustified(8, QLatin1Char('0')))
        .arg(QString::number(static_cast<int>(environment->status)))
        .arg(QString::fromStdString(environment->stage))
        .arg(QString::number(
                static_cast<qulonglong>(
                    static_cast<std::uint32_t>(environment->hresult)
                ),
                16
            ).rightJustified(8, QLatin1Char('0')))
        .toUtf8();
        QFAIL(failure.constData());
    }

    void qmlCloseWaitsForPreviewSessionRelease() {
        auto state = std::make_shared<FakeSessionState>();
        state->gateFirstResize = true;
        palmier::windows::PreviewPresentationController controller(
            [state](HWND window) {
                {
                    const std::lock_guard lock(state->mutex);
                    state->window = window;
                }
                return std::make_unique<FakeSession>(state);
            },
            nullptr
        );
        palmier::windows::ProjectLoadCoordinator project;
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty(
            QStringLiteral("projectCoordinator"),
            &project
        );
        engine.rootContext()->setContextProperty(
            QStringLiteral("previewCoordinator"),
            &controller
        );
        engine.load(QUrl::fromLocalFile(QStringLiteral(PALMIER_QML_MAIN_FILE)));
        QVERIFY2(!engine.rootObjects().isEmpty(), "Main QML did not load");
        auto* rootWindow = qobject_cast<QQuickWindow*>(engine.rootObjects().front());
        QVERIFY(rootWindow != nullptr);
        QTRY_COMPARE_WITH_TIMEOUT(state->firstResizeEntered.available(), 1, 5000);
        const auto child = reinterpret_cast<HWND>(controller.window()->winId());
        QVERIFY(IsWindow(child));
        rootWindow->close();
        QVERIFY(rootWindow->isVisible());
        QVERIFY(IsWindow(child));
        state->releaseFirstResize.release();
        QTRY_VERIFY_WITH_TIMEOUT(sessionDestroyed(state), 5000);
        {
            const std::lock_guard lock(state->mutex);
            QVERIFY(state->windowAliveAtDestruction);
        }
        QTRY_VERIFY_WITH_TIMEOUT(!rootWindow->isVisible(), 5000);
    }

    void unexpectedTeardownRetiresWindowUntilSessionDestruction() {
        auto state = std::make_shared<FakeSessionState>();
        state->gateFirstResize = true;
        auto controller = std::make_unique<palmier::windows::PreviewPresentationController>(
            [state](HWND) { return std::make_unique<FakeSession>(state); },
            nullptr
        );
        controller->window()->resize(160, 90);
        controller->window()->show();
        QTRY_COMPARE_WITH_TIMEOUT(state->firstResizeEntered.available(), 1, 5000);
        const auto handle = reinterpret_cast<HWND>(controller->window()->winId());
        QVERIFY(IsWindow(handle));
        controller.reset();
        QVERIFY(IsWindow(handle));
        state->releaseFirstResize.release();
        QTRY_VERIFY_WITH_TIMEOUT(sessionDestroyed(state), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!IsWindow(handle), 5000);
    }

    void shutdownDuringReadySignalDoesNotRestoreReadyState() {
        auto state = std::make_shared<FakeSessionState>();
        palmier::windows::PreviewPresentationController controller(
            [state](HWND) { return std::make_unique<FakeSession>(state); },
            nullptr
        );
        QObject::connect(
            &controller,
            &palmier::windows::PreviewPresentationController::readyChanged,
            &controller,
            [&] {
                if (!controller.ready()) return;
                QEventLoop loop;
                QObject::connect(
                    &controller,
                    &palmier::windows::PreviewPresentationController::shutdownReady,
                    &loop,
                    &QEventLoop::quit
                );
                if (!controller.requestShutdown()) loop.exec();
            }
        );
        QQmlEngine engine;
        auto root = createPreviewWindow(engine, controller);
        QVERIFY(root != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(controller.shutdownComplete(), 5000);
        QCOMPARE(controller.state(), QStringLiteral("closed"));
        QVERIFY(!controller.ready());
        root.reset();
    }

    void shutdownFailureSignalReentryStillNotifiesDrain() {
        auto state = std::make_shared<FakeSessionState>();
        state->failClose = true;
        palmier::windows::PreviewPresentationController controller(
            [state](HWND) { return std::make_unique<FakeSession>(state); },
            nullptr
        );
        QObject::connect(
            &controller,
            &palmier::windows::PreviewPresentationController::errorCodeChanged,
            &controller,
            [&] {
                if (!controller.errorCode().isEmpty()) {
                    static_cast<void>(controller.requestShutdown());
                }
            }
        );
        QSignalSpy shutdownReady(
            &controller,
            &palmier::windows::PreviewPresentationController::shutdownReady
        );
        QQmlEngine engine;
        auto root = createPreviewWindow(engine, controller);
        QVERIFY(root != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(controller.ready(), 5000);
        QVERIFY(!controller.requestShutdown());
        QTRY_COMPARE_WITH_TIMEOUT(shutdownReady.count(), 1, 5000);
        QVERIFY(controller.shutdownComplete());
        QCOMPARE(controller.errorCode(), QStringLiteral("previewFailed"));
        QCOMPARE(controller.state(), QStringLiteral("failed"));
        QVERIFY(controller.requestShutdown());
        QCOMPARE(shutdownReady.count(), 1);
        root.reset();
    }
};

}

int main(int argc, char* argv[]) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    QGuiApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    QtPreviewTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "qt_preview_tests.moc"
