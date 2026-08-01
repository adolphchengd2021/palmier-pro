#pragma once

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
};

struct PreviewMediaCandidateProjection final {
    std::string timelineId;
    std::string trackId;
    std::string clipId;
    std::string mediaId;
    std::filesystem::path inputPath;
    std::int64_t timelineFrame{};
    std::int64_t durationFrames{};
    std::int64_t framesPerSecond{};
    std::int64_t canvasWidth{};
    std::int64_t canvasHeight{};
    double opacity{1};
    std::optional<bool> hasAudio;
};

struct ProjectPreviewProjection final {
    PreviewCandidateAvailability availability{PreviewCandidateAvailability::noCandidate};
    std::string reasonCode{"noVideoCandidate"};
    std::optional<PreviewMediaCandidateProjection> candidate;
};

}
