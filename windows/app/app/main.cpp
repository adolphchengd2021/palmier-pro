#include "palmier/mcp/mcp_http_server.hpp"
#include "palmier/project/project_runtime.hpp"
#include "palmier/windows/project_load_coordinator.hpp"
#include "palmier/windows/preview_presentation_controller.hpp"
#include "palmier/windows/project_runtime_mailbox.hpp"
#include "palmier/windows/project_runtime_projection_bridge.hpp"

#include <QtConcurrentRun>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QEventLoop>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QTimer>
#include <QUuid>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace {

bool drainShutdown(
    palmier::windows::ProjectLoadCoordinator& project,
    palmier::windows::PreviewPresentationController& preview,
    palmier::windows::ProjectRuntimeProjectionBridge& projectionBridge,
    palmier::mcp::HttpServerService& mcpService,
    palmier::project::ProjectRuntime& runtime
) {
    QEventLoop loop;
    bool projectReady{};
    bool previewReady{};
    bool projectionReady{};
    bool mcpReady{};
    const auto finishIfReady = [&] {
        if (projectReady && previewReady && projectionReady && mcpReady) loop.quit();
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
        &projectionBridge,
        &palmier::windows::ProjectRuntimeProjectionBridge::shutdownReady,
        &loop,
        [&] {
            projectionReady = true;
            finishIfReady();
        }
    );
    QFutureWatcher<void> mcpWatcher;
    QObject::connect(&mcpWatcher, &QFutureWatcher<void>::finished, &loop, [&] {
        mcpReady = true;
        finishIfReady();
    });
    mcpService.requestStop();
    mcpWatcher.setFuture(QtConcurrent::run([&mcpService] { mcpService.join(); }));
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
    projectionReady = projectionBridge.requestShutdown();
    if (!projectReady || !previewReady || !projectionReady || !mcpReady) {
        loop.exec(QEventLoop::ExcludeUserInputEvents);
    }
    QFutureWatcher<void> runtimeWatcher;
    QObject::connect(
        &runtimeWatcher,
        &QFutureWatcher<void>::finished,
        &loop,
        &QEventLoop::quit
    );
    runtimeWatcher.setFuture(QtConcurrent::run([&runtime] { runtime.close(); }));
    if (!runtimeWatcher.isFinished()) loop.exec(QEventLoop::ExcludeUserInputEvents);
    const auto mcpStatus = mcpService.status();
    return projectReady && previewReady && projectionReady && mcpReady
        && runtimeWatcher.isFinished() && preview.shutdownComplete()
        && preview.errorCode().isEmpty()
        && mcpStatus.state != palmier::mcp::HttpServerState::failed;
}

std::string newUuid() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
}

}

int main(int argc, char* argv[]) {
    QGuiApplication application(argc, argv);
    const auto quitSmoke = application.arguments().contains(
        QStringLiteral("--quit-smoke-test")
    );
    const auto anySmoke = quitSmoke
        || application.arguments().contains(QStringLiteral("--smoke-test"));
    auto runtimeMailbox = std::make_shared<palmier::windows::ProjectRuntimeMailbox>();
    palmier::project::ProjectRuntime runtime(runtimeMailbox);
    palmier::windows::ProjectRuntimeProjectionBridge projectionBridge(runtimeMailbox);
    palmier::windows::ProjectLoadCoordinator coordinator(
        runtime,
        runtimeMailbox,
        newUuid,
        nullptr
    );
    palmier::mcp::HttpServerService mcpService(
        runtime,
        {.port = anySmoke ? std::uint16_t{0} : std::uint16_t{19789}},
        newUuid
    );
    mcpService.start();
    palmier::windows::PreviewPresentationController previewController(
        quitSmoke
            ? palmier::render::D3d11PreviewDriver::warp
            : palmier::render::D3d11PreviewDriver::hardware,
        nullptr
    );
    QObject::connect(
        &projectionBridge,
        &palmier::windows::ProjectRuntimeProjectionBridge::publicationObserved,
        &coordinator,
        [&] {
            const auto publication = projectionBridge.takeObservedPublication();
            if (publication) coordinator.observeRuntimePublication(*publication);
        }
    );
    QObject::connect(
        &projectionBridge,
        &palmier::windows::ProjectRuntimeProjectionBridge::projectionReady,
        &coordinator,
        [&] {
            auto update = projectionBridge.takeReadyUpdateIfCurrent();
            if (update) coordinator.applyRuntimeProjection(std::move(*update));
        }
    );
    QObject::connect(
        &coordinator,
        &palmier::windows::ProjectLoadCoordinator::projectCommitted,
        &previewController,
        [&coordinator, &previewController] {
            previewController.replaceProjectPreview(
                coordinator.committedGeneration(),
                coordinator.committedRevision(),
                coordinator.committedPreview()
            );
        }
    );
    bool shutdownDraining{};
    bool shutdownDrained{};
    bool shutdownSucceeded{true};
    const auto drainOnce = [&] {
        if (shutdownDraining || shutdownDrained) return;
        shutdownDraining = true;
        shutdownSucceeded = drainShutdown(
            coordinator,
            previewController,
            projectionBridge,
            mcpService,
            runtime
        );
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
    if (engine.rootObjects().isEmpty()) {
        drainOnce();
        return 1;
    }
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
