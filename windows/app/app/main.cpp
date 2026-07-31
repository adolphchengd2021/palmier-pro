#include "palmier/windows/project_load_coordinator.hpp"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QTimer>

int main(int argc, char* argv[]) {
    QGuiApplication application(argc, argv);
    QQmlApplicationEngine engine;
    palmier::windows::ProjectLoadCoordinator coordinator;
    engine.rootContext()->setContextProperty(QStringLiteral("projectCoordinator"), &coordinator);
    engine.loadFromModule("PalmierPro.Windows", "Main");
    if (engine.rootObjects().isEmpty()) return 1;
    if (application.arguments().contains(QStringLiteral("--smoke-test"))) {
        QTimer::singleShot(0, &application, [&application] { application.quit(); });
    }
    return application.exec();
}
