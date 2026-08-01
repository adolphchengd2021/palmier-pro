#pragma once

#include "palmier/render/d3d11_preview_surface.hpp"

namespace palmier::render::detail {

struct D3d11PreviewResultClassification final {
    D3d11PreviewSurfaceState state;
    D3d11PreviewSurfaceOutcome outcome;
};

D3d11PreviewResultClassification classifyD3d11PreviewResult(
    HRESULT result,
    D3d11PreviewSurfaceOutcome successOutcome
) noexcept;

}
