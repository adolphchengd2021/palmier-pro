#include "palmier/media/render_source_adapter.hpp"
#include "palmier/render/cpu_renderer.hpp"
#include "palmier/render/d3d11_warp_renderer.hpp"
#include "internal/render_source_adapter_testing.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using palmier::media::AlphaMode;
using palmier::media::DecodedVideoFrame;
using palmier::media::DisplayTransform;
using palmier::media::RenderSourceError;
using palmier::render::RenderLayer;
using palmier::render::RenderedFrame;
using palmier::render::Rgba32Float;
using palmier::render::SourceFrame;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void requireNear(float actual, float expected, const std::string& message) {
    if (std::abs(actual - expected) > 1e-6F) {
        throw std::runtime_error(message);
    }
}

template<typename Operation>
void requireError(Operation operation, std::string_view code) {
    try {
        operation();
    } catch (const RenderSourceError& error) {
        require(error.code == code, "unexpected adapter error code");
        return;
    }
    throw std::runtime_error("expected adapter failure");
}

DecodedVideoFrame frame(
    std::int16_t rotation,
    AlphaMode alphaMode = AlphaMode::straight
) {
    DecodedVideoFrame result;
    result.width = 3;
    result.height = 2;
    result.rowBytes = 16;
    result.rgba8 = {
        255, 0, 0, 255, 0, 255, 0, 128, 0, 0, 255, 64, 9, 9, 9, 9,
        255, 255, 0, 255, 0, 255, 255, 192, 255, 0, 255, 0, 8, 8, 8, 8,
    };
    result.displayTransform = DisplayTransform{rotation};
    result.color = {1, 13, 0, 2, 0};
    result.alphaMode = alphaMode;
    return result;
}

void requirePixel(
    const SourceFrame& source,
    std::uint32_t x,
    std::uint32_t y,
    const Rgba32Float& expected,
    const std::string& message
) {
    const auto& actual = source.pixels[static_cast<std::size_t>(y) * source.width + x];
    requireNear(actual.red, expected.red, message + " red");
    requireNear(actual.green, expected.green, message + " green");
    requireNear(actual.blue, expected.blue, message + " blue");
    requireNear(actual.alpha, expected.alpha, message + " alpha");
}

Rgba32Float pixel(std::uint8_t red, std::uint8_t green, std::uint8_t blue, std::uint8_t alpha) {
    constexpr float scale = 1.0F / 255.0F;
    return {red * scale, green * scale, blue * scale, alpha * scale};
}

void cardinalRotationsAndPaddedStride() {
    const auto identity = palmier::media::makeRenderSourceFrame(frame(0));
    require(identity.width == 3 && identity.height == 2, "identity dimensions");
    requirePixel(identity, 0, 0, pixel(255, 0, 0, 255), "identity A");
    requirePixel(identity, 2, 1, pixel(255, 0, 255, 0), "identity F");

    const auto counterClockwise = palmier::media::makeRenderSourceFrame(frame(90));
    require(counterClockwise.width == 2 && counterClockwise.height == 3, "CCW dimensions");
    requirePixel(counterClockwise, 0, 0, pixel(0, 0, 255, 64), "CCW top-left");
    requirePixel(counterClockwise, 1, 0, pixel(255, 0, 255, 0), "CCW top-right");
    requirePixel(counterClockwise, 0, 2, pixel(255, 0, 0, 255), "CCW bottom-left");
    requirePixel(counterClockwise, 1, 2, pixel(255, 255, 0, 255), "CCW bottom-right");

    const auto clockwise = palmier::media::makeRenderSourceFrame(frame(-90));
    require(clockwise.width == 2 && clockwise.height == 3, "clockwise dimensions");
    requirePixel(clockwise, 0, 0, pixel(255, 255, 0, 255), "clockwise top-left");
    requirePixel(clockwise, 1, 0, pixel(255, 0, 0, 255), "clockwise top-right");
    requirePixel(clockwise, 0, 2, pixel(255, 0, 255, 0), "clockwise bottom-left");
    requirePixel(clockwise, 1, 2, pixel(0, 0, 255, 64), "clockwise bottom-right");

    const auto upsideDown = palmier::media::makeRenderSourceFrame(frame(180));
    require(upsideDown.width == 3 && upsideDown.height == 2, "180 dimensions");
    requirePixel(upsideDown, 0, 0, pixel(255, 0, 255, 0), "180 top-left");
    requirePixel(upsideDown, 2, 1, pixel(255, 0, 0, 255), "180 bottom-right");

    auto opaqueFrame = frame(0, AlphaMode::opaque);
    constexpr std::array<std::size_t, 6> alphaIndices{3, 7, 11, 19, 23, 27};
    for (const auto alphaIndex : alphaIndices) {
        opaqueFrame.rgba8[alphaIndex] = 255;
    }
    const auto opaque = palmier::media::makeRenderSourceFrame(opaqueFrame);
    requirePixel(opaque, 2, 1, pixel(255, 0, 255, 255), "opaque accepted");
}

void refusalBoundaries() {
    auto unspecified = frame(0, AlphaMode::unspecified);
    requireError(
        [&] { palmier::media::makeRenderSourceFrame(unspecified); },
        "unsupportedAlphaMode"
    );
    auto premultiplied = frame(0, AlphaMode::premultiplied);
    requireError(
        [&] { palmier::media::makeRenderSourceFrame(premultiplied); },
        "unsupportedAlphaMode"
    );
    auto opaque = frame(0, AlphaMode::opaque);
    requireError(
        [&] { palmier::media::makeRenderSourceFrame(opaque); },
        "inconsistentOpaqueAlpha"
    );
    auto unsupportedPrimaries = frame(0);
    unsupportedPrimaries.color.primaries = 2;
    requireError(
        [&] { palmier::media::makeRenderSourceFrame(unsupportedPrimaries); },
        "unsupportedColorMetadata"
    );
    auto unsupportedTransfer = frame(0);
    unsupportedTransfer.color.transfer = 1;
    requireError(
        [&] { palmier::media::makeRenderSourceFrame(unsupportedTransfer); },
        "unsupportedColorMetadata"
    );
    auto unsupportedMatrix = frame(0);
    unsupportedMatrix.color.matrix = 1;
    requireError(
        [&] { palmier::media::makeRenderSourceFrame(unsupportedMatrix); },
        "unsupportedColorMetadata"
    );
    auto unsupportedRange = frame(0);
    unsupportedRange.color.range = 1;
    requireError(
        [&] { palmier::media::makeRenderSourceFrame(unsupportedRange); },
        "unsupportedColorMetadata"
    );
    auto unspecifiedRange = frame(0);
    unspecifiedRange.color.range = 0;
    palmier::media::makeRenderSourceFrame(unspecifiedRange);
    auto invalidRotation = frame(0);
    invalidRotation.displayTransform = DisplayTransform{45};
    requireError(
        [&] { palmier::media::makeRenderSourceFrame(invalidRotation); },
        "unsupportedDisplayTransform"
    );
    auto shortStride = frame(0);
    shortStride.rowBytes = 11;
    requireError(
        [&] { palmier::media::makeRenderSourceFrame(shortStride); },
        "invalidSourceStride"
    );
    auto shortStorage = frame(0);
    shortStorage.rgba8.pop_back();
    requireError(
        [&] { palmier::media::makeRenderSourceFrame(shortStorage); },
        "invalidSourceStorage"
    );
    auto zeroWidth = frame(0);
    zeroWidth.width = 0;
    requireError(
        [&] { palmier::media::makeRenderSourceFrame(zeroWidth); },
        "invalidSourceSize"
    );
    auto negativeHeight = frame(0);
    negativeHeight.height = -1;
    requireError(
        [&] { palmier::media::makeRenderSourceFrame(negativeHeight); },
        "invalidSourceSize"
    );
    auto tooLarge = frame(0);
    tooLarge.width = 3'841;
    tooLarge.height = 2'160;
    bool allocationAttempted = false;
    palmier::media::detail::RenderSourceAdapterHooks allocationHooks;
    allocationHooks.allocatePixels = [&](std::size_t pixelCount) {
        allocationAttempted = true;
        return std::vector<Rgba32Float>(pixelCount);
    };
    requireError(
        [&] {
            palmier::media::detail::makeRenderSourceFrame(
                tooLarge,
                {},
                allocationHooks
            );
        },
        "sourceBudgetExceeded"
    );
    require(!allocationAttempted, "source budget must be checked before allocation");
    std::stop_source cancellation;
    cancellation.request_stop();
    requireError(
        [&] {
            palmier::media::makeRenderSourceFrame(
                frame(0),
                cancellation.get_token()
            );
        },
        "cancelled"
    );
}

void deterministicCancellationBoundaries() {
    std::stop_source rowCancellation;
    bool secondRowConverted = false;
    palmier::media::detail::RenderSourceAdapterHooks rowHooks;
    rowHooks.didConvertRow = [&](std::uint32_t row) {
        if (row == 0) {
            rowCancellation.request_stop();
        } else {
            secondRowConverted = true;
        }
    };
    requireError(
        [&] {
            palmier::media::detail::makeRenderSourceFrame(
                frame(0),
                rowCancellation.get_token(),
                rowHooks
            );
        },
        "cancelled"
    );
    require(!secondRowConverted, "row conversion continued after cancellation");

    std::stop_source publicationCancellation;
    palmier::media::detail::RenderSourceAdapterHooks publicationHooks;
    publicationHooks.willPublish = [&] {
        publicationCancellation.request_stop();
    };
    requireError(
        [&] {
            palmier::media::detail::makeRenderSourceFrame(
                frame(0),
                publicationCancellation.get_token(),
                publicationHooks
            );
        },
        "cancelled"
    );
}

void compareFrames(
    const RenderedFrame& expected,
    const RenderedFrame& actual,
    float tolerance
) {
    require(expected.width == actual.width && expected.height == actual.height, "frame size drift");
    require(expected.pixels.size() == actual.pixels.size(), "frame storage drift");
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
            if (std::abs(expectedChannels[channel] - actualChannels[channel]) > tolerance) {
                throw std::runtime_error("CPU/WARP adapter drift");
            }
        }
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

void adapterFeedsSharedPreviewAndExport() {
    const auto adapted = palmier::media::makeRenderSourceFrame(frame(90));
    const SourceFrame background{1, 1, {{0.125F, 0.25F, 0.5F, 1}}};
    const auto plan = palmier::render::RenderPlan::create(
        adapted.width,
        adapted.height,
        30,
        0,
        {
            layer("background", "background-track", "background"),
            layer("decoded", "video-track", "decoded"),
        }
    );
    const auto resolver = [&](std::string_view mediaId, std::int64_t sourceFrame)
        -> const SourceFrame* {
        if (sourceFrame != 0) {
            return nullptr;
        }
        if (mediaId == "background") {
            return &background;
        }
        if (mediaId == "decoded") {
            return &adapted;
        }
        return nullptr;
    };

    palmier::render::CpuRenderer cpu;
    const auto cpuPreview = palmier::render::renderPreviewFrame(plan, resolver, cpu);
    const auto cpuExport = palmier::render::renderExportFrame(plan, resolver, cpu);
    require(cpuPreview.pixels == cpuExport.pixels, "CPU preview/export differ");

    palmier::render::D3d11WarpRenderer warp;
    const auto warpPreview = palmier::render::renderPreviewFrame(plan, resolver, warp);
    const auto warpExport = palmier::render::renderExportFrame(plan, resolver, warp);
    require(warpPreview.pixels == warpExport.pixels, "WARP preview/export differ");
    compareFrames(cpuPreview, warpPreview, 2e-5F);
}

}

int main() {
    try {
        cardinalRotationsAndPaddedStride();
        refusalBoundaries();
        deterministicCancellationBoundaries();
        adapterFeedsSharedPreviewAndExport();
        std::cout << "PALMIER_MEDIA_RENDER_TESTS_OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "PALMIER_MEDIA_RENDER_TESTS_FAILED " << error.what() << '\n';
        return 1;
    }
}
