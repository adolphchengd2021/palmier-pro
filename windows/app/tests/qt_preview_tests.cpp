#include "palmier/windows/preview_presentation_controller.hpp"
#include "palmier/windows/project_load_coordinator.hpp"

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
#include <memory>
#include <mutex>
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

struct FakeSessionState final {
    std::mutex mutex;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> sizes;
    std::vector<std::thread::id> threads;
    QSemaphore firstResizeEntered;
    QSemaphore releaseFirstResize;
    bool gateFirstResize{};
    bool destroyed{};
    HWND window{};
    bool windowAliveAtDestruction{};
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

    PreviewPresentationReceipt close() override {
        const std::lock_guard lock(state_->mutex);
        state_->threads.push_back(std::this_thread::get_id());
        return closedReceipt();
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
    return std::unique_ptr<QObject>(component.create());
}

std::size_t resizeCount(const std::shared_ptr<FakeSessionState>& state) {
    const std::lock_guard lock(state->mutex);
    return state->sizes.size();
}

bool sessionDestroyed(const std::shared_ptr<FakeSessionState>& state) {
    const std::lock_guard lock(state->mutex);
    return state->destroyed;
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
