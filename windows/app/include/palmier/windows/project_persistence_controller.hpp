#pragma once

#include "palmier/project/windows_project_package_writer.hpp"
#include "palmier/windows/project_runtime_mailbox.hpp"

#include <QObject>

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
        Writer writer,
        QObject* parent
    );
    ~ProjectPersistenceController() override;

    bool dirty() const noexcept;
    bool saving() const noexcept;
    bool hasProject() const noexcept;
    QString errorCode() const;
    QString errorMessage() const;
    QString warningCode() const;
    QString warningMessage() const;
    bool shutdownAdmitted() const noexcept;

    void activateProject(std::filesystem::path packagePath, std::uint64_t generation);
    void observeRuntimePublication(const ProjectRuntimePublication& publication);

    Q_INVOKABLE void save();
    Q_INVOKABLE bool requestShutdown(bool discardUnsavedChanges = false);

signals:
    void dirtyChanged();
    void savingChanged();
    void projectChanged();
    void errorCodeChanged();
    void errorMessageChanged();
    void warningCodeChanged();
    void warningMessageChanged();
    void saveFinished(bool succeeded);
    void shutdownReady();

private:
    void setDirty(bool value);
    void setSaving(bool value);
    void setErrorCode(QString value);
    void setErrorMessage(QString value);
    void setWarningCode(QString value);
    void setWarningMessage(QString value);
    void refreshFromMailbox();

    std::shared_ptr<project::ProjectRuntime> runtime_;
    std::shared_ptr<ProjectRuntimeMailbox> runtimeMailbox_;
    Writer writer_;
    std::filesystem::path packagePath_;
    std::uint64_t projectGeneration_{};
    std::stop_source stopSource_;
    bool dirty_{};
    bool saving_{};
    bool shutdownRequested_{};
    QString errorCode_;
    QString errorMessage_;
    QString warningCode_;
    QString warningMessage_;
};

}
