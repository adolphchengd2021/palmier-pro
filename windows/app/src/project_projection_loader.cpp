#include "palmier/windows/project_projection_loader.hpp"

#include "palmier/project/project_package_reader.hpp"
#include "palmier/project/media_manifest_reader.hpp"

#include <QVariantMap>

#include <algorithm>
#include <cmath>
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

bool containsParentTraversal(const std::filesystem::path& path) {
    return std::any_of(path.begin(), path.end(), [](const auto& component) {
        return component == "..";
    });
}

bool isContainedBy(
    const std::filesystem::path& candidate,
    const std::filesystem::path& packageRoot
) {
    const auto relative = candidate.lexically_relative(packageRoot);
    return !relative.empty() && !relative.is_absolute() && !containsParentTraversal(relative);
}

std::filesystem::path pathFromUtf8(const std::string& value) {
    return std::filesystem::path(std::u8string(value.begin(), value.end()));
}

std::optional<std::filesystem::path> resolvedInputPath(
    const palmier::project::MediaManifestSource& source,
    const std::filesystem::path& packagePath,
    std::stop_token cancellation
) {
    checkCancellation(cancellation);
    std::filesystem::path candidate;
    if (source.kind == palmier::project::MediaSourceKind::external) {
        candidate = pathFromUtf8(source.path);
        if (!candidate.is_absolute()) return std::nullopt;
    } else {
        const auto relative = pathFromUtf8(source.path);
        if (
            relative.empty()
            || relative.is_absolute()
            || relative.has_root_name()
            || relative.has_root_directory()
            || containsParentTraversal(relative)
        ) {
            return std::nullopt;
        }
        candidate = packagePath / relative;
    }

    std::error_code error;
    const auto canonicalCandidate = std::filesystem::weakly_canonical(candidate, error);
    checkCancellation(cancellation);
    if (error) return std::nullopt;
    if (source.kind == palmier::project::MediaSourceKind::project) {
        const auto canonicalPackage = std::filesystem::weakly_canonical(packagePath, error);
        checkCancellation(cancellation);
        if (error || !isContainedBy(canonicalCandidate, canonicalPackage)) return std::nullopt;
    }
    const bool isRegularFile = std::filesystem::is_regular_file(canonicalCandidate, error);
    checkCancellation(cancellation);
    if (!isRegularFile || error) {
        return std::nullopt;
    }
    return canonicalCandidate;
}

const palmier::project::MediaManifestEntry* firstManifestEntry(
    const palmier::project::MediaManifest& manifest,
    const std::string& mediaId
) {
    const auto entry = std::find_if(
        manifest.entries.begin(),
        manifest.entries.end(),
        [&mediaId](const auto& value) { return value.id == mediaId; }
    );
    return entry == manifest.entries.end() ? nullptr : &*entry;
}

bool supportedClip(const palmier::project::Clip& clip) {
    return checkedClipEnd(clip).has_value()
        && clip.mediaType == "video"
        && clip.sourceClipType == "video"
        && clip.trimStartFrame == 0
        && clip.trimEndFrame == 0
        && clip.speed == 1
        && std::isfinite(clip.opacity)
        && clip.opacity >= 0
        && clip.opacity <= 1
        && (!clip.blendMode || *clip.blendMode == "normal");
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

    auto result = projectDocumentForReadOnlyTimeline(document, cancellation);
    try {
        const auto manifest = palmier::project::readMediaManifest(
            packagePath,
            {.cancellation = cancellation}
        );
        result.preview = projectPreviewForActiveTimeline(
            document,
            manifest,
            packagePath,
            cancellation
        );
    } catch (const palmier::project::MediaManifestReadError& error) {
        checkCancellation(cancellation);
        result.preview = {
            PreviewCandidateAvailability::offline,
            error.code,
            std::nullopt,
        };
    }
    return result;
}

ProjectPreviewProjection projectPreviewForActiveTimeline(
    const palmier::project::ProjectDocument& document,
    const std::optional<palmier::project::MediaManifest>& manifest,
    const std::filesystem::path& packagePath,
    std::stop_token cancellation
) {
    checkCancellation(cancellation);
    if (!manifest) {
        return {PreviewCandidateAvailability::offline, "mediaManifestMissing", std::nullopt};
    }

    const palmier::project::Timeline* activeTimeline = nullptr;
    for (const auto& timeline : document.project().timelines) {
        checkCancellation(cancellation);
        if (timeline.id.value == document.project().activeTimelineId) {
            activeTimeline = &timeline;
            break;
        }
    }
    if (activeTimeline == nullptr) {
        return {PreviewCandidateAvailability::noCandidate, "activeTimelineUnavailable", std::nullopt};
    }
    if (activeTimeline->id.origin != palmier::project::EntityIdOrigin::persisted) {
        return {PreviewCandidateAvailability::unsupported, "unstableTimelineId", std::nullopt};
    }
    if (
        activeTimeline->fps > static_cast<std::int64_t>(
            std::numeric_limits<std::int32_t>::max()
        )
        || activeTimeline->width > static_cast<std::int64_t>(
            std::numeric_limits<std::uint32_t>::max()
        )
        || activeTimeline->height > static_cast<std::int64_t>(
            std::numeric_limits<std::uint32_t>::max()
        )
    ) {
        return {
            PreviewCandidateAvailability::unsupported,
            "unsupportedTimelineSettings",
            std::nullopt,
        };
    }

    struct OrderedClip final {
        const palmier::project::Track* track;
        const palmier::project::Clip* clip;
        std::size_t trackIndex;
        std::size_t clipIndex;
    };
    std::vector<OrderedClip> ordered;
    for (std::size_t trackIndex = 0; trackIndex < activeTimeline->tracks.size(); ++trackIndex) {
        checkCancellation(cancellation);
        const auto& track = activeTimeline->tracks[trackIndex];
        if (track.hidden || track.type != "video") continue;
        for (std::size_t clipIndex = 0; clipIndex < track.clips.size(); ++clipIndex) {
            checkCancellation(cancellation);
            const auto& clip = track.clips[clipIndex];
            if (clip.mediaType == "video") {
                ordered.push_back({&track, &clip, trackIndex, clipIndex});
            }
        }
    }
    std::stable_sort(ordered.begin(), ordered.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.clip->startFrame != rhs.clip->startFrame) {
            return lhs.clip->startFrame < rhs.clip->startFrame;
        }
        if (lhs.trackIndex != rhs.trackIndex) return lhs.trackIndex < rhs.trackIndex;
        return lhs.clipIndex < rhs.clipIndex;
    });
    if (ordered.empty()) {
        return {PreviewCandidateAvailability::noCandidate, "noVideoCandidate", std::nullopt};
    }

    std::string firstReason = "mediaUnavailable";
    PreviewCandidateAvailability firstAvailability = PreviewCandidateAvailability::offline;
    for (const auto& value : ordered) {
        checkCancellation(cancellation);
        const auto& track = *value.track;
        const auto& clip = *value.clip;
        if (
            track.id.origin != palmier::project::EntityIdOrigin::persisted
            || clip.id.origin != palmier::project::EntityIdOrigin::persisted
        ) {
            firstReason = "unstableCandidateId";
            firstAvailability = PreviewCandidateAvailability::unsupported;
            continue;
        }
        if (!supportedClip(clip)) {
            firstReason = "unsupportedClipTiming";
            firstAvailability = PreviewCandidateAvailability::unsupported;
            continue;
        }
        const auto* entry = firstManifestEntry(*manifest, clip.mediaRef);
        if (entry == nullptr) {
            firstReason = "mediaEntryMissing";
            firstAvailability = PreviewCandidateAvailability::offline;
            continue;
        }
        if (entry->type != "video") {
            firstReason = "mediaTypeMismatch";
            firstAvailability = PreviewCandidateAvailability::unsupported;
            continue;
        }
        if (entry->hasAudio == false) {
            firstReason = "videoOnlyPlaybackUnsupported";
            firstAvailability = PreviewCandidateAvailability::unsupported;
            continue;
        }
        const auto inputPath = resolvedInputPath(entry->source, packagePath, cancellation);
        if (!inputPath) {
            firstReason = "mediaFileUnavailable";
            firstAvailability = PreviewCandidateAvailability::offline;
            continue;
        }
        return {
            PreviewCandidateAvailability::available,
            {},
            PreviewMediaCandidateProjection{
                activeTimeline->id.value,
                track.id.value,
                clip.id.value,
                clip.mediaRef,
                *inputPath,
                clip.startFrame,
                clip.durationFrames,
                activeTimeline->fps,
                activeTimeline->width,
                activeTimeline->height,
                clip.opacity,
                entry->hasAudio,
            },
        };
    }
    return {firstAvailability, std::move(firstReason), std::nullopt};
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
