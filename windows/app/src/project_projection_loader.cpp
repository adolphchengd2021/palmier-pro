#include "palmier/windows/project_projection_loader.hpp"

#include "palmier/project/project_package_reader.hpp"
#include "palmier/project/media_manifest_reader.hpp"
#include "palmier/project/project_media_resolver.hpp"
#include "palmier/project_render/project_render_compiler.hpp"

#include <QVariantMap>

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

const project_render::StaticVideoLayer*
PreviewMediaCandidateProjection::firstRenderLayer() const noexcept {
    return renderTimeline.segments.empty() ? nullptr : &renderTimeline.segments.front();
}

const PreviewMediaSourceProjection* PreviewMediaCandidateProjection::sourceForClip(
    std::string_view clipId
) const noexcept {
    for (const auto& source : sources) {
        if (source.clipId == clipId) return &source;
    }
    return nullptr;
}

ProjectProjectionError::ProjectProjectionError(std::string codeValue, std::string detail)
    : std::runtime_error(std::move(detail)), code(std::move(codeValue)) {}

ProjectLoadCandidate::ProjectLoadCandidate(ProjectProjection value)
    : projection(std::move(value)) {}

ProjectLoadCandidate::ProjectLoadCandidate(
    palmier::project::ProjectDocument documentValue,
    ProjectProjection projectionValue
) : document(std::move(documentValue)), projection(std::move(projectionValue)) {}

ProjectProjection loadProjectProjection(
    const std::filesystem::path& packagePath,
    std::stop_token cancellation
) {
    auto candidate = loadProjectCandidate(packagePath, cancellation);
    return std::move(candidate.projection);
}

ProjectLoadCandidate loadProjectCandidate(
    const std::filesystem::path& packagePath,
    std::stop_token cancellation
) {
    std::uint64_t generatedId = 0;
    auto document = palmier::project::readProjectPackage(
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
    return {std::move(document), std::move(result)};
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

    std::optional<project_render::StaticVideoTimeline> renderTimeline;
    try {
        renderTimeline = project_render::compileStaticVideoTimeline(
            document,
            activeTimeline->id.value,
            cancellation
        );
    } catch (const project_render::ProjectRenderCompileError& error) {
        checkCancellation(cancellation);
        if (error.code == "noVisibleVideoSegments") {
            return {PreviewCandidateAvailability::noCandidate, "noVideoCandidate", std::nullopt};
        }
        return {PreviewCandidateAvailability::unsupported, error.code, std::nullopt};
    }

    std::vector<PreviewMediaSourceProjection> sources;
    sources.reserve(renderTimeline->segments.size());
    for (const auto& layer : renderTimeline->segments) {
        checkCancellation(cancellation);
        std::optional<palmier::project::ResolvedProjectMediaReference> resolved;
        try {
            resolved = palmier::project::resolveProjectMediaReference(
                *manifest,
                layer.mediaId,
                "video",
                packagePath,
                cancellation
            );
        } catch (const palmier::project::ProjectMediaResolveError& error) {
            if (error.code == "cancelled") {
                throw ProjectProjectionError("cancelled", error.what());
            }
            if (error.code == "mediaEntryMissing" || error.code == "mediaFileUnavailable") {
                return {
                    PreviewCandidateAvailability::offline,
                    error.code,
                    std::nullopt,
                };
            }
            return {
                PreviewCandidateAvailability::unsupported,
                error.code,
                std::nullopt,
            };
        }
        if (resolved->hasAudio == false) {
            return {
                PreviewCandidateAvailability::unsupported,
                "videoOnlyPlaybackUnsupported",
                std::nullopt,
            };
        }
        sources.push_back({
            std::move(resolved->path),
            layer.clipId,
            resolved->hasAudio,
            resolved->sourceKind,
        });
    }
    return {
        PreviewCandidateAvailability::available,
        {},
        PreviewMediaCandidateProjection{
            std::move(*renderTimeline),
            std::move(sources),
        },
    };
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
                    clip.trimStartFrame,
                    clip.trimEndFrame,
                    clip.speed,
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
                item.insert(QStringLiteral("trimStartFrameText"), QString::number(clip.trimStartFrame));
                item.insert(QStringLiteral("trimEndFrameText"), QString::number(clip.trimEndFrame));
                item.insert(QStringLiteral("speedText"), QString::number(clip.speed, 'g', 15));
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
