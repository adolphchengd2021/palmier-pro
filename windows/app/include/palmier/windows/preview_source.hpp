#pragma once

#include "palmier/project_render/project_render_compiler.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace palmier::windows {

enum class PreviewCandidateAvailability {
    available,
    noCandidate,
    offline,
    unsupported,
    invalidated,
};

struct PreviewMediaCandidateProjection final {
    std::filesystem::path inputPath;
    project_render::StaticVideoLayer renderLayer;
    std::optional<bool> hasAudio;
};

struct ProjectPreviewProjection final {
    PreviewCandidateAvailability availability{PreviewCandidateAvailability::noCandidate};
    std::string reasonCode{"noVideoCandidate"};
    std::optional<PreviewMediaCandidateProjection> candidate;
};

}
