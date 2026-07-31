#include "palmier/windows/project_projection_loader.hpp"

#include "palmier/project/project_package_reader.hpp"

#include <QVariantMap>

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>

namespace palmier::windows {
namespace {

void checkCancellation(std::stop_token cancellation) {
    if (cancellation.stop_requested()) {
        throw ProjectProjectionError("cancelled", "project projection was cancelled");
    }
}

std::optional<std::int64_t> checkedClipEnd(const palmier::project::Clip& clip) {
    if (
        clip.startFrame < 0
        || clip.durationFrames <= 0
        || clip.startFrame > std::numeric_limits<std::int64_t>::max() - clip.durationFrames
    ) {
        return std::nullopt;
    }
    return clip.startFrame + clip.durationFrames;
}

}

ProjectProjectionError::ProjectProjectionError(std::string codeValue, std::string detail)
    : std::runtime_error(std::move(detail)), code(std::move(codeValue)) {}

ProjectProjection loadProjectProjection(
    const std::filesystem::path& packagePath,
    std::stop_token cancellation
) {
    std::uint64_t generatedId = 0;
    const auto document = palmier::project::readProjectPackage(
        packagePath,
        [&generatedId] { return "qt-shell-generated-" + std::to_string(++generatedId); },
        {.cancellation = cancellation}
    );
    checkCancellation(cancellation);

    return projectDocumentForReadOnlyTimeline(document, cancellation);
}

ProjectProjection projectDocumentForReadOnlyTimeline(
    const palmier::project::ProjectDocument& document,
    std::stop_token cancellation
) {
    checkCancellation(cancellation);

    ProjectProjection result;
    result.activeTimelineId = document.project().activeTimelineId;
    result.diagnosticCount = document.diagnostics().size();
    if (!document.diagnostics().empty()) {
        const auto& diagnostic = document.diagnostics().front();
        result.firstDiagnostic = DiagnosticProjection{diagnostic.code, diagnostic.jsonPointer};
    }
    checkCancellation(cancellation);
    const palmier::project::Timeline* activeTimeline = nullptr;
    for (const auto& timeline : document.project().timelines) {
        checkCancellation(cancellation);
        if (timeline.id.value == result.activeTimelineId) {
            activeTimeline = &timeline;
            break;
        }
    }
    if (activeTimeline == nullptr) {
        throw ProjectProjectionError(
            "activeTimelineUnavailable",
            "active timeline is missing from the read-only project"
        );
    }
    if (activeTimeline->tracks.size() > maximumProjectedTracksPerTimeline) {
        throw ProjectProjectionError(
            "timelineTooDense",
            "timeline exceeds the bounded read-only preview track limit"
        );
    }
    result.timelines.reserve(1);
    {
        const auto& timeline = *activeTimeline;
        TimelineProjection projectedTimeline{
            timeline.id.value,
            timeline.name,
            timeline.fps,
            0,
            {},
        };
        std::int64_t timelineEnd = 0;
        std::size_t projectedClipCount = 0;
        projectedTimeline.tracks.reserve(timeline.tracks.size());
        for (const auto& track : timeline.tracks) {
            checkCancellation(cancellation);
            TrackProjection projectedTrack{track.id.value, track.type, {}, {}};
            projectedTrack.clips.reserve(track.clips.size());
            for (const auto& clip : track.clips) {
                checkCancellation(cancellation);
                const auto clipEnd = checkedClipEnd(clip);
                if (!clipEnd) {
                    ++result.skippedUnsafeClipCount;
                    continue;
                }
                if (
                    projectedTrack.clips.size() >= maximumProjectedClipsPerTrack
                    || projectedClipCount >= maximumProjectedClipsPerTimeline
                ) {
                    throw ProjectProjectionError(
                        "timelineTooDense",
                        "timeline exceeds the bounded read-only preview clip limit"
                    );
                }
                timelineEnd = std::max(timelineEnd, *clipEnd);
                projectedTrack.clips.push_back({
                    clip.id.value,
                    clip.mediaType,
                    clip.startFrame,
                    clip.durationFrames,
                    0,
                    0,
                });
                ++projectedClipCount;
            }
            projectedTimeline.tracks.push_back(std::move(projectedTrack));
        }
        projectedTimeline.durationFrames = timelineEnd;
        for (auto& track : projectedTimeline.tracks) {
            checkCancellation(cancellation);
            track.clipItems.reserve(static_cast<qsizetype>(track.clips.size()));
            for (auto& clip : track.clips) {
                checkCancellation(cancellation);
                if (timelineEnd > 0) {
                    clip.offsetRatio = static_cast<double>(clip.startFrame)
                        / static_cast<double>(timelineEnd);
                    clip.extentRatio = static_cast<double>(clip.durationFrames)
                        / static_cast<double>(timelineEnd);
                }
                QVariantMap item;
                item.insert(QStringLiteral("stableId"), QString::fromStdString(clip.id));
                item.insert(QStringLiteral("mediaType"), QString::fromStdString(clip.mediaType));
                item.insert(QStringLiteral("startFrameText"), QString::number(clip.startFrame));
                item.insert(QStringLiteral("durationFramesText"), QString::number(clip.durationFrames));
                item.insert(QStringLiteral("offsetRatio"), clip.offsetRatio);
                item.insert(QStringLiteral("extentRatio"), clip.extentRatio);
                track.clipItems.push_back(std::move(item));
            }
        }
        result.timelines.push_back(std::move(projectedTimeline));
    }
    checkCancellation(cancellation);
    return result;
}

}
