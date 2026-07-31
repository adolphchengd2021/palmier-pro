#pragma once

#include "palmier/render/render_plan.hpp"

#include <memory>

namespace palmier::render {

class D3d11WarpRenderer final : public RenderBackend {
public:
    D3d11WarpRenderer();
    ~D3d11WarpRenderer() override;

    D3d11WarpRenderer(const D3d11WarpRenderer&) = delete;
    D3d11WarpRenderer& operator=(const D3d11WarpRenderer&) = delete;

    RenderedFrame render(
        const RenderPlan& plan,
        const FrameResolver& resolveFrame
    ) override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}
