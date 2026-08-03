#include "palmier/windows/project_persistence_controller.hpp"

#include <QtConcurrentRun>
#include <QFutureWatcher>
#include <QThreadPool>

#include <algorithm>
#include <exception>
#include <limits>
#include <utility>

namespace palmier::windows {
namespace {

constexpr auto defaultAutosaveDelay = std::chrono::seconds{30};
constexpr auto defaultRecoveryDelay = std::chrono::seconds{2};

struct SaveResult final {
    std::optional<project::ProjectPackageWriteReceipt> receipt;
    std::optional<project::ProjectPackageIdentity> adoptedIdentity;
    QString errorCode;
    QString errorMessage;
    QString recoveryWarningCode;
    QString recoveryWarningMessage;
};

struct RecoveryResult final {
    std::optional<project::ProjectRecoveryJournalWriteReceipt> receipt;
    QString errorCode;
    QString errorMessage;
    bool cancelled{};
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
) : QObject(parent),
    runtime_(std::move(runtime)),
    runtimeMailbox_(std::move(runtimeMailbox)),
    writer_(project::writeProjectPackage) {
    configureAutosave(defaultAutosaveDelay);
    configureDefaultRecovery();
}

ProjectPersistenceController::ProjectPersistenceController(
    std::shared_ptr<project::ProjectRuntime> runtime,
    std::shared_ptr<ProjectRuntimeMailbox> runtimeMailbox,
    std::shared_ptr<project::ProjectPackageService> packageService,
    QObject* parent
) : QObject(parent),
    runtime_(std::move(runtime)),
    runtimeMailbox_(std::move(runtimeMailbox)),
    packageService_(std::move(packageService)) {
    configureAutosave(defaultAutosaveDelay);
    configureDefaultRecovery();
}

ProjectPersistenceController::ProjectPersistenceController(
    std::shared_ptr<project::ProjectRuntime> runtime,
    std::shared_ptr<ProjectRuntimeMailbox> runtimeMailbox,
    Writer writer,
    QObject* parent
) : QObject(parent),
    runtime_(std::move(runtime)),
    runtimeMailbox_(std::move(runtimeMailbox)),
    writer_(std::move(writer)) {
    configureAutosave(defaultAutosaveDelay);
}

ProjectPersistenceController::ProjectPersistenceController(
    std::shared_ptr<project::ProjectRuntime> runtime,
    std::shared_ptr<ProjectRuntimeMailbox> runtimeMailbox,
    Writer writer,
    std::chrono::milliseconds autosaveDelay,
    QObject* parent
) : QObject(parent),
    runtime_(std::move(runtime)),
    runtimeMailbox_(std::move(runtimeMailbox)),
    writer_(std::move(writer)) {
    configureAutosave(autosaveDelay);
}

ProjectPersistenceController::ProjectPersistenceController(
    std::shared_ptr<project::ProjectRuntime> runtime,
    std::shared_ptr<ProjectRuntimeMailbox> runtimeMailbox,
    Writer writer,
    RecoveryWriter recoveryWriter,
    RecoveryRetirer recoveryRetirer,
    std::chrono::milliseconds autosaveDelay,
    std::chrono::milliseconds recoveryDelay,
    QObject* parent
) : QObject(parent),
    runtime_(std::move(runtime)),
    runtimeMailbox_(std::move(runtimeMailbox)),
    writer_(std::move(writer)),
    recoveryWriter_(std::move(recoveryWriter)),
    recoveryRetirer_(std::move(recoveryRetirer)) {
    configureAutosave(autosaveDelay);
    configureRecovery(recoveryDelay);
}

ProjectPersistenceController::~ProjectPersistenceController() {
    stopAutosave();
    stopRecovery(true);
    stopSource_.request_stop();
}

bool ProjectPersistenceController::dirty() const noexcept { return dirty_; }
bool ProjectPersistenceController::saving() const noexcept { return saving_; }
bool ProjectPersistenceController::autosavePending() const noexcept {
    return autosavePending_;
}
bool ProjectPersistenceController::recoveryPending() const noexcept {
    return recoveryPending_;
}
bool ProjectPersistenceController::recoveryWriting() const noexcept {
    return recoveryWriting_;
}
bool ProjectPersistenceController::hasProject() const noexcept {
    return projectGeneration_ != 0 && !packagePath_.empty();
}
QString ProjectPersistenceController::errorCode() const { return errorCode_; }
QString ProjectPersistenceController::errorMessage() const { return errorMessage_; }
QString ProjectPersistenceController::warningCode() const { return warningCode_; }
QString ProjectPersistenceController::warningMessage() const { return warningMessage_; }
QString ProjectPersistenceController::recoveryErrorCode() const {
    return recoveryErrorCode_;
}
QString ProjectPersistenceController::recoveryErrorMessage() const {
    return recoveryErrorMessage_;
}
bool ProjectPersistenceController::shutdownAdmitted() const noexcept {
    return shutdownRequested_;
}

void ProjectPersistenceController::activateProject(
    std::filesystem::path packagePath,
    std::uint64_t generation
) {
    if (shutdownRequested_ || generation == 0 || packagePath.empty()) return;
    const bool changed = generation != projectGeneration_ || packagePath != packagePath_;
    if (changed) {
        stopAutosave();
        stopRecovery(true);
        lastPublicationToken_ = 0;
        setRecoveryErrorCode({});
        setRecoveryErrorMessage({});
    }
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
        || publication.token <= lastPublicationToken_
    ) {
        return;
    }
    lastPublicationToken_ = publication.token;
    setDirty(publication.session->dirty());
    if (dirty_) {
        scheduleRecovery();
        scheduleAutosave();
    }
}

void ProjectPersistenceController::save() {
    startSave(std::nullopt);
}

void ProjectPersistenceController::saveAs(const QUrl& destination) {
    if (!destination.isLocalFile()) {
        setErrorCode(QStringLiteral("invalidPackagePath"));
        setErrorMessage(QStringLiteral("Choose a local .palmier destination."));
        emit saveFinished(false);
        return;
    }
    startSave(std::filesystem::path(destination.toLocalFile().toStdWString()));
}

void ProjectPersistenceController::cancelSave() {
    if (saving_) stopSource_.request_stop();
}

void ProjectPersistenceController::startSave(
    std::optional<std::filesystem::path> destination
) {
    if (shutdownRequested_ || saving_) return;
    if (!hasProject()) {
        setErrorCode(QStringLiteral("noProject"));
        setErrorMessage(QStringLiteral("Open a project before saving."));
        emit saveFinished(false);
        return;
    }
    stopAutosave();
    stopRecovery(true);
    if (!destination && !dirty_) {
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
    savePublicationToken_ = lastPublicationToken_;
    stopSource_ = std::stop_source{};
    const auto cancellation = stopSource_.get_token();
    const auto generation = projectGeneration_;
    const auto path = packagePath_;
    const auto runtime = runtime_;
    const auto writer = writer_;
    const auto packageService = packageService_;
    auto* watcher = new QFutureWatcher<SaveResult>(this);
    connect(watcher, &QFutureWatcher<SaveResult>::finished, this, [this, watcher] {
        const auto admittedPublicationToken = savePublicationToken_;
        auto result = watcher->future().takeResult();
        watcher->deleteLater();
        setSaving(false);
        refreshFromMailbox();
        const bool succeeded = result.receipt.has_value();
        if (succeeded) {
            setErrorCode({});
            setErrorMessage({});
            setRecoveryErrorCode(result.recoveryWarningCode);
            setRecoveryErrorMessage(result.recoveryWarningMessage);
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
            if (result.adoptedIdentity) {
                packagePath_ = result.adoptedIdentity->path;
                projectGeneration_ = result.adoptedIdentity->projectGeneration;
                emit projectChanged();
                emit packageIdentityChanged();
            }
        } else {
            setErrorCode(std::move(result.errorCode));
            setErrorMessage(std::move(result.errorMessage));
        }
        emit saveFinished(succeeded);
        if (shutdownRequested_ && !recoveryWriting_) {
            emit shutdownReady();
        } else if (dirty_) {
            if (!succeeded || lastPublicationToken_ > admittedPublicationToken) {
                scheduleRecovery();
            }
            if (lastPublicationToken_ > admittedPublicationToken) {
                scheduleAutosave();
            }
        }
    });
    watcher->setFuture(QtConcurrent::run(projectSavePool(), [
        runtime,
        writer,
        packageService,
        recoveryRetirer = recoveryRetirer_,
        path,
        generation,
        destination = std::move(destination),
        cancellation
    ] {
        const auto retireRecovery = [&](SaveResult result) {
            if (!result.receipt || !recoveryRetirer) return result;
            try {
                static_cast<void>(recoveryRetirer(
                    path,
                    generation,
                    result.receipt->revision,
                    {}
                ));
            } catch (const project::ProjectRecoveryJournalError& error) {
                result.recoveryWarningCode = QStringLiteral("recoveryRetirementFailed");
                result.recoveryWarningMessage = QString::fromUtf8(error.what());
            } catch (const std::exception& error) {
                result.recoveryWarningCode = QStringLiteral("recoveryRetirementFailed");
                result.recoveryWarningMessage = QString::fromUtf8(error.what());
            } catch (...) {
                result.recoveryWarningCode = QStringLiteral("recoveryRetirementFailed");
                result.recoveryWarningMessage = QStringLiteral(
                    "The saved project recovery state could not be retired."
                );
            }
            return result;
        };
        try {
            if (packageService) {
                if (destination) {
                    auto result = packageService->saveAs(
                        *runtime,
                        *destination,
                        generation,
                        cancellation
                    );
                    return retireRecovery(SaveResult{
                        std::move(result.write),
                        std::move(result.identity),
                        {},
                        {},
                    });
                }
                return retireRecovery(SaveResult{
                    packageService->save(*runtime, generation, cancellation),
                    std::nullopt,
                    {},
                    {},
                });
            }
            if (destination) {
                return SaveResult{
                    std::nullopt,
                    std::nullopt,
                    QStringLiteral("saveAsUnavailable"),
                    QStringLiteral("Save As is unavailable for this project session."),
                };
            }
            return retireRecovery(SaveResult{
                writer(*runtime, path, generation, cancellation),
                std::nullopt,
                {},
                {},
            });
        } catch (const project::ProjectPackageServiceError& error) {
            return SaveResult{
                std::nullopt,
                std::nullopt,
                QString::fromStdString(error.code),
                QString::fromUtf8(error.what()),
            };
        } catch (const project::ProjectPackageWriteError& error) {
            return SaveResult{
                std::nullopt,
                std::nullopt,
                QString::fromStdString(error.code),
                QString::fromUtf8(error.what()),
            };
        } catch (const project::ProjectRuntimeError& error) {
            return SaveResult{
                std::nullopt,
                std::nullopt,
                QString::fromStdString(error.code),
                QString::fromUtf8(error.what()),
            };
        } catch (const std::exception& error) {
            return SaveResult{
                std::nullopt,
                std::nullopt,
                QStringLiteral("projectSaveFailed"),
                QString::fromUtf8(error.what()),
            };
        } catch (...) {
            return SaveResult{
                std::nullopt,
                std::nullopt,
                QStringLiteral("projectSaveFailed"),
                QStringLiteral("Project save failed."),
            };
        }
    }));
}

bool ProjectPersistenceController::requestShutdown(bool discardUnsavedChanges) {
    if (shutdownRequested_) return !saving_ && !recoveryWriting_;
    refreshFromMailbox();
    if (dirty_ && !discardUnsavedChanges) {
        setErrorCode(QStringLiteral("unsavedChanges"));
        setErrorMessage(QStringLiteral("Save or discard changes before closing."));
        return false;
    }
    shutdownRequested_ = true;
    stopAutosave();
    stopRecovery(true);
    return !saving_ && !recoveryWriting_;
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
    if (publication->token > lastPublicationToken_) {
        lastPublicationToken_ = publication->token;
    }
    setDirty(publication->session->dirty());
}

void ProjectPersistenceController::setDirty(bool value) {
    if (dirty_ == value) return;
    dirty_ = value;
    if (!dirty_) {
        stopAutosave();
        stopRecovery(true);
    }
    emit dirtyChanged();
}

void ProjectPersistenceController::setSaving(bool value) {
    if (saving_ == value) return;
    saving_ = value;
    emit savingChanged();
}

void ProjectPersistenceController::setAutosavePending(bool value) {
    if (autosavePending_ == value) return;
    autosavePending_ = value;
    emit autosavePendingChanged();
}

void ProjectPersistenceController::setRecoveryPending(bool value) {
    if (recoveryPending_ == value) return;
    recoveryPending_ = value;
    emit recoveryPendingChanged();
}

void ProjectPersistenceController::setRecoveryWriting(bool value) {
    if (recoveryWriting_ == value) return;
    recoveryWriting_ = value;
    emit recoveryWritingChanged();
}

void ProjectPersistenceController::configureAutosave(
    std::chrono::milliseconds delay
) {
    const auto bounded = std::clamp<std::int64_t>(
        delay.count(),
        0,
        (std::numeric_limits<int>::max)()
    );
    autosaveTimer_.setSingleShot(true);
    autosaveTimer_.setInterval(static_cast<int>(bounded));
    connect(&autosaveTimer_, &QTimer::timeout, this, [this] {
        setAutosavePending(false);
        startSave(std::nullopt);
    });
}

void ProjectPersistenceController::configureRecovery(
    std::chrono::milliseconds delay
) {
    const auto bounded = std::clamp<std::int64_t>(
        delay.count(),
        0,
        (std::numeric_limits<int>::max)()
    );
    recoveryTimer_.setSingleShot(true);
    recoveryTimer_.setInterval(static_cast<int>(bounded));
    connect(&recoveryTimer_, &QTimer::timeout, this, [this] {
        setRecoveryPending(false);
        startRecovery();
    });
}

void ProjectPersistenceController::configureDefaultRecovery() {
    auto journal = std::make_shared<project::ProjectRecoveryJournal>();
    recoveryWriter_ = [journal](
        project::ProjectRuntime& runtime,
        const std::filesystem::path& packagePath,
        std::optional<std::uint64_t> generation,
        std::stop_token cancellation
    ) {
        return journal->write(runtime, packagePath, generation, cancellation);
    };
    recoveryRetirer_ = [journal](
        const std::filesystem::path& packagePath,
        std::uint64_t generation,
        std::uint64_t revision,
        std::stop_token cancellation
    ) {
        return journal->retire(packagePath, generation, revision, cancellation);
    };
    configureRecovery(defaultRecoveryDelay);
}

void ProjectPersistenceController::scheduleAutosave() {
    if (shutdownRequested_ || saving_ || !hasProject() || !dirty_) return;
    const bool wasPending = autosavePending_;
    autosaveTimer_.start();
    if (!wasPending) setAutosavePending(true);
}

void ProjectPersistenceController::stopAutosave() {
    if (autosaveTimer_.isActive()) autosaveTimer_.stop();
    setAutosavePending(false);
}

void ProjectPersistenceController::scheduleRecovery() {
    if (
        !recoveryWriter_
        || shutdownRequested_
        || saving_
        || !hasProject()
        || !dirty_
    ) {
        return;
    }
    if (recoveryWriting_) {
        recoveryFollowUpRequested_ = true;
        return;
    }
    const bool wasPending = recoveryPending_;
    recoveryTimer_.start();
    if (!wasPending) setRecoveryPending(true);
}

void ProjectPersistenceController::stopRecovery(bool cancelActive) {
    if (recoveryTimer_.isActive()) recoveryTimer_.stop();
    setRecoveryPending(false);
    recoveryFollowUpRequested_ = false;
    if (cancelActive && recoveryWriting_) recoveryStopSource_.request_stop();
}

void ProjectPersistenceController::startRecovery() {
    if (
        !recoveryWriter_
        || shutdownRequested_
        || saving_
        || recoveryWriting_
        || !hasProject()
        || !dirty_
    ) {
        return;
    }
    setRecoveryErrorCode({});
    setRecoveryErrorMessage({});
    recoveryFollowUpRequested_ = false;
    setRecoveryWriting(true);
    recoveryPublicationToken_ = lastPublicationToken_;
    recoveryStopSource_ = std::stop_source{};
    const auto cancellation = recoveryStopSource_.get_token();
    const auto runtime = runtime_;
    const auto writer = recoveryWriter_;
    const auto path = packagePath_;
    const auto generation = projectGeneration_;
    auto* watcher = new QFutureWatcher<RecoveryResult>(this);
    connect(watcher, &QFutureWatcher<RecoveryResult>::finished, this, [this, watcher] {
        const auto admittedPublicationToken = recoveryPublicationToken_;
        auto result = watcher->future().takeResult();
        watcher->deleteLater();
        setRecoveryWriting(false);
        const bool followUpRequested = recoveryFollowUpRequested_;
        recoveryFollowUpRequested_ = false;
        refreshFromMailbox();
        const bool succeeded = result.receipt.has_value();
        if (succeeded) {
            setRecoveryErrorCode({});
            setRecoveryErrorMessage({});
        } else if (!result.cancelled) {
            setRecoveryErrorCode(std::move(result.errorCode));
            setRecoveryErrorMessage(std::move(result.errorMessage));
        }
        emit recoveryFinished(succeeded);
        if (shutdownRequested_ && !saving_) {
            emit shutdownReady();
        } else if (
            dirty_
            && (followUpRequested || lastPublicationToken_ > admittedPublicationToken)
        ) {
            scheduleRecovery();
        }
    });
    watcher->setFuture(QtConcurrent::run(projectSavePool(), [
        runtime,
        writer,
        path,
        generation,
        cancellation
    ] {
        try {
            return RecoveryResult{
                writer(*runtime, path, generation, cancellation),
                {},
                {},
                false,
            };
        } catch (const project::ProjectRecoveryJournalError& error) {
            const bool cancelled = error.code == "cancelled" || error.code == "projectClean";
            return RecoveryResult{
                std::nullopt,
                QString::fromStdString(error.code),
                QString::fromUtf8(error.what()),
                cancelled,
            };
        } catch (const project::ProjectRuntimeError& error) {
            return RecoveryResult{
                std::nullopt,
                QString::fromStdString(error.code),
                QString::fromUtf8(error.what()),
                cancellation.stop_requested(),
            };
        } catch (const std::exception& error) {
            return RecoveryResult{
                std::nullopt,
                QStringLiteral("recoveryWriteFailed"),
                QString::fromUtf8(error.what()),
                cancellation.stop_requested(),
            };
        } catch (...) {
            return RecoveryResult{
                std::nullopt,
                QStringLiteral("recoveryWriteFailed"),
                QStringLiteral("Project recovery state could not be written."),
                cancellation.stop_requested(),
            };
        }
    }));
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

void ProjectPersistenceController::setRecoveryErrorCode(QString value) {
    if (recoveryErrorCode_ == value) return;
    recoveryErrorCode_ = std::move(value);
    emit recoveryErrorCodeChanged();
}

void ProjectPersistenceController::setRecoveryErrorMessage(QString value) {
    if (recoveryErrorMessage_ == value) return;
    recoveryErrorMessage_ = std::move(value);
    emit recoveryErrorMessageChanged();
}

}
