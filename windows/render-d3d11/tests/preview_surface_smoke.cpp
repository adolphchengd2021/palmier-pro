#include "palmier/render/d3d11_preview_surface.hpp"
#include "internal/d3d11_preview_surface_testing.hpp"

#include <Windows.h>

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <stop_token>
#include <string>

namespace {

using palmier::render::D3d11PreviewDriver;
using palmier::render::D3d11PreviewSurface;
using palmier::render::D3d11PreviewSurfaceOutcome;
using palmier::render::D3d11PreviewSurfaceState;
using palmier::render::RenderedFrame;
using palmier::render::detail::D3d11PreviewSurfaceTestAccess;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

LRESULT CALLBACK previewWindowProcedure(
    HWND window,
    UINT message,
    WPARAM word,
    LPARAM value
) {
    return DefWindowProcW(window, message, word, value);
}

class HiddenWindow final {
public:
    HiddenWindow() {
        const WNDCLASSEXW windowClass{
            sizeof(WNDCLASSEXW),
            0,
            previewWindowProcedure,
            0,
            0,
            GetModuleHandleW(nullptr),
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            className,
            nullptr,
        };
        if (RegisterClassExW(&windowClass) == 0
            && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            throw std::runtime_error("hidden preview window class registration failed");
        }
        window_ = CreateWindowExW(
            0,
            className,
            L"Palmier preview smoke",
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            64,
            48,
            nullptr,
            nullptr,
            GetModuleHandleW(nullptr),
            nullptr
        );
        if (window_ == nullptr) {
            throw std::runtime_error("hidden preview window creation failed");
        }
    }

    ~HiddenWindow() {
        if (window_ != nullptr) {
            DestroyWindow(window_);
        }
        UnregisterClassW(className, GetModuleHandleW(nullptr));
    }

    HWND get() const noexcept { return window_; }

private:
    static constexpr auto className = L"PalmierPreviewSurfaceSmoke";
    HWND window_{};
};

bool acceptedEnvironmentOutcome(D3d11PreviewSurfaceOutcome outcome) {
    return outcome == D3d11PreviewSurfaceOutcome::presented
        || outcome == D3d11PreviewSurfaceOutcome::cleared
        || outcome == D3d11PreviewSurfaceOutcome::occluded
        || outcome == D3d11PreviewSurfaceOutcome::noOp;
}

RenderedFrame frame() {
    return {
        2,
        2,
        {
            {1, 0, 0, 1},
            {0, 1, 0, 1},
            {0, 0, 1, 1},
            {1, 1, 1, 1},
        },
    };
}

RenderedFrame smallFrame() {
    return {1, 1, {{0.25F, 0.5F, 0.75F, 1}}};
}

void presentsToAHiddenWarpSwapChain() {
    HiddenWindow window;
    D3d11PreviewSurface surface(window.get(), D3d11PreviewDriver::warp);

    const auto resized = surface.resize(64, 48);
    if (resized.outcome == D3d11PreviewSurfaceOutcome::unavailable) {
        require(
            resized.state == D3d11PreviewSurfaceState::invalidated,
            "unavailable WARP resize returned the wrong state"
        );
        surface.close();
        return;
    }
    require(resized.state == D3d11PreviewSurfaceState::ready, "WARP resize failed");
    require(resized.width == 64 && resized.height == 48, "WARP resize changed size");
    require(
        surface.resize(64, 48).outcome == D3d11PreviewSurfaceOutcome::noOp,
        "same WARP resize changed state"
    );
    require(
        SUCCEEDED(D3d11PreviewSurfaceTestAccess::prepareUploadResources(surface, 2, 2)),
        "WARP upload resource preparation failed"
    );
    const auto firstUploadSerial = D3d11PreviewSurfaceTestAccess::uploadResourceSerial(surface);
    require(firstUploadSerial == 1, "first WARP upload resource was not recorded");
    require(
        SUCCEEDED(D3d11PreviewSurfaceTestAccess::prepareUploadResources(surface, 2, 2)),
        "same-size WARP upload resource reuse failed"
    );
    require(
        D3d11PreviewSurfaceTestAccess::uploadResourceSerial(surface) == firstUploadSerial,
        "same-size WARP frame recreated upload resources"
    );
    require(
        SUCCEEDED(D3d11PreviewSurfaceTestAccess::prepareUploadResources(surface, 1, 1)),
        "resized WARP upload resource preparation failed"
    );
    require(
        D3d11PreviewSurfaceTestAccess::uploadResourceSerial(surface)
            == firstUploadSerial + 1,
        "resized WARP frame did not replace upload resources"
    );

    auto invalid = frame();
    invalid.pixels.pop_back();
    require(
        surface.present(invalid).outcome == D3d11PreviewSurfaceOutcome::refused,
        "invalid WARP frame was accepted"
    );
    const auto presented = surface.present(frame());
    require(
        acceptedEnvironmentOutcome(presented.outcome),
        "hidden WARP Present was not classified"
    );
    require(
        presented.state == D3d11PreviewSurfaceState::ready
            || presented.state == D3d11PreviewSurfaceState::occluded,
        "hidden WARP Present returned the wrong state"
    );
    const auto uploadSerialAfterPresent
        = D3d11PreviewSurfaceTestAccess::uploadResourceSerial(surface);
    const auto repeated = surface.present(frame());
    require(
        acceptedEnvironmentOutcome(repeated.outcome),
        "repeated hidden WARP Present was not classified"
    );
    require(
        D3d11PreviewSurfaceTestAccess::uploadResourceSerial(surface)
            == uploadSerialAfterPresent,
        "same-size WARP Present recreated upload resources"
    );
    if (repeated.state == D3d11PreviewSurfaceState::ready) {
        const auto resizedSource = surface.present(smallFrame());
        require(
            acceptedEnvironmentOutcome(resizedSource.outcome),
            "resized-source hidden WARP Present was not classified"
        );
        require(
            D3d11PreviewSurfaceTestAccess::uploadResourceSerial(surface)
                == uploadSerialAfterPresent + 1,
            "resized-source WARP Present did not replace upload resources"
        );
    }

    const auto secondResize = surface.resize(32, 32);
    require(secondResize.state == D3d11PreviewSurfaceState::ready, "WARP re-resize failed");
    require(secondResize.width == 32 && secondResize.height == 32, "WARP re-resize changed size");
    require(
        acceptedEnvironmentOutcome(surface.clear().outcome),
        "hidden WARP clear was not classified"
    );
    require(surface.close().state == D3d11PreviewSurfaceState::closed, "WARP close failed");
    require(surface.close().state == D3d11PreviewSurfaceState::closed, "repeat WARP close changed");
}

void validatesCancellationAndConfiguration() {
    HiddenWindow window;
    D3d11PreviewSurface surface(window.get(), D3d11PreviewDriver::warp);
    std::stop_source cancelled;
    cancelled.request_stop();
    require(
        surface.resize(16, 16, cancelled.get_token()).outcome
            == D3d11PreviewSurfaceOutcome::cancelled,
        "pre-cancelled WARP resize ran"
    );
    require(
        surface.resize(0, 16).outcome == D3d11PreviewSurfaceOutcome::refused,
        "zero WARP width was accepted"
    );
    require(
        surface.resize(16, 0).outcome == D3d11PreviewSurfaceOutcome::refused,
        "zero WARP height was accepted"
    );
    surface.close();

    bool threw = false;
    try {
        palmier::render::D3d11PreviewSurfaceLimits limits;
        limits.maximumSurfacePixels = 0;
        D3d11PreviewSurface invalid(window.get(), D3d11PreviewDriver::warp, limits);
        static_cast<void>(invalid.snapshot());
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "zero WARP surface budget was accepted");
}

void classifiesInjectedPresentResults() {
    using palmier::render::detail::classifyD3d11PreviewResult;
    const auto success = classifyD3d11PreviewResult(
        S_OK,
        D3d11PreviewSurfaceOutcome::presented
    );
    require(success.state == D3d11PreviewSurfaceState::ready, "success state drifted");
    require(success.outcome == D3d11PreviewSurfaceOutcome::presented, "success outcome drifted");

    const auto occluded = classifyD3d11PreviewResult(
        DXGI_STATUS_OCCLUDED,
        D3d11PreviewSurfaceOutcome::presented
    );
    require(occluded.state == D3d11PreviewSurfaceState::occluded, "occlusion state drifted");
    require(occluded.outcome == D3d11PreviewSurfaceOutcome::occluded, "occlusion outcome drifted");

    const auto busy = classifyD3d11PreviewResult(
        DXGI_ERROR_WAS_STILL_DRAWING,
        D3d11PreviewSurfaceOutcome::presented
    );
    require(busy.state == D3d11PreviewSurfaceState::ready, "busy state drifted");
    require(busy.outcome == D3d11PreviewSurfaceOutcome::noOp, "busy outcome drifted");

    const auto removed = classifyD3d11PreviewResult(
        DXGI_ERROR_DEVICE_REMOVED,
        D3d11PreviewSurfaceOutcome::presented
    );
    require(removed.state == D3d11PreviewSurfaceState::invalidated, "device-loss state drifted");
    require(removed.outcome == D3d11PreviewSurfaceOutcome::invalidated, "device-loss outcome drifted");

    const auto unavailable = classifyD3d11PreviewResult(
        DXGI_ERROR_UNSUPPORTED,
        D3d11PreviewSurfaceOutcome::presented
    );
    require(unavailable.state == D3d11PreviewSurfaceState::invalidated, "unavailable state drifted");
    require(unavailable.outcome == D3d11PreviewSurfaceOutcome::unavailable, "unavailable outcome drifted");

    const auto failure = classifyD3d11PreviewResult(
        E_FAIL,
        D3d11PreviewSurfaceOutcome::presented
    );
    require(failure.state == D3d11PreviewSurfaceState::failed, "failure state drifted");
    require(failure.outcome == D3d11PreviewSurfaceOutcome::failed, "failure outcome drifted");
}

}

int main() {
    try {
        presentsToAHiddenWarpSwapChain();
        validatesCancellationAndConfiguration();
        classifiesInjectedPresentResults();
        std::cout << "PALMIER_D3D11_PREVIEW_SURFACE_CLASSIFIED\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "PALMIER_D3D11_PREVIEW_SURFACE_FAILED " << error.what() << '\n';
        return 1;
    }
}
