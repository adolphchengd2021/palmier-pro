#pragma once

#include "palmier/project/project.hpp"

#include <QVariantList>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <vector>

namespace palmier::windows {

inline constexpr std::size_t maximumProjectedClipsPerTrack = 500;
inline constexpr std::size_t maximumProjectedClipsPerTimeline = 10'000;
inline constexpr std::size_t maximumProjectedTracksPerTimeline = 200;

struct ClipProjection final {
    std::string id;
    std::string mediaType;
    std::int64_t startFrame{};
    std::int64_t durationFrames{};
    double offsetRatio{};
    double extentRatio{};
};

struct TrackProjection final {
    std::string id;
    std::string type;
    std::vector<ClipProjection> clips;
    QVariantList clipItems;
};

struct TimelineProjection final {
    std::string id;
    std::string name;
    std::int64_t fps{};
    std::int64_t durationFrames{};
    std::vector<TrackProjection> tracks;
};

struct DiagnosticProjection final {
    std::string code;
    std::string jsonPointer;
};

struct ProjectProjection final {
    std::string activeTimelineId;
    std::vector<TimelineProjection> timelines;
    std::size_t diagnosticCount{};
    std::size_t skippedUnsafeClipCount{};
    std::optional<DiagnosticProjection> firstDiagnostic;
};

class ProjectProjectionError final : public std::runtime_error {
public:
    ProjectProjectionError(std::string code, std::string detail);

    const std::string code;
};

ProjectProjection loadProjectProjection(
    const std::filesystem::path& packagePath,
    std::stop_token cancellation
);
ProjectProjection projectDocumentForReadOnlyTimeline(
    const palmier::project::ProjectDocument& document,
    std::stop_token cancellation
);

}
