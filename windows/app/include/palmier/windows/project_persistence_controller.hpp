#pragma once

#include "palmier/project/project_package_service.hpp"
#include "palmier/project/project_recovery_journal.hpp"
#include "palmier/windows/project_runtime_mailbox.hpp"

#include <QObject>
#include <QTimer>
#include <QUrl>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>

namespace palmier::windows {

class ProjectPersistenceController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool dirty READ dirty NOTIFY dirtyChanged)
    Q_PROPERTY(bool saving READ saving NOTIFY savingChanged)
    Q_PROPERTY(bool autosavePending READ autosavePending NOTIFY autosavePendingChanged)
    Q_PROPERTY(bool recoveryPending READ recoveryPending NOTIFY recoveryPendingChanged)
    Q_PROPERTY(bool recoveryWriting READ recoveryWriting NOTIFY recoveryWritingChanged)
    Q_PROPERTY(bool hasProject READ hasProject NOTIFY projectChanged)
    Q_PROPERTY(QString errorCode READ errorCode NOTIFY errorCodeChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    Q_PROPERTY(QString warningCode READ warningCode NOTIFY warningCodeChanged)
    Q_PROPERTY(QString warningMessage READ warningMessage NOTIFY warningMessageChanged)
    Q_PROPERTY(QString recoveryErrorCode READ recoveryErrorCode NOTIFY recoveryErrorCodeChanged)
    Q_PROPERTY(QString recoveryErrorMessage READ recoveryErrorMessage NOTIFY recoveryErrorMessageChanged)

public:
    using Writer = std::function<project::ProjectPackageWriteReceipt(
        project::ProjectRuntime&,
        const std::filesystem::path&,
        std::optional<std::uint64_t>,
        std::stop_token
    )>;
    using RecoveryWriter = std::function<project::ProjectRecoveryJournalWriteReceipt(
        project::ProjectRuntime&,
        const std::filesystem::path&,
        std::optional<std::uint64_t>,
        std::stop_token
    )>;
    using RecoveryRetirer = std::function<bool(
        const std::filesystem::path&,
        std::uint64_t,
        std::uint64_t,
        std::stop_token
    )>;

    ProjectPersistenceController(
        std::shared_ptr<project::ProjectRuntime> runtime,
        std::shared_ptr<ProjectRuntimeMailbox> runtimeMailbox,
        QObject* parent = nullptr
    );
    ProjectPersistenceController(
        std::shared_ptr<project::ProjectRuntime> runtime,
        std::shared_ptr<ProjectRuntimeMailbox> runtimeMailbox,
        std::shared_ptr<project::ProjectPackageService> packageService,
        QObject* parent
    );
    ProjectPersistenceController(
        std::shared_ptr<project::ProjectRuntime> runtime,
        std::shared_ptr<ProjectRuntimeMailbox> runtimeMailbox,
        Writer writer,
        QObject* parent
    );
    ProjectPersistenceController(
        std::shared_ptr<project::ProjectRuntime> runtime,
        std::shared_ptr<ProjectRuntimeMailbox> runtimeMailbox,
        Writer writer,
        std::chrono::milliseconds autosaveDelay,
        QObject* parent
    );
    ProjectPersistenceController(
        std::shared_ptr<project::ProjectRuntime> runtime,
        std::shared_ptr<ProjectRuntimeMailbox> runtimeMailbox,
        Writer writer,
        RecoveryWriter recoveryWriter,
        RecoveryRetirer recoveryRetirer,
        std::chrono::milliseconds autosaveDelay,
        std::chrono::milliseconds recoveryDelay,
        QObject* parent
    );
    ~ProjectPersistenceController() override;

    bool dirty() const noexcept;
    bool saving() const noexcept;
    bool autosavePending() const noexcept;
    bool recoveryPending() const noexcept;
    bool recoveryWriting() const noexcept;
    bool hasProject() const noexcept;
    QString errorCode() const;
    QString errorMessage() const;
    QString warningCode() const;
    QString warningMessage() const;
    QString recoveryErrorCode() const;
    QString recoveryErrorMessage() const;
    bool shutdownAdmitted() const noexcept;

    void activateProject(std::filesystem::path packagePath, std::uint64_t generation);
    void observeRuntimePublication(const ProjectRuntimePublication& publication);

    Q_INVOKABLE void save();
    Q_INVOKABLE void saveAs(const QUrl& destination);
    Q_INVOKABLE void cancelSave();
    Q_INVOKABLE bool requestShutdown(bool discardUnsavedChanges = false);

signals:
    void dirtyChanged();
    void savingChanged();
    void autosavePendingChanged();
    void recoveryPendingChanged();
    void recoveryWritingChanged();
    void projectChanged();
    void errorCodeChanged();
    void errorMessageChanged();
    void warningCodeChanged();
    void warningMessageChanged();
    void recoveryErrorCodeChanged();
    void recoveryErrorMessageChanged();
    void saveFinished(bool succeeded);
    void recoveryFinished(bool succeeded);
    void packageIdentityChanged();
    void shutdownReady();

private:
    void setDirty(bool value);
    void setSaving(bool value);
    void setAutosavePending(bool value);
    void setRecoveryPending(bool value);
    void setRecoveryWriting(bool value);
    void setErrorCode(QString value);
    void setErrorMessage(QString value);
    void setWarningCode(QString value);
    void setWarningMessage(QString value);
    void setRecoveryErrorCode(QString value);
    void setRecoveryErrorMessage(QString value);
    void refreshFromMailbox();
    void configureAutosave(std::chrono::milliseconds delay);
    void configureRecovery(std::chrono::milliseconds delay);
    void configureDefaultRecovery();
    void scheduleAutosave();
    void stopAutosave();
    void scheduleRecovery();
    void stopRecovery(bool cancelActive);
    void startRecovery();
    void startSave(std::optional<std::filesystem::path> destination);

    std::shared_ptr<project::ProjectRuntime> runtime_;
    std::shared_ptr<ProjectRuntimeMailbox> runtimeMailbox_;
    Writer writer_;
    RecoveryWriter recoveryWriter_;
    RecoveryRetirer recoveryRetirer_;
    std::shared_ptr<project::ProjectPackageService> packageService_;
    std::filesystem::path packagePath_;
    std::uint64_t projectGeneration_{};
    std::stop_source stopSource_;
    std::stop_source recoveryStopSource_;
    QTimer autosaveTimer_;
    QTimer recoveryTimer_;
    std::uint64_t lastPublicationToken_{};
    std::uint64_t savePublicationToken_{};
    std::uint64_t recoveryPublicationToken_{};
    bool dirty_{};
    bool saving_{};
    bool autosavePending_{};
    bool recoveryPending_{};
    bool recoveryWriting_{};
    bool recoveryFollowUpRequested_{};
    bool shutdownRequested_{};
    QString errorCode_;
    QString errorMessage_;
    QString warningCode_;
    QString warningMessage_;
    QString recoveryErrorCode_;
    QString recoveryErrorMessage_;
};

}
