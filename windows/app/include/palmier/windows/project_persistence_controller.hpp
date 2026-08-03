#pragma once

#include "palmier/project/project_package_service.hpp"
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
    Q_PROPERTY(bool hasProject READ hasProject NOTIFY projectChanged)
    Q_PROPERTY(QString errorCode READ errorCode NOTIFY errorCodeChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    Q_PROPERTY(QString warningCode READ warningCode NOTIFY warningCodeChanged)
    Q_PROPERTY(QString warningMessage READ warningMessage NOTIFY warningMessageChanged)

public:
    using Writer = std::function<project::ProjectPackageWriteReceipt(
        project::ProjectRuntime&,
        const std::filesystem::path&,
        std::optional<std::uint64_t>,
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
    ~ProjectPersistenceController() override;

    bool dirty() const noexcept;
    bool saving() const noexcept;
    bool autosavePending() const noexcept;
    bool hasProject() const noexcept;
    QString errorCode() const;
    QString errorMessage() const;
    QString warningCode() const;
    QString warningMessage() const;
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
    void projectChanged();
    void errorCodeChanged();
    void errorMessageChanged();
    void warningCodeChanged();
    void warningMessageChanged();
    void saveFinished(bool succeeded);
    void packageIdentityChanged();
    void shutdownReady();

private:
    void setDirty(bool value);
    void setSaving(bool value);
    void setAutosavePending(bool value);
    void setErrorCode(QString value);
    void setErrorMessage(QString value);
    void setWarningCode(QString value);
    void setWarningMessage(QString value);
    void refreshFromMailbox();
    void configureAutosave(std::chrono::milliseconds delay);
    void scheduleAutosave();
    void stopAutosave();
    void startSave(std::optional<std::filesystem::path> destination);

    std::shared_ptr<project::ProjectRuntime> runtime_;
    std::shared_ptr<ProjectRuntimeMailbox> runtimeMailbox_;
    Writer writer_;
    std::shared_ptr<project::ProjectPackageService> packageService_;
    std::filesystem::path packagePath_;
    std::uint64_t projectGeneration_{};
    std::stop_source stopSource_;
    QTimer autosaveTimer_;
    std::uint64_t lastPublicationToken_{};
    std::uint64_t savePublicationToken_{};
    bool dirty_{};
    bool saving_{};
    bool autosavePending_{};
    bool shutdownRequested_{};
    QString errorCode_;
    QString errorMessage_;
    QString warningCode_;
    QString warningMessage_;
};

}
