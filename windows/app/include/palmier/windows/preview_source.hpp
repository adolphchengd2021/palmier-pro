#pragma once

#include "palmier/project/media_manifest_reader.hpp"
#include "palmier/project_render/project_render_compiler.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace palmier::windows {

enum class PreviewCandidateAvailability {
    available,
    noCandidate,
    offline,
    unsupported,
    invalidated,
};

struct PreviewMediaSourceProjection final {
    std::filesystem::path inputPath;
    std::string clipId;
    std::optional<bool> hasAudio;
    project::MediaSourceKind sourceKind{project::MediaSourceKind::project};
};

struct PreviewMediaCandidateProjection final {
    project_render::StaticVideoTimeline renderTimeline;
    std::vector<PreviewMediaSourceProjection> sources;

    const project_render::StaticVideoLayer* firstRenderLayer() const noexcept;
    const PreviewMediaSourceProjection* sourceForClip(
        std::string_view clipId
    ) const noexcept;
};

struct ProjectPreviewProjection final {
    PreviewCandidateAvailability availability{PreviewCandidateAvailability::noCandidate};
    std::string reasonCode{"noVideoCandidate"};
    std::optional<PreviewMediaCandidateProjection> candidate;
};

}
