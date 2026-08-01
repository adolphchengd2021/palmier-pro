#pragma once

#include "palmier/render/render_plan.hpp"

#include <Windows.h>

#include <cstdint>
#include <memory>
#include <stop_token>

namespace palmier::render {

enum class D3d11PreviewDriver {
    hardware,
    warp,
};

enum class D3d11PreviewSurfaceState {
    idle,
    ready,
    occluded,
    invalidated,
    failed,
    closed,
};

enum class D3d11PreviewSurfaceOutcome {
    presented,
    cleared,
    noOp,
    cancelled,
    refused,
    occluded,
    unavailable,
    invalidated,
    failed,
};

enum class D3d11PreviewSurfaceStage {
    none,
    initialize,
    resize,
    upload,
    draw,
    present,
    clear,
    close,
};

struct D3d11PreviewSurfaceLimits final {
    std::uint64_t maximumSurfacePixels{3'840ULL * 2'160ULL};
};

struct D3d11PreviewSurfaceReceipt final {
    D3d11PreviewSurfaceState state{D3d11PreviewSurfaceState::idle};
    D3d11PreviewSurfaceOutcome outcome{D3d11PreviewSurfaceOutcome::noOp};
    D3d11PreviewSurfaceStage stage{D3d11PreviewSurfaceStage::none};
    HRESULT hresult{S_OK};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint64_t presentSerial{};
};

class D3d11PreviewSurface final {
public:
    D3d11PreviewSurface(
        HWND window,
        D3d11PreviewDriver driver = D3d11PreviewDriver::hardware,
        D3d11PreviewSurfaceLimits limits = {}
    );
    ~D3d11PreviewSurface();

    D3d11PreviewSurface(const D3d11PreviewSurface&) = delete;
    D3d11PreviewSurface& operator=(const D3d11PreviewSurface&) = delete;
    D3d11PreviewSurface(D3d11PreviewSurface&&) = delete;
    D3d11PreviewSurface& operator=(D3d11PreviewSurface&&) = delete;

    D3d11PreviewSurfaceReceipt resize(
        std::uint32_t width,
        std::uint32_t height,
        std::stop_token cancellation = {}
    );
    D3d11PreviewSurfaceReceipt present(
        const RenderedFrame& frame,
        std::stop_token cancellation = {}
    );
    D3d11PreviewSurfaceReceipt clear(std::stop_token cancellation = {});
    D3d11PreviewSurfaceReceipt snapshot() const;
    D3d11PreviewSurfaceReceipt close();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}
