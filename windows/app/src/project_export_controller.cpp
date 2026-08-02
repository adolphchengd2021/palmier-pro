#include "palmier/windows/project_export_controller.hpp"

#include <QtConcurrentRun>
#include <QDir>
#include <QFutureWatcher>
#include <QThreadPool>

#include <exception>
#include <optional>
#include <utility>

namespace palmier::windows {
namespace {

struct ExportResult final {
    std::optional<exporting::H264ProjectExportReceipt> receipt;
    QString errorCode;
    QString errorStage;
    QString errorMessage;
};

QThreadPool* projectExportPool() {
    static auto* pool = [] {
        auto* value = new QThreadPool;
        value->setMaxThreadCount(1);
        value->setExpiryTimeout(-1);
        return value;
    }();
    return pool;
}

QString exportFailureCode(exporting::H264ExportFailureCode code) {
    using enum exporting::H264ExportFailureCode;
    switch (code) {
        case invalidRequest: return QStringLiteral("invalidRequest");
        case unsupportedProject: return QStringLiteral("unsupportedProject");
        case resourceLimitExceeded: return QStringLiteral("resourceLimitExceeded");
        case unsupportedSourceTiming: return QStringLiteral("unsupportedSourceTiming");
        case unsupportedEncoder: return QStringLiteral("unsupportedEncoder");
        case mediaUnavailable: return QStringLiteral("mediaUnavailable");
        case sourceEndedEarly: return QStringLiteral("sourceEndedEarly");
        case encodeFailed: return QStringLiteral("encodeFailed");
        case verificationFailed: return QStringLiteral("verificationFailed");
        case destinationExists: return QStringLiteral("destinationExists");
        case stagingFailed: return QStringLiteral("stagingFailed");
        case cleanupFailed: return QStringLiteral("cleanupFailed");
        case installFailed: return QStringLiteral("installFailed");
        case cancelled: return QStringLiteral("cancelled");
    }
    return QStringLiteral("exportFailed");
}

exporting::H264ProjectExportReceipt runProjectExport(
    const project::ProjectDocument& document,
    const exporting::ProjectClipH264ExportRequest& request,
    const exporting::H264ExportLimits& limits,
    std::stop_token cancellation
) {
    return exporting::exportProjectClipH264(document, request, limits, cancellation);
}

}

ProjectExportController::ProjectExportController(
    std::shared_ptr<ProjectRuntimeMailbox> runtimeMailbox,
    QObject* parent
) : ProjectExportController(
    std::move(runtimeMailbox),
    runProjectExport,
    parent
) {}

ProjectExportController::ProjectExportController(
    std::shared_ptr<ProjectRuntimeMailbox> runtimeMailbox,
    ExportOperation operation,
    QObject* parent
) : QObject(parent),
    runtimeMailbox_(std::move(runtimeMailbox)),
    operation_(std::move(operation)) {}

ProjectExportController::~ProjectExportController() {
    stopSource_.request_stop();
}

bool ProjectExportController::hasProject() const noexcept {
    return projectGeneration_ != 0 && !packagePath_.empty();
}
bool ProjectExportController::exporting() const noexcept { return exporting_; }
bool ProjectExportController::canCancel() const noexcept {
    return exporting_ && state_ == QStringLiteral("exporting");
}
QString ProjectExportController::state() const { return state_; }
QString ProjectExportController::errorCode() const { return errorCode_; }
QString ProjectExportController::errorStage() const { return errorStage_; }
QString ProjectExportController::errorMessage() const { return errorMessage_; }
QString ProjectExportController::warningCode() const { return warningCode_; }
QString ProjectExportController::warningMessage() const { return warningMessage_; }
QString ProjectExportController::outputPath() const { return outputPath_; }

void ProjectExportController::activateProject(
    std::filesystem::path packagePath,
    std::uint64_t generation,
    std::uint64_t presentedRevision,
    bool presentationReady
) {
    if (shutdownRequested_ || generation == 0 || packagePath.empty()) return;
    const bool identityChanged = generation != projectGeneration_
        || packagePath != packagePath_;
    const bool availabilityChanged = presentationReady != presentationReady_;
    projectGeneration_ = generation;
    presentedRevision_ = presentedRevision;
    presentationReady_ = presentationReady;
    packagePath_ = std::move(packagePath);
    if (
        exporting_
        && (
            activeJobGeneration_ != generation
            || activeJobRevision_ != presentedRevision
            || activeJobPackagePath_ != packagePath_
            || !presentationReady_
        )
    ) {
        cancel();
    }
    if (!exporting_ && identityChanged) {
        clearTerminalDetails();
        setState(presentationReady_ ? QStringLiteral("idle") : QStringLiteral("pendingPresentation"));
    } else if (
        !exporting_
        && availabilityChanged
        && state_ == QStringLiteral("idle")
        && !presentationReady_
    ) {
        setState(QStringLiteral("pendingPresentation"));
    } else if (
        !exporting_
        && presentationReady_
        && state_ == QStringLiteral("pendingPresentation")
    ) {
        setState(QStringLiteral("idle"));
    } else if (
        !exporting_
        && presentationReady_
        && errorCode_ == QStringLiteral("presentationPending")
    ) {
        clearTerminalDetails();
        setState(QStringLiteral("idle"));
    }
    if (identityChanged || availabilityChanged) emit projectChanged();
}

void ProjectExportController::observeRuntimePublication(
    const ProjectRuntimePublication& publication
) {
    if (shutdownRequested_ || !publication.session) return;
    const bool activeOutputChanged = publication.projectGeneration != activeJobGeneration_
        || publication.session->revision != activeJobRevision_;
    if (exporting_ && activeOutputChanged) {
        cancel();
    } else if (state_ == QStringLiteral("completed") && activeOutputChanged) {
        setWarningCode(QStringLiteral("exportedOlderState"));
        setWarningMessage(QStringLiteral(
            "Export finished from an earlier project revision."
        ));
        setState(QStringLiteral("completedOutdated"));
    }
}

void ProjectExportController::exportSelectedClip(
    const QString& trackId,
    const QString& clipId,
    const QUrl& destination
) {
    if (shutdownRequested_) {
        refuse(
            QStringLiteral("shutdownInProgress"),
            QStringLiteral("Export is unavailable while Palmier Pro is closing.")
        );
        return;
    }
    if (exporting_) {
        refuse(
            QStringLiteral("exportBusy"),
            QStringLiteral("Wait for the current export to finish or cancel it.")
        );
        return;
    }
    if (!hasProject()) {
        setState(QStringLiteral("failed"));
        setErrorCode(QStringLiteral("noActiveProject"));
        setErrorStage(QStringLiteral("admitExport"));
        setErrorMessage(QStringLiteral("Open a project before exporting."));
        emit exportFinished(false);
        return;
    }
    if (!presentationReady_) {
        setState(QStringLiteral("failed"));
        setErrorCode(QStringLiteral("presentationPending"));
        setErrorStage(QStringLiteral("admitExport"));
        setErrorMessage(QStringLiteral("Wait for the timeline to finish refreshing."));
        emit exportFinished(false);
        return;
    }
    if (trackId.isEmpty() || clipId.isEmpty() || !destination.isLocalFile()) {
        setState(QStringLiteral("failed"));
        setErrorCode(QStringLiteral("invalidArguments"));
        setErrorStage(QStringLiteral("admitExport"));
        setErrorMessage(QStringLiteral("Choose a clip and a local MP4 destination."));
        emit exportFinished(false);
        return;
    }
    const auto publication = runtimeMailbox_ ? runtimeMailbox_->latest() : std::nullopt;
    if (
        !publication
        || !publication->session
        || publication->projectGeneration != projectGeneration_
        || publication->session->revision != presentedRevision_
    ) {
        setState(QStringLiteral("failed"));
        setErrorCode(QStringLiteral("staleSelection"));
        setErrorStage(QStringLiteral("admitExport"));
        setErrorMessage(QStringLiteral("Select the clip again after the timeline refreshes."));
        emit exportFinished(false);
        return;
    }

    const auto destinationPath = std::filesystem::path(
        destination.toLocalFile().toStdWString()
    );
    if (destinationPath.empty()) {
        setState(QStringLiteral("failed"));
        setErrorCode(QStringLiteral("invalidArguments"));
        setErrorStage(QStringLiteral("admitExport"));
        setErrorMessage(QStringLiteral("Choose a local MP4 destination."));
        emit exportFinished(false);
        return;
    }

    clearTerminalDetails();
    setExporting(true);
    setState(QStringLiteral("exporting"));
    stopSource_ = std::stop_source{};
    const auto cancellation = stopSource_.get_token();
    const auto jobId = ++activeJobId_;
    activeJobGeneration_ = publication->projectGeneration;
    activeJobRevision_ = publication->session->revision;
    activeJobPackagePath_ = packagePath_;
    const auto snapshot = publication->session;
    const auto operation = operation_;
    exporting::ProjectClipH264ExportRequest request{
        packagePath_,
        trackId.toStdString(),
        clipId.toStdString(),
        destinationPath,
        8'000'000,
        false,
    };
    auto* watcher = new QFutureWatcher<ExportResult>(this);
    connect(watcher, &QFutureWatcher<ExportResult>::finished, this, [
        this,
        watcher,
        jobId
    ] {
        auto result = watcher->future().takeResult();
        watcher->deleteLater();
        if (jobId != activeJobId_) return;
        setExporting(false);
        const bool succeeded = result.receipt.has_value();
        if (succeeded) {
            const auto latest = runtimeMailbox_ ? runtimeMailbox_->latest() : std::nullopt;
            const bool current = latest
                && latest->session
                && latest->projectGeneration == activeJobGeneration_
                && latest->session->revision == activeJobRevision_
                && projectGeneration_ == activeJobGeneration_
                && presentedRevision_ == activeJobRevision_
                && packagePath_ == activeJobPackagePath_
                && presentationReady_;
            setErrorCode({});
            setErrorStage({});
            setErrorMessage({});
            setOutputPath(QDir::toNativeSeparators(
                QString::fromStdWString(result.receipt->destination.native())
            ));
            if (current) {
                setWarningCode({});
                setWarningMessage({});
                setState(QStringLiteral("completed"));
            } else {
                setWarningCode(QStringLiteral("exportedOlderState"));
                setWarningMessage(QStringLiteral(
                    "Export finished from an earlier project revision."
                ));
                setState(QStringLiteral("completedOutdated"));
            }
        } else if (result.errorCode == QStringLiteral("cancelled")) {
            setErrorCode({});
            setErrorStage({});
            setErrorMessage({});
            setWarningCode({});
            setWarningMessage({});
            setState(QStringLiteral("cancelled"));
        } else {
            setWarningCode({});
            setWarningMessage({});
            setErrorCode(std::move(result.errorCode));
            setErrorStage(std::move(result.errorStage));
            setErrorMessage(std::move(result.errorMessage));
            setState(QStringLiteral("failed"));
        }
        emit exportFinished(succeeded);
        if (shutdownRequested_) emit shutdownReady();
    });
    watcher->setFuture(QtConcurrent::run(projectExportPool(), [
        snapshot,
        operation,
        request = std::move(request),
        cancellation
    ] {
        try {
            return ExportResult{
                operation(snapshot->document, request, {}, cancellation),
                {},
                {},
                {},
            };
        } catch (const exporting::H264ExportError& error) {
            return ExportResult{
                std::nullopt,
                exportFailureCode(error.code),
                QString::fromStdString(error.stage),
                QString::fromUtf8(error.what()),
            };
        } catch (const std::exception& error) {
            return ExportResult{
                std::nullopt,
                QStringLiteral("exportFailed"),
                QStringLiteral("runExport"),
                QString::fromUtf8(error.what()),
            };
        } catch (...) {
            return ExportResult{
                std::nullopt,
                QStringLiteral("exportFailed"),
                QStringLiteral("runExport"),
                QStringLiteral("Export failed."),
            };
        }
    }));
}

void ProjectExportController::cancel() {
    if (!exporting_) {
        refuse(
            QStringLiteral("noExportToCancel"),
            QStringLiteral("There is no active export to cancel.")
        );
        return;
    }
    if (state_ == QStringLiteral("cancelling")) return;
    setState(QStringLiteral("cancelling"));
    stopSource_.request_stop();
}

bool ProjectExportController::requestShutdown() {
    if (shutdownRequested_) return !exporting_;
    shutdownRequested_ = true;
    if (exporting_) {
        setState(QStringLiteral("cancelling"));
        stopSource_.request_stop();
    }
    return !exporting_;
}

void ProjectExportController::refuse(QString code, QString message) {
    emit requestRefused(std::move(code), std::move(message));
}

void ProjectExportController::setExporting(bool value) {
    if (exporting_ == value) return;
    const bool oldCanCancel = canCancel();
    exporting_ = value;
    emit exportingChanged();
    if (oldCanCancel != canCancel()) emit canCancelChanged();
}

void ProjectExportController::setState(QString value) {
    if (state_ == value) return;
    const bool oldCanCancel = canCancel();
    state_ = std::move(value);
    emit stateChanged();
    if (oldCanCancel != canCancel()) emit canCancelChanged();
}

void ProjectExportController::setErrorCode(QString value) {
    if (errorCode_ == value) return;
    errorCode_ = std::move(value);
    emit errorCodeChanged();
}

void ProjectExportController::setErrorStage(QString value) {
    if (errorStage_ == value) return;
    errorStage_ = std::move(value);
    emit errorStageChanged();
}

void ProjectExportController::setErrorMessage(QString value) {
    if (errorMessage_ == value) return;
    errorMessage_ = std::move(value);
    emit errorMessageChanged();
}

void ProjectExportController::setWarningCode(QString value) {
    if (warningCode_ == value) return;
    warningCode_ = std::move(value);
    emit warningCodeChanged();
}

void ProjectExportController::setWarningMessage(QString value) {
    if (warningMessage_ == value) return;
    warningMessage_ = std::move(value);
    emit warningMessageChanged();
}

void ProjectExportController::setOutputPath(QString value) {
    if (outputPath_ == value) return;
    outputPath_ = std::move(value);
    emit outputPathChanged();
}

void ProjectExportController::clearTerminalDetails() {
    setErrorCode({});
    setErrorStage({});
    setErrorMessage({});
    setWarningCode({});
    setWarningMessage({});
    setOutputPath({});
}

}
