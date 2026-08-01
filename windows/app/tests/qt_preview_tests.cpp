#include "palmier/windows/preview_presentation_controller.hpp"
#include "palmier/windows/project_load_coordinator.hpp"

#include <QDebug>
#include <QEventLoop>
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

#include <Windows.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using palmier::preview::PreviewPresentationOutcome;
using palmier::preview::PreviewPresentationReceipt;
using palmier::preview::PreviewPresentationState;

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
            "timeline",
            "video-track",
            "clip-" + mediaId,
            std::move(mediaId),
            std::filesystem::path(L"C:\\fixture.mp4"),
            0,
            30,
            30,
            1920,
            1080,
            1,
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
        QCoreApplication::processEvents();
        QCOMPARE(tickCount(state), std::size_t{1});
        controller.replaceProjectPreview(1, availablePreview("media-duplicate"));
        controller.replaceProjectPreview(0, availablePreview("media-invalid"));
        QCoreApplication::processEvents();
        QCOMPARE(playCount(state), std::size_t{1});
        {
            const std::lock_guard lock(state->mutex);
            QCOMPARE(state->candidates.front().mediaId, std::string("media-a"));
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
                state->candidates.front().mediaId,
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
