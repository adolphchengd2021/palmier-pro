#include "palmier/render/cpu_renderer.hpp"
#include "palmier/render/d3d11_warp_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using palmier::render::RenderLayer;
using palmier::render::RenderPlan;
using palmier::render::RenderedFrame;
using palmier::render::Rgba32Float;
using palmier::render::SourceFrame;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

RenderLayer layer(std::string id, std::string trackId, std::string mediaId) {
    return {
        std::move(id),
        std::move(trackId),
        std::move(mediaId),
        0,
        {},
        1,
        palmier::render::BlendMode::normal,
        std::nullopt,
    };
}

void compareFrames(
    const RenderedFrame& expected,
    const RenderedFrame& actual,
    float absoluteTolerance
) {
    require(expected.width == actual.width && expected.height == actual.height, "frame size drift");
    require(expected.pixels.size() == actual.pixels.size(), "frame storage drift");
    float maximumError = 0;
    std::size_t maximumIndex = 0;
    for (std::size_t index = 0; index < expected.pixels.size(); ++index) {
        const float expectedChannels[]{
            expected.pixels[index].red,
            expected.pixels[index].green,
            expected.pixels[index].blue,
            expected.pixels[index].alpha,
        };
        const float actualChannels[]{
            actual.pixels[index].red,
            actual.pixels[index].green,
            actual.pixels[index].blue,
            actual.pixels[index].alpha,
        };
        for (std::size_t channel = 0; channel < 4; ++channel) {
            require(std::isfinite(actualChannels[channel]), "D3D output contains non-finite value");
            const auto error = std::abs(expectedChannels[channel] - actualChannels[channel]);
            if (error > maximumError) {
                maximumError = error;
                maximumIndex = index * 4 + channel;
            }
        }
    }
    if (maximumError > absoluteTolerance) {
        throw std::runtime_error(
            "CPU/WARP drift at channel " + std::to_string(maximumIndex)
            + ": " + std::to_string(maximumError)
        );
    }
}

void requirePixel(
    const RenderedFrame& frame,
    std::uint32_t x,
    std::uint32_t y,
    const Rgba32Float& expected,
    const std::string& message
) {
    const auto& actual = frame.pixels[static_cast<std::size_t>(y) * frame.width + x];
    const float actualChannels[]{actual.red, actual.green, actual.blue, actual.alpha};
    const float expectedChannels[]{
        expected.red,
        expected.green,
        expected.blue,
        expected.alpha,
    };
    for (std::size_t channel = 0; channel < 4; ++channel) {
        if (std::abs(actualChannels[channel] - expectedChannels[channel]) > 1e-6F) {
            throw std::runtime_error(message + " channel " + std::to_string(channel));
        }
    }
}

void renderParity() {
    const SourceFrame quadrants{
        2,
        2,
        {
            {0.1F, 0.2F, 0.3F, 1}, {0.4F, 0.3F, 0.2F, 1},
            {0.2F, 0.4F, 0.1F, 1}, {0.3F, 0.1F, 0.4F, 1},
        },
    };
    const SourceFrame overlay{
        1,
        1,
        {{0.25F, 0.5F, 0.125F, 0.5F}},
    };
    auto bottom = layer("bottom", "track-bottom", "quadrants");
    bottom.exposureEv = 1.0F;
    auto top = layer("top", "track-top", "overlay");
    top.transform = {0.75F, 0.5F, 0.5F, 1, 0};
    top.opacity = 0.5F;
    const auto plan = RenderPlan::create(8, 6, 30, 12, {bottom, top});
    const auto resolver = [&](std::string_view mediaId, std::int64_t) -> const SourceFrame* {
        if (mediaId == "quadrants") {
            return &quadrants;
        }
        if (mediaId == "overlay") {
            return &overlay;
        }
        return nullptr;
    };

    palmier::render::CpuRenderer cpu;
    const auto expected = cpu.render(plan, resolver);
    palmier::render::D3d11WarpRenderer warp;
    const auto preview = palmier::render::renderPreviewFrame(plan, resolver, warp);
    const auto exported = palmier::render::renderExportFrame(plan, resolver, warp);
    require(
        preview.width == exported.width && preview.height == exported.height,
        "preview/export WARP frame dimensions differ"
    );
    require(preview.pixels == exported.pixels, "preview/export WARP frames are not bitwise equal");
    compareFrames(expected, preview, 2e-5F);
}

void clockwiseRotationCoverage() {
    const SourceFrame source{
        2,
        2,
        {
            {1, 0, 0, 1}, {0, 1, 0, 1},
            {0, 0, 1, 1}, {1, 1, 1, 1},
        },
    };
    auto rotated = layer("rotated", "track", "source");
    rotated.transform = {0.5F, 0.5F, 0.5F, 0.5F, 90};
    const auto plan = RenderPlan::create(8, 8, 30, 0, {rotated});
    const auto resolver = [&](std::string_view, std::int64_t) { return &source; };
    palmier::render::CpuRenderer cpu;
    palmier::render::D3d11WarpRenderer warp;
    const auto expected = cpu.render(plan, resolver);
    requirePixel(expected, 2, 2, {0, 0, 1, 1}, "clockwise top-left quadrant");
    requirePixel(expected, 5, 2, {1, 0, 0, 1}, "clockwise top-right quadrant");
    requirePixel(expected, 2, 5, {1, 1, 1, 1}, "clockwise bottom-left quadrant");
    requirePixel(expected, 5, 5, {0, 1, 0, 1}, "clockwise bottom-right quadrant");
    requirePixel(expected, 0, 0, {0, 0, 0, 1}, "clockwise left outside coverage");
    requirePixel(expected, 7, 7, {0, 0, 0, 1}, "clockwise bottom outside coverage");
    compareFrames(expected, warp.render(plan, resolver), 2e-5F);
}

void cancellationStopsWarpBeforeRendering() {
    const SourceFrame source{1, 1, {{1, 0, 0, 1}}};
    const auto plan = RenderPlan::create(
        1,
        1,
        30,
        0,
        {layer("clip", "track", "media")}
    );
    const auto resolver = [&](std::string_view, std::int64_t) { return &source; };
    palmier::render::D3d11WarpRenderer warp;
    std::stop_source stopped;
    stopped.request_stop();
    try {
        static_cast<void>(warp.render(plan, resolver, stopped.get_token()));
    } catch (const palmier::render::RenderError& error) {
        require(error.code == "cancelled", "WARP cancellation code differs");
        return;
    }
    throw std::runtime_error("cancelled WARP render was accepted");
}

}

int main() {
    try {
        renderParity();
        clockwiseRotationCoverage();
        cancellationStopsWarpBeforeRendering();
        std::cout << "PALMIER_D3D11_WARP_TESTS_OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "PALMIER_D3D11_WARP_TESTS_FAILED " << error.what() << '\n';
        return 1;
    }
}
