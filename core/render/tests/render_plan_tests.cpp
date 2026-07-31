#include "palmier/render/cpu_renderer.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using palmier::render::RenderError;
using palmier::render::RenderLayer;
using palmier::render::RenderPlan;
using palmier::render::Rgba32Float;
using palmier::render::SourceFrame;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void requireNear(float actual, float expected, const std::string& message) {
    if (std::abs(actual - expected) > 1e-5F) {
        throw std::runtime_error(message);
    }
}

template<typename Operation>
void requireError(Operation operation, const std::string& code) {
    try {
        operation();
    } catch (const RenderError& error) {
        require(error.code == code, "unexpected render error code");
        return;
    }
    throw std::runtime_error("expected render failure");
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

void validationBoundaries() {
    requireError(
        [] { RenderPlan::create(0, 8, 30, 0, {}); },
        "invalidCanvasSize"
    );
    requireError(
        [] { RenderPlan::create(8, 8, 0, 0, {}); },
        "invalidFrameRate"
    );
    requireError(
        [] { RenderPlan::create(8, 8, 30, -1, {}); },
        "invalidTimelineFrame"
    );

    auto invalidOpacity = layer("clip", "track", "media");
    invalidOpacity.opacity = 1.1F;
    requireError(
        [&] { RenderPlan::create(8, 8, 30, 0, {invalidOpacity}); },
        "invalidOpacity"
    );

    auto invalidExposure = layer("clip", "track", "media");
    invalidExposure.exposureEv = 3.1F;
    requireError(
        [&] { RenderPlan::create(8, 8, 30, 0, {invalidExposure}); },
        "invalidExposure"
    );

    auto invalidTransform = layer("clip", "track", "media");
    invalidTransform.transform.centerX = std::numeric_limits<float>::quiet_NaN();
    requireError(
        [&] { RenderPlan::create(8, 8, 30, 0, {invalidTransform}); },
        "nonFiniteValue"
    );

    auto first = layer("clip-1", "track", "media-1");
    auto second = layer("clip-2", "track", "media-2");
    requireError(
        [&] { RenderPlan::create(8, 8, 30, 0, {first, second}); },
        "overlappingTrackLayers"
    );

    auto duplicateId = layer("clip-1", "other-track", "media-2");
    requireError(
        [&] { RenderPlan::create(8, 8, 30, 0, {first, duplicateId}); },
        "duplicateStableId"
    );

    requireError(
        [] { RenderPlan::create(3'841, 2'160, 30, 0, {}); },
        "canvasBudgetExceeded"
    );

    std::vector<RenderLayer> tooManyLayers;
    for (int index = 0; index < 257; ++index) {
        tooManyLayers.push_back(
            layer(
                "clip-" + std::to_string(index),
                "track-" + std::to_string(index),
                "media-" + std::to_string(index)
            )
        );
    }
    requireError(
        [&] { RenderPlan::create(8, 8, 30, 0, std::move(tooManyLayers)); },
        "layerBudgetExceeded"
    );

    std::vector<RenderLayer> expensiveLayers;
    for (int index = 0; index < 9; ++index) {
        expensiveLayers.push_back(
            layer(
                "expensive-clip-" + std::to_string(index),
                "expensive-track-" + std::to_string(index),
                "expensive-media-" + std::to_string(index)
            )
        );
    }
    requireError(
        [&] { RenderPlan::create(3'840, 2'160, 30, 0, std::move(expensiveLayers)); },
        "compositeBudgetExceeded"
    );
}

void sourceFailuresAreObservable() {
    const auto plan = RenderPlan::create(1, 1, 30, 0, {layer("clip", "track", "media")});
    palmier::render::CpuRenderer renderer;
    const auto missing = [](std::string_view, std::int64_t) -> const SourceFrame* {
        return nullptr;
    };
    requireError(
        [&] { renderer.render(plan, missing); },
        "missingSourceFrame"
    );

    const SourceFrame invalid{1, 1, {{0, 0, 0, std::numeric_limits<float>::infinity()}}};
    const auto invalidResolver = [&](std::string_view, std::int64_t) { return &invalid; };
    requireError(
        [&] { renderer.render(plan, invalidResolver); },
        "invalidSourcePixel"
    );

    const SourceFrame oversized{3'841, 2'160, {}};
    requireError(
        [&] { palmier::render::validateSourceFrame(oversized, "/source"); },
        "sourceBudgetExceeded"
    );
}

void extremeFiniteTransformIsCanonicalAndSafe() {
    auto extremeRotation = layer("rotation", "track-rotation", "media");
    extremeRotation.transform.rotationDegrees = std::numeric_limits<float>::max();
    const auto normalized = RenderPlan::create(1, 1, 30, 0, {extremeRotation});
    require(
        std::isfinite(normalized.layers()[0].transform.rotationDegrees),
        "normalized rotation must remain finite"
    );
    require(
        std::abs(normalized.layers()[0].transform.rotationDegrees) <= 180.0F,
        "normalized rotation must use the canonical range"
    );

    const SourceFrame source{1, 1, {{1, 0, 0, 1}}};
    auto extremePlacement = layer("placement", "track-placement", "media");
    extremePlacement.transform.centerX = std::numeric_limits<float>::max();
    extremePlacement.transform.width = std::numeric_limits<float>::min();
    const auto plan = RenderPlan::create(1, 1, 30, 0, {extremePlacement});
    const auto resolver = [&](std::string_view, std::int64_t) { return &source; };
    palmier::render::CpuRenderer renderer;
    const auto output = renderer.render(plan, resolver);
    require(output.pixels[0] == Rgba32Float{0, 0, 0, 1}, "extreme placement stays outside");
}

void resolvedSourceWorkIsBoundedBeforeRendering() {
    std::vector<RenderLayer> layers;
    for (int index = 0; index < 256; ++index) {
        layers.push_back(
            layer(
                "source-clip-" + std::to_string(index),
                "source-track-" + std::to_string(index),
                "shared-media"
            )
        );
    }
    const auto plan = RenderPlan::create(1, 1, 30, 0, std::move(layers));
    const SourceFrame source{
        17,
        15'421,
        std::vector<Rgba32Float>(17ULL * 15'421ULL, {0, 0, 0, 1}),
    };
    const auto resolver = [&](std::string_view, std::int64_t) { return &source; };
    requireError(
        [&] { palmier::render::resolveAndValidateSourceFrames(plan, resolver); },
        "sourceWorkBudgetExceeded"
    );
}

void cpuReferencePipeline() {
    const SourceFrame bottom{
        1,
        1,
        {{0.25F, 0.5F, 0.75F, 1}},
    };
    const SourceFrame top{
        1,
        1,
        {{0.5F, 0.25F, 0.125F, 0.5F}},
    };
    auto bottomLayer = layer("bottom", "track-bottom", "bottom-media");
    bottomLayer.exposureEv = 1.0F;
    auto topLayer = layer("top", "track-top", "top-media");
    topLayer.opacity = 0.5F;
    const auto plan = RenderPlan::create(
        2,
        2,
        30,
        7,
        {bottomLayer, topLayer}
    );
    const auto resolver = [&](std::string_view mediaId, std::int64_t) -> const SourceFrame* {
        if (mediaId == "bottom-media") {
            return &bottom;
        }
        if (mediaId == "top-media") {
            return &top;
        }
        return nullptr;
    };
    palmier::render::CpuRenderer renderer;
    const auto preview = palmier::render::renderPreviewFrame(plan, resolver, renderer);
    const auto exported = palmier::render::renderExportFrame(plan, resolver, renderer);
    require(preview.pixels == exported.pixels, "preview and export CPU frames differ");
    require(preview.width == 2 && preview.height == 2, "CPU output dimensions");
    for (const auto& pixel : preview.pixels) {
        requireNear(pixel.red, 0.38909462F, "CPU red pipeline result");
        requireNear(pixel.green, 0.57687712F, "CPU green pipeline result");
        requireNear(pixel.blue, 0.79590958F, "CPU blue pipeline result");
        require(pixel.alpha == 1, "opaque black background must keep final alpha opaque");
        require(
            std::isfinite(pixel.red) && std::isfinite(pixel.green) && std::isfinite(pixel.blue),
            "CPU output must remain finite"
        );
    }
}

void transformedLayerUsesTopLeftCanvasCoordinates() {
    const SourceFrame source{1, 1, {{1, 0, 0, 1}}};
    auto transformed = layer("clip", "track", "media");
    transformed.transform = {0.75F, 0.75F, 0.5F, 0.5F, 0};
    const auto plan = RenderPlan::create(4, 4, 30, 0, {transformed});
    const auto resolver = [&](std::string_view, std::int64_t) { return &source; };
    palmier::render::CpuRenderer renderer;
    const auto output = renderer.render(plan, resolver);
    const auto at = [&](std::uint32_t x, std::uint32_t y) -> const Rgba32Float& {
        return output.pixels[static_cast<std::size_t>(y) * output.width + x];
    };
    require(at(0, 0).red == 0, "top-left remains black");
    requireNear(at(2, 2).red, 1, "bottom-right first covered pixel");
    requireNear(at(3, 3).red, 1, "bottom-right last covered pixel");
}

}

int main() {
    try {
        validationBoundaries();
        sourceFailuresAreObservable();
        extremeFiniteTransformIsCanonicalAndSafe();
        resolvedSourceWorkIsBoundedBeforeRendering();
        cpuReferencePipeline();
        transformedLayerUsesTopLeftCanvasCoordinates();
        std::cout << "PALMIER_RENDER_PLAN_TESTS_OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "PALMIER_RENDER_PLAN_TESTS_FAILED " << error.what() << '\n';
        return 1;
    }
}
