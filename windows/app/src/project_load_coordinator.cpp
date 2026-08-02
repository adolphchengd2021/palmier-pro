#include "palmier/windows/project_load_coordinator.hpp"

#include "palmier/project/project_package_reader.hpp"

#include <QtConcurrentRun>
#include <QFutureWatcher>
#include <QThreadPool>

#include <exception>
#include <optional>
#include <utility>

namespace palmier::windows {
namespace {

struct LoadResult final {
    std::optional<ProjectProjection> project;
    std::uint64_t revision{};
    std::uint64_t publicationToken{};
    QString errorCode;
    QString errorPointer;
    QString error;
    bool cancelled{};
};

QThreadPool* projectReadPool() {
    // Process-lifetime serial pool avoids UI-thread teardown waits and unbounded project reads.
    static auto* pool = [] {
        auto* value = new QThreadPool;
        value->setMaxThreadCount(1);
        value->setExpiryTimeout(-1);
        return value;
    }();
    return pool;
}

}

ProjectLoadCoordinator::ProjectLoadCoordinator(QObject* parent)
    : ProjectLoadCoordinator(loadProjectCandidate, parent) {}

ProjectLoadCoordinator::ProjectLoadCoordinator(
    project::ProjectRuntime& runtime,
    std::shared_ptr<ProjectRuntimeMailbox> runtimeMailbox,
    project::IdGenerator idGenerator,
    QObject* parent
) : ProjectLoadCoordinator(
    runtime,
    std::move(runtimeMailbox),
    std::move(idGenerator),
    loadProjectCandidate,
    parent
) {}

ProjectLoadCoordinator::ProjectLoadCoordinator(
    project::ProjectRuntime& runtime,
    std::shared_ptr<ProjectRuntimeMailbox> runtimeMailbox,
    project::IdGenerator idGenerator,
    Loader loader,
    QObject* parent
) : ProjectLoadCoordinator(std::move(loader), {}, parent) {
    runtime_ = &runtime;
    runtimeMailbox_ = std::move(runtimeMailbox);
    idGenerator_ = std::move(idGenerator);
}

ProjectLoadCoordinator::ProjectLoadCoordinator(Loader loader, QObject* parent)
    : ProjectLoadCoordinator(std::move(loader), {}, parent) {}

ProjectLoadCoordinator::ProjectLoadCoordinator(
    Loader loader,
    ResultDeliveryCheckpoint resultDeliveryCheckpoint,
    QObject* parent
) : QObject(parent),
    loader_(std::move(loader)),
    resultDeliveryCheckpoint_(std::move(resultDeliveryCheckpoint)),
    model_(this) {}

ProjectLoadCoordinator::~ProjectLoadCoordinator() {
    stopSource_.request_stop();
    pendingLoad_.reset();
}

ReadOnlyTimelineModel* ProjectLoadCoordinator::model() noexcept { return &model_; }
bool ProjectLoadCoordinator::loading() const noexcept { return loading_; }
QString ProjectLoadCoordinator::errorMessage() const { return errorMessage_; }
QString ProjectLoadCoordinator::errorCode() const { return errorCode_; }
QString ProjectLoadCoordinator::errorJsonPointer() const { return errorJsonPointer_; }
QString ProjectLoadCoordinator::state() const { return state_; }
QString ProjectLoadCoordinator::warningSummary() const { return warningSummary_; }
std::uint64_t ProjectLoadCoordinator::committedGeneration() const noexcept {
    return committedGeneration_;
}
std::uint64_t ProjectLoadCoordinator::committedRevision() const noexcept {
    return committedRevision_;
}
ProjectPreviewProjection ProjectLoadCoordinator::committedPreview() const {
    return committedPreview_;
}

void ProjectLoadCoordinator::openFolder(const QUrl& folder) {
    if (shutdownRequested_) return;
    preserveLoadFailureOnRuntimeRefresh_ = false;
    stopSource_.request_stop();
    const auto requestGeneration = ++generation_;
    if (!folder.isLocalFile()) {
        pendingLoad_.reset();
        setLoading(false);
        setErrorCode(QStringLiteral("invalidProjectUrl"));
        setErrorJsonPointer({});
        setErrorMessage(QStringLiteral("Choose a local .palmier project folder."));
        setState(QStringLiteral("failed"));
        return;
    }
    setErrorCode({});
    setErrorJsonPointer({});
    setErrorMessage({});
    setLoading(true);
    setState(QStringLiteral("loading"));
    PendingLoad request{
        std::filesystem::path(folder.toLocalFile().toStdWString()),
        requestGeneration,
    };
    if (workerActive_) {
        pendingLoad_ = std::move(request);
        return;
    }
    startLoad(std::move(request));
}

void ProjectLoadCoordinator::startLoad(PendingLoad request) {
    workerActive_ = true;
    stopSource_ = std::stop_source{};
    const auto cancellation = stopSource_.get_token();
    const auto requestGeneration = request.generation;
    const auto path = std::move(request.path);
    const auto loader = loader_;
    auto* const runtime = runtime_;
    const auto runtimeMailbox = runtimeMailbox_;
    const auto idGenerator = idGenerator_;
    const auto resultDeliveryCheckpoint = resultDeliveryCheckpoint_;
    auto* watcher = new QFutureWatcher<LoadResult>(this);
    connect(watcher, &QFutureWatcher<LoadResult>::finished, this, [
        this, watcher, requestGeneration, cancellation
    ] {
        auto future = watcher->future();
        auto result = future.takeResult();
        watcher->deleteLater();
        workerActive_ = false;
        if (shutdownRequested_) {
            pendingLoad_.reset();
            setLoading(false);
            restoreCommittedPresentation();
            emit shutdownReady();
            return;
        }
        if (requestGeneration == generation_) {
            setLoading(false);
            if (
                result.cancelled
                || (cancellation.stop_requested() && runtime_ == nullptr)
            ) {
                restoreCommittedPresentation();
                if (deferredRuntimeUpdate_) {
                    auto deferred = std::move(*deferredRuntimeUpdate_);
                    deferredRuntimeUpdate_.reset();
                    applyRuntimeProjection(std::move(deferred));
                }
            } else if (result.project) {
                bool publicationIsCurrent = runtime_ == nullptr;
                if (runtime_ != nullptr && result.publicationToken != 0 && runtimeMailbox_) {
                    const auto latest = runtimeMailbox_->latest();
                    publicationIsCurrent = latest
                        && latest->token == result.publicationToken;
                }
                if (publicationIsCurrent) {
                    commitProjection(
                        std::move(*result.project),
                        requestGeneration,
                        result.revision,
                        result.publicationToken
                    );
                }
            } else {
                setErrorCode(result.errorCode);
                setErrorJsonPointer(result.errorPointer);
                setErrorMessage(result.error);
                setState(QStringLiteral("failed"));
                const auto latest = runtimeMailbox_ ? runtimeMailbox_->latest() : std::nullopt;
                preserveLoadFailureOnRuntimeRefresh_ = latest
                    && latest->projectGeneration != requestGeneration;
                if (deferredRuntimeUpdate_) {
                    auto deferred = std::move(*deferredRuntimeUpdate_);
                    deferredRuntimeUpdate_.reset();
                    applyRuntimeProjection(std::move(deferred));
                }
            }
        }
        startPendingLoad();
    });
    watcher->setFuture(QtConcurrent::run(projectReadPool(), [
        loader,
        path,
        requestGeneration,
        cancellation,
        resultDeliveryCheckpoint,
        runtime,
        runtimeMailbox,
        idGenerator
    ] {
        try {
            auto candidate = loader(path, cancellation);
            if (cancellation.stop_requested()) {
                return LoadResult{
                    std::nullopt,
                    0,
                    0,
                    QStringLiteral("cancelled"),
                    {},
                    {},
                    true,
                };
            }
            if (resultDeliveryCheckpoint) resultDeliveryCheckpoint();
            if (cancellation.stop_requested()) {
                return LoadResult{
                    std::nullopt,
                    0,
                    0,
                    QStringLiteral("cancelled"),
                    {},
                    {},
                    true,
                };
            }
            std::uint64_t revision = 0;
            std::uint64_t publicationToken = 0;
            if (runtime != nullptr) {
                if (!candidate.document || !runtimeMailbox || !idGenerator) {
                    throw project::ProjectRuntimeError(
                        "invalidRuntimeLoadCandidate",
                        "runtime project load candidate is incomplete"
                    );
                }
                auto state = runtime->install(
                    std::move(*candidate.document),
                    requestGeneration,
                    idGenerator,
                    false,
                    cancellation
                );
                revision = state.session->revision;
                const auto publication = runtimeMailbox->latest();
                if (
                    publication
                    && publication->projectGeneration == requestGeneration
                    && publication->session.get() == state.session.get()
                ) {
                    publicationToken = publication->token;
                }
            }
            return LoadResult{
                std::move(candidate.projection),
                revision,
                publicationToken,
                {},
                {},
                {},
                false,
            };
        } catch (const ProjectProjectionError& error) {
            return LoadResult{
                std::nullopt,
                0,
                0,
                QString::fromStdString(error.code),
                {},
                QString::fromUtf8(error.what()),
                cancellation.stop_requested(),
            };
        } catch (const palmier::project::ProjectPackageReadError& error) {
            return LoadResult{
                std::nullopt,
                0,
                0,
                QString::fromStdString(error.code),
                {},
                QString::fromUtf8(error.what()),
                cancellation.stop_requested(),
            };
        } catch (const palmier::project::ReadError& error) {
            return LoadResult{
                std::nullopt,
                0,
                0,
                QString::fromStdString(error.code),
                QString::fromStdString(error.jsonPointer),
                QString::fromUtf8(error.what()),
                cancellation.stop_requested(),
            };
        } catch (const palmier::json::Error& error) {
            return LoadResult{
                std::nullopt,
                0,
                0,
                QStringLiteral("invalidProjectJson"),
                {},
                QString::fromUtf8(error.what()),
                cancellation.stop_requested(),
            };
        } catch (const palmier::project::ProjectRuntimeError& error) {
            return LoadResult{
                std::nullopt,
                0,
                0,
                QString::fromStdString(error.code),
                {},
                QString::fromUtf8(error.what()),
                cancellation.stop_requested(),
            };
        } catch (const std::exception& error) {
            return LoadResult{
                std::nullopt,
                0,
                0,
                QStringLiteral("projectLoadFailed"),
                {},
                QString::fromUtf8(error.what()),
                cancellation.stop_requested(),
            };
        } catch (...) {
            return LoadResult{
                std::nullopt,
                0,
                0,
                QStringLiteral("projectLoadFailed"),
                {},
                QStringLiteral("Project load failed."),
                cancellation.stop_requested(),
            };
        }
    }));
}

void ProjectLoadCoordinator::commitProjection(
    ProjectProjection project,
    std::uint64_t generation,
    std::uint64_t revision,
    std::uint64_t publicationToken,
    bool preservePresentedFailure
) {
    const auto diagnosticCount = project.diagnosticCount;
    const auto skippedUnsafeClipCount = project.skippedUnsafeClipCount;
    const auto firstDiagnostic = project.firstDiagnostic;
    auto committedPreview = project.preview;
    model_.replace(std::move(project));
    committedErrorCode_.clear();
    committedErrorJsonPointer_.clear();
    committedErrorMessage_.clear();
    if (!firstDiagnostic) {
        committedWarningSummary_.clear();
        committedState_ = QStringLiteral("loaded");
    } else {
        committedWarningSummary_ = QStringLiteral("%1 warning(s): %2 at %3")
            .arg(static_cast<qulonglong>(diagnosticCount))
            .arg(QString::fromStdString(firstDiagnostic->code))
            .arg(QString::fromStdString(firstDiagnostic->jsonPointer));
        if (skippedUnsafeClipCount > 0) {
            committedWarningSummary_ += QStringLiteral("; %1 unsafe clip(s) omitted")
                .arg(static_cast<qulonglong>(skippedUnsafeClipCount));
        }
        committedState_ = QStringLiteral("loadedWithWarnings");
    }
    if (!preservePresentedFailure) {
        setErrorCode({});
        setErrorJsonPointer({});
        setErrorMessage({});
        setWarningSummary(committedWarningSummary_);
        setState(committedState_);
    }
    committedGeneration_ = generation;
    committedRevision_ = revision;
    committedPublicationToken_ = publicationToken;
    if (
        deferredRuntimeUpdate_
        && deferredRuntimeUpdate_->publication.token <= publicationToken
    ) {
        deferredRuntimeUpdate_.reset();
    }
    committedPreview_ = std::move(committedPreview);
    emit projectCommitted();
}

void ProjectLoadCoordinator::observeRuntimePublication(
    const ProjectRuntimePublication& publication
) {
    if (
        shutdownRequested_
        || !publication.session
        || publication.projectGeneration < committedGeneration_
        || publication.projectGeneration > generation_
    ) {
        return;
    }
    if (runtimeMailbox_) {
        const auto latest = runtimeMailbox_->latest();
        if (!latest || latest->token != publication.token) return;
    }
    const bool acceptedGeneration = publication.projectGeneration == committedGeneration_
        || publication.projectGeneration == generation_
        || (!loading_ && !workerActive_ && !pendingLoad_);
    if (!acceptedGeneration) return;
    const bool contentChanged = publication.projectGeneration != committedGeneration_
        || publication.session->revision > committedRevision_;
    if (!contentChanged) return;
    committedGeneration_ = publication.projectGeneration;
    committedRevision_ = publication.session->revision;
    committedPreview_ = {
        PreviewCandidateAvailability::invalidated,
        "runtimeStateChanged",
        std::nullopt,
    };
    emit projectCommitted();
}

void ProjectLoadCoordinator::applyRuntimeProjection(RuntimeProjectionUpdate update) {
    if (shutdownRequested_ || !runtimeMailbox_) return;
    const auto latest = runtimeMailbox_->latest();
    if (!latest || latest->token != update.publication.token) return;
    if (
        update.publication.projectGeneration < committedGeneration_
        || update.publication.projectGeneration > generation_
        || update.publication.token <= committedPublicationToken_
    ) {
        return;
    }
    const bool acceptedGeneration = update.publication.projectGeneration
            == committedGeneration_
        || update.publication.projectGeneration == generation_
        || (!loading_ && !workerActive_ && !pendingLoad_);
    if (!acceptedGeneration) {
        if (
            !deferredRuntimeUpdate_
            || deferredRuntimeUpdate_->publication.token < update.publication.token
        ) {
            deferredRuntimeUpdate_ = std::move(update);
        }
        return;
    }
    if (!update.project) {
        model_.replace(ProjectProjection{});
        committedGeneration_ = update.publication.projectGeneration;
        committedRevision_ = update.publication.session
            ? update.publication.session->revision
            : 0;
        committedPublicationToken_ = update.publication.token;
        committedPreview_ = {
            PreviewCandidateAvailability::invalidated,
            "runtimeProjectionFailed",
            std::nullopt,
        };
        committedWarningSummary_.clear();
        committedState_ = QStringLiteral("failed");
        committedErrorCode_ = update.errorCode;
        committedErrorJsonPointer_.clear();
        committedErrorMessage_ = update.errorMessage;
        if (!preserveLoadFailureOnRuntimeRefresh_) {
            setWarningSummary({});
            setErrorCode(update.errorCode);
            setErrorJsonPointer({});
            setErrorMessage(update.errorMessage);
            setState(committedState_);
        }
        emit projectCommitted();
        return;
    }
    const auto revision = update.publication.session
        ? update.publication.session->revision
        : 0;
    commitProjection(
        std::move(*update.project),
        update.publication.projectGeneration,
        revision,
        update.publication.token,
        preserveLoadFailureOnRuntimeRefresh_
    );
}

void ProjectLoadCoordinator::startPendingLoad() {
    if (pendingLoad_) {
        auto request = std::move(*pendingLoad_);
        pendingLoad_.reset();
        startLoad(std::move(request));
        return;
    }
    if (state_ == QStringLiteral("cancelling")) {
        setLoading(false);
        restoreCommittedPresentation();
    }
}

void ProjectLoadCoordinator::cancelLoading() {
    if (!loading_) return;
    stopSource_.request_stop();
    pendingLoad_.reset();
    setErrorCode({});
    setErrorJsonPointer({});
    setErrorMessage({});
    setState(QStringLiteral("cancelling"));
}

bool ProjectLoadCoordinator::requestShutdown() {
    shutdownRequested_ = true;
    pendingLoad_.reset();
    deferredRuntimeUpdate_.reset();
    ++generation_;
    stopSource_.request_stop();
    if (!workerActive_) return true;
    setLoading(true);
    setErrorCode({});
    setErrorJsonPointer({});
    setErrorMessage({});
    setState(QStringLiteral("cancelling"));
    return false;
}

void ProjectLoadCoordinator::setState(QString value) {
    if (state_ == value) return;
    state_ = std::move(value);
    emit stateChanged();
}

void ProjectLoadCoordinator::setWarningSummary(QString value) {
    if (warningSummary_ == value) return;
    warningSummary_ = std::move(value);
    emit warningSummaryChanged();
}

void ProjectLoadCoordinator::setLoading(bool value) {
    if (loading_ == value) return;
    loading_ = value;
    emit loadingChanged();
}

void ProjectLoadCoordinator::setErrorMessage(QString value) {
    if (errorMessage_ == value) return;
    errorMessage_ = std::move(value);
    emit errorMessageChanged();
}

void ProjectLoadCoordinator::setErrorCode(QString value) {
    if (errorCode_ == value) return;
    errorCode_ = std::move(value);
    emit errorCodeChanged();
}

void ProjectLoadCoordinator::setErrorJsonPointer(QString value) {
    if (errorJsonPointer_ == value) return;
    errorJsonPointer_ = std::move(value);
    emit errorJsonPointerChanged();
}

void ProjectLoadCoordinator::restoreCommittedPresentation() {
    setWarningSummary(committedWarningSummary_);
    setErrorCode(committedErrorCode_);
    setErrorJsonPointer(committedErrorJsonPointer_);
    setErrorMessage(committedErrorMessage_);
    setState(committedState_);
}

}
