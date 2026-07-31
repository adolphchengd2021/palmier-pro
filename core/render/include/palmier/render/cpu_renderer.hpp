#pragma once

#include "palmier/render/render_plan.hpp"

namespace palmier::render {

class CpuRenderer final : public RenderBackend {
public:
    RenderedFrame render(
        const RenderPlan& plan,
        const FrameResolver& resolveFrame
    ) override;
};

}
