#include "palmier/windows/project_runtime_projection_bridge.hpp"

#include <QtConcurrentRun>
#include <QThreadPool>

#include <exception>
#include <utility>

namespace palmier::windows {
namespace {

QThreadPool* runtimeProjectionPool() {
    static auto* pool = [] {
        auto* value = new QThreadPool;
        value->setMaxThreadCount(1);
        value->setExpiryTimeout(-1);
        return value;
    }();
    return pool;
}

RuntimeProjectionUpdate projectRuntimePublication(
    ProjectRuntimePublication publication,
    std::stop_token cancellation,
    const ProjectRuntimeProjectionBridge::ProjectionCheckpoint& projectionCheckpoint
) {
    RuntimeProjectionUpdate result{publication, std::nullopt, {}, {}};
    try {
        if (projectionCheckpoint) projectionCheckpoint(publication);
        if (!publication.session) {
            throw ProjectProjectionError(
                "invalidRuntimePublication",
                "runtime publication has no project session"
            );
        }
        auto project = projectDocumentForReadOnlyTimeline(
            publication.session->document,
            cancellation
        );
        project.preview = {
            PreviewCandidateAvailability::invalidated,
            "runtimeStateChanged",
            std::nullopt,
        };
        result.project = std::move(project);
    } catch (const ProjectProjectionError& error) {
        result.errorCode = QString::fromStdString(error.code);
        result.errorMessage = QString::fromUtf8(error.what());
    } catch (const std::exception& error) {
        result.errorCode = QStringLiteral("runtimeProjectionFailed");
        result.errorMessage = QString::fromUtf8(error.what());
    } catch (...) {
        result.errorCode = QStringLiteral("runtimeProjectionFailed");
        result.errorMessage = QStringLiteral("Runtime projection failed.");
    }
    return result;
}

bool sameContent(
    const ProjectRuntimePublication& lhs,
    const ProjectRuntimePublication& rhs
) {
    return lhs.projectGeneration == rhs.projectGeneration
        && lhs.session
        && rhs.session
        && lhs.session->stateId == rhs.session->stateId;
}

}

ProjectRuntimeProjectionBridge::ProjectRuntimeProjectionBridge(
    std::shared_ptr<ProjectRuntimeMailbox> mailbox,
    QObject* parent
) : ProjectRuntimeProjectionBridge(std::move(mailbox), {}, parent) {}

ProjectRuntimeProjectionBridge::ProjectRuntimeProjectionBridge(
    std::shared_ptr<ProjectRuntimeMailbox> mailbox,
    ProjectionCheckpoint projectionCheckpoint,
    QObject* parent
) : QObject(parent),
    mailbox_(std::move(mailbox)),
    projectionCheckpoint_(std::move(projectionCheckpoint)) {
    pollTimer_.setInterval(25);
    connect(&pollTimer_, &QTimer::timeout, this, &ProjectRuntimeProjectionBridge::pollLatest);
    connect(
        &watcher_,
        &QFutureWatcher<RuntimeProjectionUpdate>::finished,
        this,
        &ProjectRuntimeProjectionBridge::completeProjection
    );
    pollTimer_.start();
}

ProjectRuntimeProjectionBridge::~ProjectRuntimeProjectionBridge() {
    pollTimer_.stop();
    projectionStopSource_.request_stop();
}

std::optional<ProjectRuntimePublication>
ProjectRuntimeProjectionBridge::takeObservedPublication() {
    auto result = std::move(observedPublication_);
    observedPublication_.reset();
    return result;
}

std::optional<RuntimeProjectionUpdate>
ProjectRuntimeProjectionBridge::takeReadyUpdateIfCurrent() {
    if (!readyUpdate_) return std::nullopt;
    const auto latest = mailbox_->latest();
    if (!latest) {
        readyUpdate_.reset();
        return std::nullopt;
    }
    if (latest->token != readyUpdate_->publication.token) {
        if (!sameContent(*latest, readyUpdate_->publication)) {
            readyUpdate_.reset();
            return std::nullopt;
        }
        readyUpdate_->publication = *latest;
    }
    auto result = std::move(readyUpdate_);
    readyUpdate_.reset();
    return result;
}

bool ProjectRuntimeProjectionBridge::requestShutdown() {
    if (shutdownRequested_) return !watcher_.isRunning();
    shutdownRequested_ = true;
    pollTimer_.stop();
    pendingPublication_.reset();
    observedPublication_.reset();
    readyUpdate_.reset();
    projectionStopSource_.request_stop();
    return !watcher_.isRunning();
}

void ProjectRuntimeProjectionBridge::pollLatest() {
    if (shutdownRequested_) return;
    const auto latest = mailbox_->latest();
    if (!latest || latest->token == observedToken_) return;
    observedToken_ = latest->token;
    observedPublication_ = latest;
    emit publicationObserved();
    if (shutdownRequested_) return;
    if (
        !latest->session
        || (scheduledGeneration_ == latest->projectGeneration
            && scheduledStateId_ == latest->session->stateId)
    ) {
        return;
    }
    scheduledGeneration_ = latest->projectGeneration;
    scheduledStateId_ = latest->session->stateId;
    projectionStopSource_.request_stop();
    pendingPublication_ = latest;
    if (!watcher_.isRunning()) startPendingProjection();
}

void ProjectRuntimeProjectionBridge::startPendingProjection() {
    if (shutdownRequested_ || watcher_.isRunning() || !pendingPublication_) return;
    auto publication = std::move(*pendingPublication_);
    pendingPublication_.reset();
    projectionStopSource_ = std::stop_source{};
    const auto cancellation = projectionStopSource_.get_token();
    const auto projectionCheckpoint = projectionCheckpoint_;
    watcher_.setFuture(QtConcurrent::run(
        runtimeProjectionPool(),
        [publication = std::move(publication), cancellation, projectionCheckpoint] {
            return projectRuntimePublication(publication, cancellation, projectionCheckpoint);
        }
    ));
}

void ProjectRuntimeProjectionBridge::completeProjection() {
    auto future = watcher_.future();
    auto result = future.takeResult();
    if (shutdownRequested_) {
        emit shutdownReady();
        return;
    }
    const auto latest = mailbox_->latest();
    if (
        latest
        && (latest->token == result.publication.token
            || sameContent(*latest, result.publication))
    ) {
        result.publication = *latest;
        readyUpdate_ = std::move(result);
        emit projectionReady();
    } else {
        pollLatest();
    }
    startPendingProjection();
}

}
