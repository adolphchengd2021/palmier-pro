#include "palmier/windows/read_only_timeline_model.hpp"

#include <QString>

#include <algorithm>
#include <utility>

namespace palmier::windows {

ReadOnlyTimelineModel::ReadOnlyTimelineModel(QObject* parent) : QAbstractListModel(parent) {}

int ReadOnlyTimelineModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    const auto* timeline = activeTimeline();
    return timeline == nullptr ? 0 : static_cast<int>(timeline->tracks.size());
}
QVariant ReadOnlyTimelineModel::data(const QModelIndex& index, int role) const {
    const auto* timeline = activeTimeline();
    if (
        timeline == nullptr
        || !index.isValid()
        || index.column() != 0
        || index.row() < 0
        || static_cast<std::size_t>(index.row()) >= timeline->tracks.size()
    ) {
        return {};
    }
    const auto& track = timeline->tracks[static_cast<std::size_t>(index.row())];
    switch (role) {
    case StableIdRole: return QString::fromStdString(track.id);
    case TrackTypeRole: return QString::fromStdString(track.type);
    case ClipItemsRole: return track.clipItems;
    default: return {};
    }
}

QHash<int, QByteArray> ReadOnlyTimelineModel::roleNames() const {
    return {
        {StableIdRole, "stableId"},
        {TrackTypeRole, "trackType"},
        {ClipItemsRole, "clipItems"},
    };
}

void ReadOnlyTimelineModel::replace(ProjectProjection&& project) {
    beginResetModel();
    project_ = std::move(project);
    const auto iterator = std::find_if(
        project_.timelines.begin(),
        project_.timelines.end(),
        [this](const TimelineProjection& timeline) {
            return timeline.id == project_.activeTimelineId;
        }
    );
    if (iterator == project_.timelines.end()) {
        activeTimelineIndex_.reset();
    } else {
        activeTimelineIndex_ = static_cast<std::size_t>(
            std::distance(project_.timelines.begin(), iterator)
        );
    }
    endResetModel();
    emit timelineChanged();
}

const ProjectProjection& ReadOnlyTimelineModel::project() const noexcept { return project_; }

QString ReadOnlyTimelineModel::timelineName() const {
    const auto* timeline = activeTimeline();
    return timeline == nullptr ? QString{} : QString::fromStdString(timeline->name);
}

QString ReadOnlyTimelineModel::frameRateText() const {
    const auto* timeline = activeTimeline();
    return timeline == nullptr ? QString{} : QString::number(timeline->fps);
}

QString ReadOnlyTimelineModel::durationFramesText() const {
    const auto* timeline = activeTimeline();
    return timeline == nullptr ? QString{} : QString::number(timeline->durationFrames);
}

const TimelineProjection* ReadOnlyTimelineModel::activeTimeline() const noexcept {
    if (!activeTimelineIndex_ || *activeTimelineIndex_ >= project_.timelines.size()) return nullptr;
    return &project_.timelines[*activeTimelineIndex_];
}

}
