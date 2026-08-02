#include "palmier/windows/project_persistence_controller.hpp"

#include <QtConcurrentRun>
#include <QFutureWatcher>
#include <QThreadPool>

#include <exception>
#include <utility>

namespace palmier::windows {
namespace {

struct SaveResult final {
    std::optional<project::ProjectPackageWriteReceipt> receipt;
    QString errorCode;
    QString errorMessage;
};

QThreadPool* projectSavePool() {
    static auto* pool = [] {
        auto* value = new QThreadPool;
        value->setMaxThreadCount(1);
        value->setExpiryTimeout(-1);
        return value;
    }();
    return pool;
}

}

ProjectPersistenceController::ProjectPersistenceController(
    std::shared_ptr<project::ProjectRuntime> runtime,
    std::shared_ptr<ProjectRuntimeMailbox> runtimeMailbox,
    QObject* parent
) : ProjectPersistenceController(
    runtime,
    std::move(runtimeMailbox),
    project::writeProjectPackage,
    parent
) {}

ProjectPersistenceController::ProjectPersistenceController(
    std::shared_ptr<project::ProjectRuntime> runtime,
    std::shared_ptr<ProjectRuntimeMailbox> runtimeMailbox,
    Writer writer,
    QObject* parent
) : QObject(parent),
    runtime_(std::move(runtime)),
    runtimeMailbox_(std::move(runtimeMailbox)),
    writer_(std::move(writer)) {}

ProjectPersistenceController::~ProjectPersistenceController() {
    stopSource_.request_stop();
}

bool ProjectPersistenceController::dirty() const noexcept { return dirty_; }
bool ProjectPersistenceController::saving() const noexcept { return saving_; }
bool ProjectPersistenceController::hasProject() const noexcept {
    return projectGeneration_ != 0 && !packagePath_.empty();
}
QString ProjectPersistenceController::errorCode() const { return errorCode_; }
QString ProjectPersistenceController::errorMessage() const { return errorMessage_; }
QString ProjectPersistenceController::warningCode() const { return warningCode_; }
QString ProjectPersistenceController::warningMessage() const { return warningMessage_; }
bool ProjectPersistenceController::shutdownAdmitted() const noexcept {
    return shutdownRequested_;
}

void ProjectPersistenceController::activateProject(
    std::filesystem::path packagePath,
    std::uint64_t generation
) {
    if (shutdownRequested_ || generation == 0 || packagePath.empty()) return;
    const bool changed = generation != projectGeneration_ || packagePath != packagePath_;
    projectGeneration_ = generation;
    packagePath_ = std::move(packagePath);
    refreshFromMailbox();
    if (changed) emit projectChanged();
}

void ProjectPersistenceController::observeRuntimePublication(
    const ProjectRuntimePublication& publication
) {
    if (
        shutdownRequested_
        || !publication.session
        || publication.projectGeneration != projectGeneration_
    ) {
        return;
    }
    setDirty(publication.session->dirty());
}

void ProjectPersistenceController::save() {
    if (shutdownRequested_ || saving_) return;
    if (!hasProject()) {
        setErrorCode(QStringLiteral("noProject"));
        setErrorMessage(QStringLiteral("Open a project before saving."));
        emit saveFinished(false);
        return;
    }
    if (!dirty_) {
        setErrorCode({});
        setErrorMessage({});
        setWarningCode({});
        setWarningMessage({});
        emit saveFinished(true);
        return;
    }
    setErrorCode({});
    setErrorMessage({});
    setWarningCode({});
    setWarningMessage({});
    setSaving(true);
    stopSource_ = std::stop_source{};
    const auto cancellation = stopSource_.get_token();
    const auto generation = projectGeneration_;
    const auto path = packagePath_;
    const auto runtime = runtime_;
    const auto writer = writer_;
    auto* watcher = new QFutureWatcher<SaveResult>(this);
    connect(watcher, &QFutureWatcher<SaveResult>::finished, this, [this, watcher] {
        auto result = watcher->future().takeResult();
        watcher->deleteLater();
        setSaving(false);
        refreshFromMailbox();
        const bool succeeded = result.receipt.has_value();
        if (succeeded) {
            setErrorCode({});
            setErrorMessage({});
            if (!result.receipt->runtimeAcknowledged) {
                setWarningCode(QStringLiteral("saveCommittedRuntimeNotAcknowledged"));
                setWarningMessage(QStringLiteral(
                    "The project file was saved, but the active editor state could not be confirmed."
                ));
            } else if (result.receipt->runtimeDirty) {
                setWarningCode(QStringLiteral("saveCommittedNewerChangesRemain"));
                setWarningMessage(QStringLiteral(
                    "The project file was saved, but newer edits still need saving."
                ));
            } else {
                setWarningCode({});
                setWarningMessage({});
            }
        } else {
            setErrorCode(std::move(result.errorCode));
            setErrorMessage(std::move(result.errorMessage));
        }
        emit saveFinished(succeeded);
        if (shutdownRequested_) emit shutdownReady();
    });
    watcher->setFuture(QtConcurrent::run(projectSavePool(), [
        runtime,
        writer,
        path,
        generation,
        cancellation
    ] {
        try {
            return SaveResult{
                writer(*runtime, path, generation, cancellation),
                {},
                {},
            };
        } catch (const project::ProjectPackageWriteError& error) {
            return SaveResult{
                std::nullopt,
                QString::fromStdString(error.code),
                QString::fromUtf8(error.what()),
            };
        } catch (const project::ProjectRuntimeError& error) {
            return SaveResult{
                std::nullopt,
                QString::fromStdString(error.code),
                QString::fromUtf8(error.what()),
            };
        } catch (const std::exception& error) {
            return SaveResult{
                std::nullopt,
                QStringLiteral("projectSaveFailed"),
                QString::fromUtf8(error.what()),
            };
        } catch (...) {
            return SaveResult{
                std::nullopt,
                QStringLiteral("projectSaveFailed"),
                QStringLiteral("Project save failed."),
            };
        }
    }));
}

bool ProjectPersistenceController::requestShutdown(bool discardUnsavedChanges) {
    if (shutdownRequested_) return !saving_;
    if (dirty_ && !discardUnsavedChanges) {
        setErrorCode(QStringLiteral("unsavedChanges"));
        setErrorMessage(QStringLiteral("Save or discard changes before closing."));
        return false;
    }
    shutdownRequested_ = true;
    return !saving_;
}

void ProjectPersistenceController::refreshFromMailbox() {
    if (!runtimeMailbox_ || projectGeneration_ == 0) {
        setDirty(false);
        return;
    }
    const auto publication = runtimeMailbox_->latest();
    if (
        !publication
        || !publication->session
        || publication->projectGeneration != projectGeneration_
    ) {
        setDirty(false);
        return;
    }
    setDirty(publication->session->dirty());
}

void ProjectPersistenceController::setDirty(bool value) {
    if (dirty_ == value) return;
    dirty_ = value;
    emit dirtyChanged();
}

void ProjectPersistenceController::setSaving(bool value) {
    if (saving_ == value) return;
    saving_ = value;
    emit savingChanged();
}

void ProjectPersistenceController::setErrorCode(QString value) {
    if (errorCode_ == value) return;
    errorCode_ = std::move(value);
    emit errorCodeChanged();
}

void ProjectPersistenceController::setErrorMessage(QString value) {
    if (errorMessage_ == value) return;
    errorMessage_ = std::move(value);
    emit errorMessageChanged();
}

void ProjectPersistenceController::setWarningCode(QString value) {
    if (warningCode_ == value) return;
    warningCode_ = std::move(value);
    emit warningCodeChanged();
}

void ProjectPersistenceController::setWarningMessage(QString value) {
    if (warningMessage_ == value) return;
    warningMessage_ = std::move(value);
    emit warningMessageChanged();
}

}
