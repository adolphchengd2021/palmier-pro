#include "palmier/exporting/project_clip_h264_export_workflow.hpp"

#include "palmier/project/media_manifest_reader.hpp"
#include "palmier/project/project_media_resolver.hpp"
#include "palmier/project_render/project_render_compiler.hpp"

#include <optional>
#include <string>
#include <utility>

namespace palmier::exporting {
namespace {

[[noreturn]] void fail(
    H264ExportFailureCode code,
    std::string stage,
    std::string detail
) {
    throw H264ExportError(code, std::move(stage), std::move(detail));
}

void checkCancellation(std::stop_token cancellation, std::string stage) {
    if (cancellation.stop_requested()) {
        fail(H264ExportFailureCode::cancelled, std::move(stage), "export cancelled");
    }
}

struct SelectedClip final {
    const project::Timeline* timeline;
    const project::Track* track;
    const project::Clip* clip;
};

SelectedClip resolveSelection(
    const project::ProjectDocument& document,
    const ProjectClipH264ExportRequest& request,
    std::stop_token cancellation
) {
    checkCancellation(cancellation, "resolveSelection");
    if (
        request.packagePath.empty()
        || request.trackId.empty()
        || request.clipId.empty()
        || request.destination.empty()
    ) {
        fail(
            H264ExportFailureCode::invalidRequest,
            "resolveSelection",
            "package, track, clip, and destination are required"
        );
    }
    const project::Timeline* activeTimeline = nullptr;
    for (const auto& timeline : document.project().timelines) {
        checkCancellation(cancellation, "resolveSelection");
        if (timeline.id.value != document.project().activeTimelineId) continue;
        if (activeTimeline != nullptr) {
            fail(
                H264ExportFailureCode::invalidRequest,
                "resolveSelection",
                "active timeline identity is ambiguous"
            );
        }
        activeTimeline = &timeline;
    }
    if (activeTimeline == nullptr) {
        fail(
            H264ExportFailureCode::invalidRequest,
            "resolveSelection",
            "active timeline is unavailable"
        );
    }
    if (activeTimeline->id.origin != project::EntityIdOrigin::persisted) {
        fail(
            H264ExportFailureCode::invalidRequest,
            "resolveSelection",
            "active timeline has no persisted identity"
        );
    }

    const project::Track* selectedTrack = nullptr;
    for (const auto& track : activeTimeline->tracks) {
        checkCancellation(cancellation, "resolveSelection");
        if (track.id.value != request.trackId) continue;
        if (selectedTrack != nullptr) {
            fail(
                H264ExportFailureCode::invalidRequest,
                "resolveSelection",
                "selected track identity is ambiguous"
            );
        }
        selectedTrack = &track;
    }
    if (selectedTrack == nullptr) {
        fail(
            H264ExportFailureCode::invalidRequest,
            "resolveSelection",
            "selected track is unavailable"
        );
    }
    if (selectedTrack->id.origin != project::EntityIdOrigin::persisted) {
        fail(
            H264ExportFailureCode::invalidRequest,
            "resolveSelection",
            "selected track has no persisted identity"
        );
    }

    const project::Clip* selectedClip = nullptr;
    for (const auto& clip : selectedTrack->clips) {
        checkCancellation(cancellation, "resolveSelection");
        if (clip.id.value != request.clipId) continue;
        if (selectedClip != nullptr) {
            fail(
                H264ExportFailureCode::invalidRequest,
                "resolveSelection",
                "selected clip identity is ambiguous"
            );
        }
        selectedClip = &clip;
    }
    if (selectedClip == nullptr) {
        fail(
            H264ExportFailureCode::invalidRequest,
            "resolveSelection",
            "selected clip is unavailable"
        );
    }
    if (selectedClip->id.origin != project::EntityIdOrigin::persisted) {
        fail(
            H264ExportFailureCode::invalidRequest,
            "resolveSelection",
            "selected clip has no persisted identity"
        );
    }
    if (
        selectedTrack->type != "video"
        || selectedClip->mediaType != "video"
        || selectedClip->sourceClipType != "video"
    ) {
        fail(
            H264ExportFailureCode::unsupportedProject,
            "resolveSelection",
            "selected clip is not a supported video clip"
        );
    }
    return {activeTimeline, selectedTrack, selectedClip};
}

}

H264ProjectExportReceipt exportProjectClipH264(
    const project::ProjectDocument& document,
    const ProjectClipH264ExportRequest& request,
    const H264ExportLimits& limits,
    std::stop_token cancellation
) {
    const auto selected = resolveSelection(document, request, cancellation);
    std::optional<project::MediaManifest> manifest;
    try {
        manifest = project::readMediaManifest(
            request.packagePath,
            {.cancellation = cancellation}
        );
    } catch (const project::MediaManifestReadError& error) {
        fail(
            error.code == "cancelled"
                ? H264ExportFailureCode::cancelled
                : H264ExportFailureCode::mediaUnavailable,
            "loadMediaManifest",
            error.what()
        );
    }
    if (!manifest) {
        fail(
            H264ExportFailureCode::mediaUnavailable,
            "loadMediaManifest",
            "project media manifest is unavailable"
        );
    }

    project::ResolvedProjectMediaReference media;
    try {
        media = project::resolveProjectMediaReference(
            *manifest,
            selected.clip->mediaRef,
            "video",
            request.packagePath,
            cancellation
        );
    } catch (const project::ProjectMediaResolveError& error) {
        fail(
            error.code == "cancelled"
                ? H264ExportFailureCode::cancelled
                : H264ExportFailureCode::mediaUnavailable,
            "resolveMediaReference",
            error.what()
        );
    }

    return exportStaticProjectH264(
        document,
        {
            selected.timeline->id.value,
            selected.track->id.value,
            selected.clip->id.value,
            std::move(media.path),
            request.destination,
            request.bitRate,
            request.replaceExisting,
        },
        limits,
        cancellation
    );
}

H264ProjectExportReceipt exportProjectTimelineH264(
    const project::ProjectDocument& document,
    const ProjectTimelineH264ExportRequest& request,
    const H264ExportLimits& limits,
    std::stop_token cancellation
) {
    checkCancellation(cancellation, "resolveTimeline");
    if (request.packagePath.empty() || request.destination.empty()
        || document.project().activeTimelineId.empty()) {
        fail(
            H264ExportFailureCode::invalidRequest,
            "resolveTimeline",
            "package, active timeline, and destination are required"
        );
    }
    project_render::StaticVideoTimeline timeline;
    try {
        timeline = project_render::compileStaticVideoTimeline(
            document,
            document.project().activeTimelineId,
            cancellation
        );
    } catch (const project_render::ProjectRenderCompileError& error) {
        fail(
            error.code == "cancelled"
                ? H264ExportFailureCode::cancelled
                : error.code == "entityUnavailable"
                    ? H264ExportFailureCode::invalidRequest
                    : error.code == "resourceLimitExceeded"
                        ? H264ExportFailureCode::resourceLimitExceeded
                        : H264ExportFailureCode::unsupportedProject,
            "compileTimeline",
            error.code + " at " + error.jsonPointer
        );
    }

    std::optional<project::MediaManifest> manifest;
    try {
        manifest = project::readMediaManifest(
            request.packagePath,
            {.cancellation = cancellation}
        );
    } catch (const project::MediaManifestReadError& error) {
        fail(
            error.code == "cancelled"
                ? H264ExportFailureCode::cancelled
                : H264ExportFailureCode::mediaUnavailable,
            "loadMediaManifest",
            error.what()
        );
    }
    if (!manifest) {
        fail(
            H264ExportFailureCode::mediaUnavailable,
            "loadMediaManifest",
            "project media manifest is unavailable"
        );
    }

    std::vector<H264ProjectExportSource> sources;
    sources.reserve(timeline.segments.size());
    for (const auto& segment : timeline.segments) {
        checkCancellation(cancellation, "resolveMediaReferences");
        try {
            auto media = project::resolveProjectMediaReference(
                *manifest,
                segment.mediaId,
                "video",
                request.packagePath,
                cancellation
            );
            sources.push_back({segment.clipId, std::move(media.path)});
        } catch (const project::ProjectMediaResolveError& error) {
            fail(
                error.code == "cancelled"
                    ? H264ExportFailureCode::cancelled
                    : H264ExportFailureCode::mediaUnavailable,
                "resolveMediaReferences",
                error.what()
            );
        }
    }

    return exportStaticProjectTimelineH264(
        document,
        {
            timeline.timelineId,
            std::move(sources),
            request.destination,
            request.bitRate,
            request.replaceExisting,
        },
        limits,
        cancellation
    );
}

}
