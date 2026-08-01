#include "palmier/render/cpu_renderer.hpp"

#include <cmath>
#include <numbers>
#include <new>
#include <utility>

namespace palmier::render {
namespace {

float srgbToLinear(float value) {
    return value <= 0.04045F
        ? value / 12.92F
        : std::pow((value + 0.055F) / 1.055F, 2.4F);
}

float linearToSrgb(float value) {
    return value <= 0.0031308F
        ? value * 12.92F
        : 1.055F * std::pow(value, 1.0F / 2.4F) - 0.055F;
}

Rgba32Float sampleLayer(
    const RenderLayer& layer,
    const SourceFrame& source,
    std::uint32_t targetX,
    std::uint32_t targetY,
    std::uint32_t canvasWidth,
    std::uint32_t canvasHeight
) {
    const auto targetU = (static_cast<float>(targetX) + 0.5F)
        / static_cast<float>(canvasWidth);
    const auto targetV = (static_cast<float>(targetY) + 0.5F)
        / static_cast<float>(canvasHeight);
    const auto deltaX = targetU - layer.transform.centerX;
    const auto deltaY = targetV - layer.transform.centerY;
    const auto angle = layer.transform.rotationDegrees
        * std::numbers::pi_v<float> / 180.0F;
    const auto cosine = std::cos(angle);
    const auto sine = std::sin(angle);
    const auto localX = cosine * deltaX + sine * deltaY;
    const auto localY = -sine * deltaX + cosine * deltaY;
    const auto sourceU = localX / layer.transform.width + 0.5F;
    const auto sourceV = localY / layer.transform.height + 0.5F;
    if (
        !std::isfinite(sourceU)
        || !std::isfinite(sourceV)
        || sourceU < 0
        || sourceU >= 1
        || sourceV < 0
        || sourceV >= 1
    ) {
        return {0, 0, 0, 0};
    }

    const auto sourceX = static_cast<std::uint32_t>(sourceU * source.width);
    const auto sourceY = static_cast<std::uint32_t>(sourceV * source.height);
    const auto& pixel = source.pixels[
        static_cast<std::size_t>(sourceY) * source.width + sourceX
    ];
    const auto multiplier = std::exp2(layer.exposureEv.value_or(0));
    const auto alpha = pixel.alpha * layer.opacity;
    const auto red = linearToSrgb(srgbToLinear(pixel.red) * multiplier);
    const auto green = linearToSrgb(srgbToLinear(pixel.green) * multiplier);
    const auto blue = linearToSrgb(srgbToLinear(pixel.blue) * multiplier);
    return {red * alpha, green * alpha, blue * alpha, alpha};
}

Rgba32Float sourceOver(const Rgba32Float& source, const Rgba32Float& destination) {
    const auto remaining = 1 - source.alpha;
    return {
        source.red + destination.red * remaining,
        source.green + destination.green * remaining,
        source.blue + destination.blue * remaining,
        source.alpha + destination.alpha * remaining,
    };
}

}

RenderedFrame CpuRenderer::render(
    const RenderPlan& plan,
    const FrameResolver& resolveFrame,
    std::stop_token cancellation
) {
    const auto pixelCount = static_cast<std::size_t>(plan.canvasWidth()) * plan.canvasHeight();
    const auto sources = resolveAndValidateSourceFrames(plan, resolveFrame, cancellation);
    if (cancellation.stop_requested()) {
        throw RenderError("cancelled", "/", "CPU render was cancelled");
    }
    std::vector<Rgba32Float> pixels;
    try {
        pixels.assign(pixelCount, {0, 0, 0, 1});
    } catch (const std::bad_alloc&) {
        throw RenderError(
            "resourceExhausted",
            "/canvas",
            "CPU render output allocation failed"
        );
    }
    if (cancellation.stop_requested()) {
        throw RenderError("cancelled", "/", "CPU render was cancelled");
    }
    RenderedFrame result{plan.canvasWidth(), plan.canvasHeight(), std::move(pixels)};

    for (std::size_t layerIndex = 0; layerIndex < plan.layers().size(); ++layerIndex) {
        if (cancellation.stop_requested()) {
            throw RenderError("cancelled", "/", "CPU render was cancelled");
        }
        const auto& layer = plan.layers()[layerIndex];
        const auto& source = *sources[layerIndex];
        for (std::uint32_t y = 0; y < plan.canvasHeight(); ++y) {
            if (cancellation.stop_requested()) {
                throw RenderError("cancelled", "/", "CPU render was cancelled");
            }
            for (std::uint32_t x = 0; x < plan.canvasWidth(); ++x) {
                const auto sourcePixel = sampleLayer(
                    layer,
                    source,
                    x,
                    y,
                    plan.canvasWidth(),
                    plan.canvasHeight()
                );
                const auto offset = static_cast<std::size_t>(y) * plan.canvasWidth() + x;
                result.pixels[offset] = sourceOver(sourcePixel, result.pixels[offset]);
            }
        }
    }
    return result;
}

}
