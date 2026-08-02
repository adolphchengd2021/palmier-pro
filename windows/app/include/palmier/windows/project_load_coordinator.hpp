#pragma once

#include "palmier/project/project_package_service.hpp"
#include "palmier/windows/project_projection_loader.hpp"
#include "palmier/windows/read_only_timeline_model.hpp"
#include "palmier/windows/project_runtime_projection_bridge.hpp"

#include <QObject>
#include <QUrl>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <memory>
#include <stop_token>

namespace palmier::windows {

class ProjectLoadCoordinator final : public QObject {
    Q_OBJECT
    Q_PROPERTY(ReadOnlyTimelineModel* model READ model CONSTANT)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(bool presentationReady READ presentationReady NOTIFY presentationReadyChanged)
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString warningSummary READ warningSummary NOTIFY warningSummaryChanged)
    Q_PROPERTY(QString errorCode READ errorCode NOTIFY errorCodeChanged)
    Q_PROPERTY(QString errorJsonPointer READ errorJsonPointer NOTIFY errorJsonPointerChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)

public:
    using Loader = std::function<ProjectLoadCandidate(
        const std::filesystem::path&,
        std::stop_token
    )>;
    using ResultDeliveryCheckpoint = std::function<void()>;

    explicit ProjectLoadCoordinator(QObject* parent = nullptr);
    ProjectLoadCoordinator(
        project::ProjectRuntime& runtime,
        std::shared_ptr<ProjectRuntimeMailbox> runtimeMailbox,
        project::IdGenerator idGenerator,
        QObject* parent
    );
    ProjectLoadCoordinator(
        project::ProjectRuntime& runtime,
        std::shared_ptr<ProjectRuntimeMailbox> runtimeMailbox,
        project::IdGenerator idGenerator,
        std::shared_ptr<project::ProjectPackageService> packageService,
        QObject* parent
    );
    ProjectLoadCoordinator(
        project::ProjectRuntime& runtime,
        std::shared_ptr<ProjectRuntimeMailbox> runtimeMailbox,
        project::IdGenerator idGenerator,
        Loader loader,
        QObject* parent
    );
    ProjectLoadCoordinator(Loader loader, QObject* parent);
    ProjectLoadCoordinator(
        Loader loader,
        ResultDeliveryCheckpoint resultDeliveryCheckpoint,
        QObject* parent
    );
    ~ProjectLoadCoordinator() override;

    ReadOnlyTimelineModel* model() noexcept;
    bool loading() const noexcept;
    bool presentationReady() const noexcept;
    QString state() const;
    QString warningSummary() const;
    QString errorCode() const;
    QString errorJsonPointer() const;
    QString errorMessage() const;
    std::uint64_t committedGeneration() const noexcept;
    std::uint64_t committedRevision() const noexcept;
    std::filesystem::path committedPackagePath() const;
    ProjectPreviewProjection committedPreview() const;
    void observeRuntimePublication(const ProjectRuntimePublication& publication);
    void applyRuntimeProjection(RuntimeProjectionUpdate update);
    void adoptPackagePath(std::filesystem::path packagePath, std::uint64_t generation);

    Q_INVOKABLE void openFolder(const QUrl& folder);
    Q_INVOKABLE void cancelLoading();
    Q_INVOKABLE bool requestShutdown();

signals:
    void loadingChanged();
    void presentationReadyChanged();
    void stateChanged();
    void warningSummaryChanged();
    void errorCodeChanged();
    void errorJsonPointerChanged();
    void errorMessageChanged();
    void projectCommitted();
    void shutdownReady();

private:
    struct PendingLoad final {
        std::filesystem::path path;
        std::uint64_t generation{};
    };

    void startLoad(PendingLoad request);
    void startPendingLoad();
    void commitProjection(
        ProjectProjection project,
        std::uint64_t generation,
        std::uint64_t revision,
        std::uint64_t publicationToken,
        std::optional<std::filesystem::path> packagePath = {},
        bool preservePresentedFailure = false
    );
    void setLoading(bool value);
    void setPresentationReady(bool value);
    void setState(QString value);
    void setWarningSummary(QString value);
    void setErrorCode(QString value);
    void setErrorJsonPointer(QString value);
    void setErrorMessage(QString value);
    void restoreCommittedPresentation();

    Loader loader_;
    ResultDeliveryCheckpoint resultDeliveryCheckpoint_;
    project::ProjectRuntime* runtime_{};
    std::shared_ptr<ProjectRuntimeMailbox> runtimeMailbox_;
    std::shared_ptr<project::ProjectPackageService> packageService_;
    project::IdGenerator idGenerator_;
    ReadOnlyTimelineModel model_;
    std::uint64_t generation_{};
    std::uint64_t committedGeneration_{};
    std::uint64_t committedRevision_{};
    std::uint64_t committedPublicationToken_{};
    std::filesystem::path committedPackagePath_;
    ProjectPreviewProjection committedPreview_;
    std::stop_source stopSource_;
    std::optional<PendingLoad> pendingLoad_;
    std::optional<RuntimeProjectionUpdate> deferredRuntimeUpdate_;
    bool workerActive_{};
    bool shutdownRequested_{};
    bool loading_{};
    bool presentationReady_{};
    bool preserveLoadFailureOnRuntimeRefresh_{};
    QString state_{QStringLiteral("empty")};
    QString committedState_{QStringLiteral("empty")};
    QString errorCode_;
    QString errorJsonPointer_;
    QString errorMessage_;
    QString warningSummary_;
    QString committedWarningSummary_;
    QString committedErrorCode_;
    QString committedErrorJsonPointer_;
    QString committedErrorMessage_;
};

}
