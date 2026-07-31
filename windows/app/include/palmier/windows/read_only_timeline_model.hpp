#pragma once

#include "palmier/windows/project_projection_loader.hpp"

#include <QAbstractListModel>

#include <cstddef>
#include <optional>

namespace palmier::windows {

class ReadOnlyTimelineModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString timelineName READ timelineName NOTIFY timelineChanged)
    Q_PROPERTY(QString frameRateText READ frameRateText NOTIFY timelineChanged)
    Q_PROPERTY(QString durationFramesText READ durationFramesText NOTIFY timelineChanged)

public:
    enum Role {
        StableIdRole = Qt::UserRole + 1,
        TrackTypeRole,
        ClipItemsRole,
    };

    explicit ReadOnlyTimelineModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void replace(ProjectProjection&& project);
    const ProjectProjection& project() const noexcept;
    QString timelineName() const;
    QString frameRateText() const;
    QString durationFramesText() const;

signals:
    void timelineChanged();

private:
    const TimelineProjection* activeTimeline() const noexcept;

    ProjectProjection project_;
    std::optional<std::size_t> activeTimelineIndex_;
};

}
