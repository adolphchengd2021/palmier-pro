#pragma once

#include "palmier/windows/project_projection_loader.hpp"
#include "palmier/windows/project_runtime_mailbox.hpp"

#include <QFutureWatcher>
#include <QObject>
#include <QString>
#include <QTimer>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>

namespace palmier::windows {

struct RuntimeProjectionUpdate final {
    ProjectRuntimePublication publication;
    std::optional<ProjectProjection> project;
    QString errorCode;
    QString errorMessage;
};

class ProjectRuntimeProjectionBridge final : public QObject {
    Q_OBJECT

public:
    using ProjectionCheckpoint = std::function<void(const ProjectRuntimePublication&)>;

    explicit ProjectRuntimeProjectionBridge(
        std::shared_ptr<ProjectRuntimeMailbox> mailbox,
        QObject* parent = nullptr
    );
    ProjectRuntimeProjectionBridge(
        std::shared_ptr<ProjectRuntimeMailbox> mailbox,
        ProjectionCheckpoint projectionCheckpoint,
        QObject* parent
    );
    ~ProjectRuntimeProjectionBridge() override;

    std::optional<ProjectRuntimePublication> takeObservedPublication();
    std::optional<RuntimeProjectionUpdate> takeReadyUpdateIfCurrent();
    bool requestShutdown();

signals:
    void publicationObserved();
    void projectionReady();
    void shutdownReady();

private:
    void pollLatest();
    void startPendingProjection();
    void completeProjection();

    std::shared_ptr<ProjectRuntimeMailbox> mailbox_;
    ProjectionCheckpoint projectionCheckpoint_;
    QTimer pollTimer_;
    QFutureWatcher<RuntimeProjectionUpdate> watcher_;
    std::stop_source projectionStopSource_;
    std::optional<ProjectRuntimePublication> pendingPublication_;
    std::optional<ProjectRuntimePublication> observedPublication_;
    std::optional<RuntimeProjectionUpdate> readyUpdate_;
    std::uint64_t observedToken_{};
    std::optional<std::uint64_t> scheduledGeneration_;
    std::optional<std::uint64_t> scheduledStateId_;
    bool shutdownRequested_{};
};

}
