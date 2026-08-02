#pragma once

#include "palmier/project/project_runtime.hpp"
#include "palmier/windows/project_runtime_mailbox.hpp"

#include <QObject>

#include <cstdint>
#include <memory>
#include <stop_token>

namespace palmier::windows {

class ProjectEditingController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY canUndoChanged)
    Q_PROPERTY(QString errorCode READ errorCode NOTIFY errorCodeChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)

public:
    ProjectEditingController(
        std::shared_ptr<project::ProjectRuntime> runtime,
        std::shared_ptr<ProjectRuntimeMailbox> runtimeMailbox,
        QObject* parent = nullptr
    );
    ~ProjectEditingController() override;

    bool busy() const noexcept;
    bool canUndo() const noexcept;
    QString errorCode() const;
    QString errorMessage() const;

    void activateProject(std::uint64_t generation);
    void observeRuntimePublication(const ProjectRuntimePublication& publication);

    Q_INVOKABLE void splitClip(const QString& clipId, const QString& frameText);
    Q_INVOKABLE void undo();
    Q_INVOKABLE bool requestShutdown();

signals:
    void busyChanged();
    void canUndoChanged();
    void errorCodeChanged();
    void errorMessageChanged();
    void operationFinished(bool succeeded);
    void shutdownReady();

private:
    enum class Operation { split, undo };

    void start(Operation operation, QString clipId = {}, std::int64_t atFrame = 0);
    void refreshFromMailbox();
    void setBusy(bool value);
    void setCanUndo(bool value);
    void setErrorCode(QString value);
    void setErrorMessage(QString value);

    std::shared_ptr<project::ProjectRuntime> runtime_;
    std::shared_ptr<ProjectRuntimeMailbox> runtimeMailbox_;
    std::uint64_t projectGeneration_{};
    std::stop_source stopSource_;
    bool busy_{};
    bool canUndo_{};
    bool shutdownRequested_{};
    QString errorCode_;
    QString errorMessage_;
};

}
