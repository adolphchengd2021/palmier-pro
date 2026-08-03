#pragma once

#include "palmier/exporting/project_clip_h264_export_workflow.hpp"
#include "palmier/windows/project_runtime_mailbox.hpp"

#include <QObject>
#include <QString>
#include <QUrl>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <stop_token>

namespace palmier::windows {

class ProjectExportController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool hasProject READ hasProject NOTIFY projectChanged)
    Q_PROPERTY(bool exporting READ exporting NOTIFY exportingChanged)
    Q_PROPERTY(bool canCancel READ canCancel NOTIFY canCancelChanged)
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString errorCode READ errorCode NOTIFY errorCodeChanged)
    Q_PROPERTY(QString errorStage READ errorStage NOTIFY errorStageChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    Q_PROPERTY(QString warningCode READ warningCode NOTIFY warningCodeChanged)
    Q_PROPERTY(QString warningMessage READ warningMessage NOTIFY warningMessageChanged)
    Q_PROPERTY(QString outputPath READ outputPath NOTIFY outputPathChanged)

public:
    using ExportOperation = std::function<exporting::H264ProjectExportReceipt(
        const project::ProjectDocument&,
        const exporting::ProjectTimelineH264ExportRequest&,
        const exporting::H264ExportLimits&,
        std::stop_token
    )>;

    ProjectExportController(
        std::shared_ptr<ProjectRuntimeMailbox> runtimeMailbox,
        QObject* parent = nullptr
    );
    ProjectExportController(
        std::shared_ptr<ProjectRuntimeMailbox> runtimeMailbox,
        ExportOperation operation,
        QObject* parent
    );
    ~ProjectExportController() override;

    bool hasProject() const noexcept;
    bool exporting() const noexcept;
    bool canCancel() const noexcept;
    QString state() const;
    QString errorCode() const;
    QString errorStage() const;
    QString errorMessage() const;
    QString warningCode() const;
    QString warningMessage() const;
    QString outputPath() const;

    void activateProject(
        std::filesystem::path packagePath,
        std::uint64_t generation,
        std::uint64_t presentedRevision,
        bool presentationReady = true
    );
    void observeRuntimePublication(const ProjectRuntimePublication& publication);

    Q_INVOKABLE void exportTimeline(const QUrl& destination);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE bool requestShutdown();

signals:
    void projectChanged();
    void exportingChanged();
    void canCancelChanged();
    void stateChanged();
    void errorCodeChanged();
    void errorStageChanged();
    void errorMessageChanged();
    void warningCodeChanged();
    void warningMessageChanged();
    void outputPathChanged();
    void exportFinished(bool succeeded);
    void requestRefused(QString code, QString message);
    void shutdownReady();

private:
    void refuse(QString code, QString message);
    void setExporting(bool value);
    void setState(QString value);
    void setErrorCode(QString value);
    void setErrorStage(QString value);
    void setErrorMessage(QString value);
    void setWarningCode(QString value);
    void setWarningMessage(QString value);
    void setOutputPath(QString value);
    void clearTerminalDetails();

    std::shared_ptr<ProjectRuntimeMailbox> runtimeMailbox_;
    ExportOperation operation_;
    std::filesystem::path packagePath_;
    std::uint64_t projectGeneration_{};
    std::uint64_t presentedRevision_{};
    std::uint64_t activeJobId_{};
    std::uint64_t activeJobGeneration_{};
    std::uint64_t activeJobRevision_{};
    std::filesystem::path activeJobPackagePath_;
    std::stop_source stopSource_;
    bool exporting_{};
    bool presentationReady_{};
    bool shutdownRequested_{};
    QString state_{QStringLiteral("empty")};
    QString errorCode_;
    QString errorStage_;
    QString errorMessage_;
    QString warningCode_;
    QString warningMessage_;
    QString outputPath_;
};

}
