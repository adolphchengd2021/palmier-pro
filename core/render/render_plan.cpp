#include "palmier/render/render_plan.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <set>
#include <utility>

namespace palmier::render {
namespace {

constexpr std::uint32_t maximumTextureDimension = 16'384;
constexpr std::uint64_t maximumFramePixels = 3'840ULL * 2'160ULL;
constexpr std::uint64_t maximumCompositeSamples = 67'108'864;
constexpr std::uint64_t maximumResolvedSourcePixels = 67'108'864;
constexpr std::size_t maximumLayerCount = 256;

[[noreturn]] void fail(std::string code, std::string pointer, std::string detail) {
    throw RenderError(std::move(code), std::move(pointer), std::move(detail));
}

void requireFinite(float value, const std::string& pointer) {
    if (!std::isfinite(value)) {
        fail("nonFiniteValue", pointer, "render value must be finite");
    }
}

}

RenderError::RenderError(std::string codeValue, std::string pointerValue, std::string detail)
    : std::runtime_error(std::move(detail)),
      code(std::move(codeValue)),
      pointer(std::move(pointerValue)) {}

RenderPlan RenderPlan::create(
    std::uint32_t canvasWidth,
    std::uint32_t canvasHeight,
    std::int32_t fps,
    std::int64_t timelineFrame,
    std::vector<RenderLayer> layers
) {
    if (
        canvasWidth == 0
        || canvasHeight == 0
        || canvasWidth > maximumTextureDimension
        || canvasHeight > maximumTextureDimension
    ) {
        fail(
            "invalidCanvasSize",
            "/canvas",
            "canvas dimensions must be within the D3D11 feature-level 11 limit"
        );
    }
    const auto canvasPixels = static_cast<std::uint64_t>(canvasWidth) * canvasHeight;
    if (canvasPixels > maximumFramePixels) {
        fail(
            "canvasBudgetExceeded",
            "/canvas",
            "the synthetic reference frame is limited to 8294400 pixels"
        );
    }
    if (fps <= 0) {
        fail("invalidFrameRate", "/fps", "frame rate must be positive");
    }
    if (timelineFrame < 0) {
        fail("invalidTimelineFrame", "/timelineFrame", "timeline frame must be non-negative");
    }
    if (layers.size() > maximumLayerCount) {
        fail("layerBudgetExceeded", "/layers", "a render plan is limited to 256 layers");
    }
    if (
        !layers.empty()
        && canvasPixels > maximumCompositeSamples / layers.size()
    ) {
        fail(
            "compositeBudgetExceeded",
            "/layers",
            "canvas pixels multiplied by layers must not exceed 67108864"
        );
    }

    std::set<std::string> layerIds;
    std::set<std::string> trackIds;
    for (std::size_t index = 0; index < layers.size(); ++index) {
        const auto pointer = "/layers/" + std::to_string(index);
        auto& layer = layers[index];
        if (layer.id.empty()) {
            fail("missingStableId", pointer + "/id", "layer ID must not be empty");
        }
        if (!layerIds.insert(layer.id).second) {
            fail("duplicateStableId", pointer + "/id", "layer ID must be unique");
        }
        if (layer.trackId.empty()) {
            fail("missingStableId", pointer + "/trackId", "track ID must not be empty");
        }
        if (!trackIds.insert(layer.trackId).second) {
            fail(
                "overlappingTrackLayers",
                pointer + "/trackId",
                "one resolved frame cannot contain overlapping layers from one track"
            );
        }
        if (layer.mediaId.empty()) {
            fail("missingStableId", pointer + "/mediaId", "media ID must not be empty");
        }
        if (layer.sourceFrame < 0) {
            fail("invalidSourceFrame", pointer + "/sourceFrame", "source frame must be non-negative");
        }
        requireFinite(layer.transform.centerX, pointer + "/transform/centerX");
        requireFinite(layer.transform.centerY, pointer + "/transform/centerY");
        requireFinite(layer.transform.width, pointer + "/transform/width");
        requireFinite(layer.transform.height, pointer + "/transform/height");
        requireFinite(
            layer.transform.rotationDegrees,
            pointer + "/transform/rotationDegrees"
        );
        layer.transform.rotationDegrees = std::remainder(
            layer.transform.rotationDegrees,
            360.0F
        );
        if (layer.transform.width <= 0 || layer.transform.height <= 0) {
            fail(
                "invalidTransformSize",
                pointer + "/transform",
                "transform width and height must be positive"
            );
        }
        requireFinite(layer.opacity, pointer + "/opacity");
        if (layer.opacity < 0 || layer.opacity > 1) {
            fail("invalidOpacity", pointer + "/opacity", "resolved opacity must be in 0...1");
        }
        if (layer.exposureEv) {
            requireFinite(*layer.exposureEv, pointer + "/exposureEv");
            if (*layer.exposureEv < -3 || *layer.exposureEv > 3) {
                fail(
                    "invalidExposure",
                    pointer + "/exposureEv",
                    "resolved exposure must be in -3...3 EV"
                );
            }
        }
    }
    return RenderPlan(
        canvasWidth,
        canvasHeight,
        fps,
        timelineFrame,
        std::move(layers)
    );
}

RenderPlan::RenderPlan(
    std::uint32_t canvasWidth,
    std::uint32_t canvasHeight,
    std::int32_t fps,
    std::int64_t timelineFrame,
    std::vector<RenderLayer> layers
) : canvasWidth_(canvasWidth),
    canvasHeight_(canvasHeight),
    fps_(fps),
    timelineFrame_(timelineFrame),
    layers_(std::move(layers)) {}

std::uint32_t RenderPlan::canvasWidth() const noexcept { return canvasWidth_; }
std::uint32_t RenderPlan::canvasHeight() const noexcept { return canvasHeight_; }
std::int32_t RenderPlan::fps() const noexcept { return fps_; }
std::int64_t RenderPlan::timelineFrame() const noexcept { return timelineFrame_; }
const std::vector<RenderLayer>& RenderPlan::layers() const noexcept { return layers_; }

RenderedFrame renderPreviewFrame(
    const RenderPlan& plan,
    const FrameResolver& resolveFrame,
    RenderBackend& backend
) {
    return backend.render(plan, resolveFrame);
}

RenderedFrame renderExportFrame(
    const RenderPlan& plan,
    const FrameResolver& resolveFrame,
    RenderBackend& backend
) {
    return backend.render(plan, resolveFrame);
}

void validateSourceFrameDimensions(
    std::uint32_t width,
    std::uint32_t height,
    std::string_view pointerValue
) {
    const auto pointer = std::string(pointerValue);
    if (
        width == 0
        || height == 0
        || width > maximumTextureDimension
        || height > maximumTextureDimension
    ) {
        fail("invalidSourceSize", pointer, "source dimensions are invalid");
    }
    const auto expected = static_cast<std::uint64_t>(width) * height;
    if (expected > maximumFramePixels) {
        fail(
            "sourceBudgetExceeded",
            pointer,
            "the synthetic reference source is limited to 8294400 pixels"
        );
    }
}

void validateSourceFrame(const SourceFrame& frame, std::string_view layerPointer) {
    const auto pointer = std::string(layerPointer);
    validateSourceFrameDimensions(frame.width, frame.height, pointer);
    const auto expected = static_cast<std::uint64_t>(frame.width) * frame.height;
    if (expected != frame.pixels.size()) {
        fail("invalidSourceStorage", pointer, "source pixel count does not match dimensions");
    }
    for (const auto& pixel : frame.pixels) {
        const double values[]{pixel.red, pixel.green, pixel.blue, pixel.alpha};
        for (const auto value : values) {
            if (!std::isfinite(value) || value < 0 || value > 1) {
                fail(
                    "invalidSourcePixel",
                    pointer,
                    "source pixels must be finite straight-alpha sRGB values in 0...1"
                );
            }
        }
    }
}

std::vector<const SourceFrame*> resolveAndValidateSourceFrames(
    const RenderPlan& plan,
    const FrameResolver& resolveFrame
) {
    std::vector<const SourceFrame*> sources;
    try {
        sources.reserve(plan.layers().size());
    } catch (const std::bad_alloc&) {
        fail("resourceExhausted", "/layers", "source resolution allocation failed");
    }

    std::uint64_t resolvedSourcePixels = 0;
    for (std::size_t index = 0; index < plan.layers().size(); ++index) {
        const auto& layer = plan.layers()[index];
        const auto pointer = "/layers/" + std::to_string(index);
        const auto* source = resolveFrame(layer.mediaId, layer.sourceFrame);
        if (!source) {
            fail(
                "missingSourceFrame",
                pointer + "/mediaId",
                "render source frame is unavailable"
            );
        }
        if (std::find(sources.begin(), sources.end(), source) == sources.end()) {
            validateSourceFrame(*source, pointer);
        }
        const auto sourcePixels = static_cast<std::uint64_t>(source->width)
            * source->height;
        if (sourcePixels > maximumResolvedSourcePixels - resolvedSourcePixels) {
            fail(
                "sourceWorkBudgetExceeded",
                "/layers",
                "resolved source pixels must not exceed 67108864 per render"
            );
        }
        resolvedSourcePixels += sourcePixels;
        sources.push_back(source);
    }
    return sources;
}

}
