#include "palmier/windows/project_load_coordinator.hpp"
#include "palmier/windows/preview_presentation_controller.hpp"

#include <QGuiApplication>
#include <QEventLoop>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QTimer>

namespace {

bool drainShutdown(
    palmier::windows::ProjectLoadCoordinator& project,
    palmier::windows::PreviewPresentationController& preview
) {
    QEventLoop loop;
    bool projectReady{};
    bool previewReady{};
    const auto finishIfReady = [&] {
        if (projectReady && previewReady) loop.quit();
    };
    QObject::connect(
        &project,
        &palmier::windows::ProjectLoadCoordinator::shutdownReady,
        &loop,
        [&] {
            projectReady = true;
            finishIfReady();
        }
    );
    QObject::connect(
        &preview,
        &palmier::windows::PreviewPresentationController::shutdownReady,
        &loop,
        [&] {
            previewReady = true;
            finishIfReady();
        }
    );
    projectReady = project.requestShutdown();
    previewReady = preview.requestShutdown();
    if (!projectReady || !previewReady) {
        loop.exec(QEventLoop::ExcludeUserInputEvents);
    }
    return projectReady && previewReady && preview.shutdownComplete()
        && preview.errorCode().isEmpty();
}

}

int main(int argc, char* argv[]) {
    QGuiApplication application(argc, argv);
    const auto quitSmoke = application.arguments().contains(
        QStringLiteral("--quit-smoke-test")
    );
    palmier::windows::ProjectLoadCoordinator coordinator;
    palmier::windows::PreviewPresentationController previewController(
        quitSmoke
            ? palmier::render::D3d11PreviewDriver::warp
            : palmier::render::D3d11PreviewDriver::hardware,
        nullptr
    );
    bool shutdownDraining{};
    bool shutdownDrained{};
    bool shutdownSucceeded{true};
    const auto drainOnce = [&] {
        if (shutdownDraining || shutdownDrained) return;
        shutdownDraining = true;
        shutdownSucceeded = drainShutdown(coordinator, previewController);
        shutdownDrained = true;
        shutdownDraining = false;
    };
    QObject::connect(
        &application,
        &QCoreApplication::aboutToQuit,
        &application,
        drainOnce,
        Qt::DirectConnection
    );
    bool quitSmokeExitScheduled{};
    if (quitSmoke) {
        QObject::connect(
            &previewController,
            &palmier::windows::PreviewPresentationController::readyChanged,
            &application,
            [&] {
                if (!previewController.ready() || quitSmokeExitScheduled) return;
                quitSmokeExitScheduled = true;
                QTimer::singleShot(0, &application, [&] {
                    drainOnce();
                    application.exit(shutdownSucceeded ? 0 : 3);
                });
            }
        );
        QObject::connect(
            &previewController,
            &palmier::windows::PreviewPresentationController::errorCodeChanged,
            &application,
            [&] {
                if (previewController.errorCode().isEmpty() || shutdownDraining
                    || quitSmokeExitScheduled) {
                    return;
                }
                quitSmokeExitScheduled = true;
                QTimer::singleShot(0, &application, [&] {
                    drainOnce();
                    application.exit(2);
                });
            }
        );
    }
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("projectCoordinator"), &coordinator);
    engine.rootContext()->setContextProperty(
        QStringLiteral("previewCoordinator"),
        &previewController
    );
    engine.loadFromModule("PalmierPro.Windows", "Main");
    if (engine.rootObjects().isEmpty()) return 1;
    if (!quitSmoke && application.arguments().contains(QStringLiteral("--smoke-test"))) {
        QTimer::singleShot(0, &application, [] {
            for (auto* window : QGuiApplication::topLevelWindows()) window->close();
        });
    }
    auto result = application.exec();
    drainOnce();
    if (!shutdownSucceeded && result == 0) result = 3;
    return result;
}
