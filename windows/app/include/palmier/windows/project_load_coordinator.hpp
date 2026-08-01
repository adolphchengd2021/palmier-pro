#pragma once

#include "palmier/windows/project_projection_loader.hpp"
#include "palmier/windows/read_only_timeline_model.hpp"

#include <QObject>
#include <QUrl>

#include <cstdint>
#include <functional>
#include <optional>
#include <stop_token>

namespace palmier::windows {

class ProjectLoadCoordinator final : public QObject {
    Q_OBJECT
    Q_PROPERTY(ReadOnlyTimelineModel* model READ model CONSTANT)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString warningSummary READ warningSummary NOTIFY warningSummaryChanged)
    Q_PROPERTY(QString errorCode READ errorCode NOTIFY errorCodeChanged)
    Q_PROPERTY(QString errorJsonPointer READ errorJsonPointer NOTIFY errorJsonPointerChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)

public:
    using Loader = std::function<ProjectProjection(const std::filesystem::path&, std::stop_token)>;
    using ResultDeliveryCheckpoint = std::function<void()>;

    explicit ProjectLoadCoordinator(QObject* parent = nullptr);
    ProjectLoadCoordinator(Loader loader, QObject* parent);
    ProjectLoadCoordinator(
        Loader loader,
        ResultDeliveryCheckpoint resultDeliveryCheckpoint,
        QObject* parent
    );
    ~ProjectLoadCoordinator() override;

    ReadOnlyTimelineModel* model() noexcept;
    bool loading() const noexcept;
    QString state() const;
    QString warningSummary() const;
    QString errorCode() const;
    QString errorJsonPointer() const;
    QString errorMessage() const;
    std::uint64_t committedGeneration() const noexcept;
    ProjectPreviewProjection committedPreview() const;

    Q_INVOKABLE void openFolder(const QUrl& folder);
    Q_INVOKABLE void cancelLoading();
    Q_INVOKABLE bool requestShutdown();

signals:
    void loadingChanged();
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
    void setLoading(bool value);
    void setState(QString value);
    void setWarningSummary(QString value);
    void setErrorCode(QString value);
    void setErrorJsonPointer(QString value);
    void setErrorMessage(QString value);

    Loader loader_;
    ResultDeliveryCheckpoint resultDeliveryCheckpoint_;
    ReadOnlyTimelineModel model_;
    std::uint64_t generation_{};
    std::uint64_t committedGeneration_{};
    ProjectPreviewProjection committedPreview_;
    std::stop_source stopSource_;
    std::optional<PendingLoad> pendingLoad_;
    bool workerActive_{};
    bool shutdownRequested_{};
    bool loading_{};
    QString state_{QStringLiteral("empty")};
    QString committedState_{QStringLiteral("empty")};
    QString errorCode_;
    QString errorJsonPointer_;
    QString errorMessage_;
    QString warningSummary_;
    QString committedWarningSummary_;
};

}
