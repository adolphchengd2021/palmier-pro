#pragma once

#include "palmier/project/project.hpp"
#include "palmier/render/render_plan.hpp"

#include <cstdint>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace palmier::project_render {

struct StaticVideoLayer final {
    std::uint32_t canvasWidth{};
    std::uint32_t canvasHeight{};
    std::int32_t framesPerSecond{};
    std::string timelineId;
    std::string trackId;
    std::string clipId;
    std::string mediaId;
    std::int64_t timelineStartFrame{};
    std::int64_t durationFrames{};
    std::int64_t sourceStartFrame{};
    render::Transform2D transform;
    float opacity{1};
    std::optional<float> exposureEv;
};

inline constexpr std::size_t maximumStaticVideoTimelineSegments = 256;

struct StaticVideoTimeline final {
    std::uint32_t canvasWidth{};
    std::uint32_t canvasHeight{};
    std::int32_t framesPerSecond{};
    std::string timelineId;
    std::int64_t durationFrames{};
    std::vector<StaticVideoLayer> segments;
};

class ProjectRenderCompileError final : public std::runtime_error {
public:
    ProjectRenderCompileError(
        std::string code,
        std::string jsonPointer,
        std::string detail
    );

    const std::string code;
    const std::string jsonPointer;
};

StaticVideoLayer compileStaticVideoLayer(
    const project::ProjectDocument& document,
    std::string_view timelineId,
    std::string_view trackId,
    std::string_view clipId,
    std::stop_token cancellation = {}
);

StaticVideoLayer compileExclusiveStaticVideoLayer(
    const project::ProjectDocument& document,
    std::string_view timelineId,
    std::string_view trackId,
    std::string_view clipId,
    std::stop_token cancellation = {}
);

StaticVideoTimeline compileStaticVideoTimeline(
    const project::ProjectDocument& document,
    std::string_view timelineId,
    std::stop_token cancellation = {}
);

const StaticVideoLayer* staticVideoLayerAt(
    const StaticVideoTimeline& timeline,
    std::int64_t timelineFrame
) noexcept;

render::RenderPlan makeRenderPlan(
    const StaticVideoLayer& layer,
    std::int64_t timelineFrame
);

render::RenderPlan makeRenderPlan(
    const StaticVideoTimeline& timeline,
    std::int64_t timelineFrame
);

}
