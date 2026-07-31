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
    : ProjectLoadCoordinator(loadProjectProjection, parent) {}

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

void ProjectLoadCoordinator::openFolder(const QUrl& folder) {
    if (shutdownRequested_) return;
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
            setWarningSummary(committedWarningSummary_);
            setState(committedState_);
            emit shutdownReady();
            return;
        }
        if (requestGeneration == generation_) {
            setLoading(false);
            if (result.cancelled || cancellation.stop_requested()) {
                setWarningSummary(committedWarningSummary_);
                setState(committedState_);
            } else if (result.project) {
                const auto diagnosticCount = result.project->diagnosticCount;
                const auto skippedUnsafeClipCount = result.project->skippedUnsafeClipCount;
                const auto firstDiagnostic = result.project->firstDiagnostic;
                model_.replace(std::move(*result.project));
                setErrorCode({});
                setErrorJsonPointer({});
                setErrorMessage({});
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
                setWarningSummary(committedWarningSummary_);
                setState(committedState_);
            } else {
                setErrorCode(result.errorCode);
                setErrorJsonPointer(result.errorPointer);
                setErrorMessage(result.error);
                setState(QStringLiteral("failed"));
            }
        }
        startPendingLoad();
    });
    watcher->setFuture(QtConcurrent::run(projectReadPool(), [
        loader, path, cancellation, resultDeliveryCheckpoint
    ] {
        try {
            auto project = loader(path, cancellation);
            if (cancellation.stop_requested()) {
                return LoadResult{std::nullopt, QStringLiteral("cancelled"), {}, {}, true};
            }
            if (resultDeliveryCheckpoint) resultDeliveryCheckpoint();
            return LoadResult{std::move(project), {}, {}, {}, false};
        } catch (const ProjectProjectionError& error) {
            return LoadResult{
                std::nullopt,
                QString::fromStdString(error.code),
                {},
                QString::fromUtf8(error.what()),
                cancellation.stop_requested(),
            };
        } catch (const palmier::project::ProjectPackageReadError& error) {
            return LoadResult{
                std::nullopt,
                QString::fromStdString(error.code),
                {},
                QString::fromUtf8(error.what()),
                cancellation.stop_requested(),
            };
        } catch (const palmier::project::ReadError& error) {
            return LoadResult{
                std::nullopt,
                QString::fromStdString(error.code),
                QString::fromStdString(error.jsonPointer),
                QString::fromUtf8(error.what()),
                cancellation.stop_requested(),
            };
        } catch (const palmier::json::Error& error) {
            return LoadResult{
                std::nullopt,
                QStringLiteral("invalidProjectJson"),
                {},
                QString::fromUtf8(error.what()),
                cancellation.stop_requested(),
            };
        } catch (const std::exception& error) {
            return LoadResult{
                std::nullopt,
                QStringLiteral("projectLoadFailed"),
                {},
                QString::fromUtf8(error.what()),
                cancellation.stop_requested(),
            };
        } catch (...) {
            return LoadResult{
                std::nullopt,
                QStringLiteral("projectLoadFailed"),
                {},
                QStringLiteral("Project load failed."),
                cancellation.stop_requested(),
            };
        }
    }));
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
        setWarningSummary(committedWarningSummary_);
        setState(committedState_);
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

}
