#include "palmier/media/render_source_adapter.hpp"
#include "internal/render_source_adapter_testing.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace palmier::media {
namespace {

[[noreturn]] void fail(
    std::string code,
    std::string pointer,
    std::string detail
) {
    throw RenderSourceError(
        std::move(code),
        std::move(pointer),
        std::move(detail)
    );
}

void requireNotCancelled(std::stop_token cancellation) {
    if (cancellation.stop_requested()) {
        fail("cancelled", "/source", "render source conversion was cancelled");
    }
}

std::int16_t normalizedRotation(const DecodedVideoFrame& decoded) {
    const auto rotation = decoded.displayTransform
        ? decoded.displayTransform->counterClockwiseDegrees
        : 0;
    if (rotation != 0 && rotation != 90 && rotation != -90 && rotation != 180) {
        fail(
            "unsupportedDisplayTransform",
            "/source/displayTransform",
            "display rotation must be cardinal"
        );
    }
    return rotation;
}

std::size_t destinationIndex(
    std::uint32_t sourceX,
    std::uint32_t sourceY,
    std::uint32_t sourceWidth,
    std::uint32_t sourceHeight,
    std::int16_t counterClockwiseDegrees,
    std::uint32_t destinationWidth
) {
    std::uint32_t destinationX{};
    std::uint32_t destinationY{};
    switch (counterClockwiseDegrees) {
    case 90:
        destinationX = sourceY;
        destinationY = sourceWidth - 1 - sourceX;
        break;
    case -90:
        destinationX = sourceHeight - 1 - sourceY;
        destinationY = sourceX;
        break;
    case 180:
        destinationX = sourceWidth - 1 - sourceX;
        destinationY = sourceHeight - 1 - sourceY;
        break;
    default:
        destinationX = sourceX;
        destinationY = sourceY;
        break;
    }
    return static_cast<std::size_t>(destinationY) * destinationWidth
        + destinationX;
}

}

RenderSourceError::RenderSourceError(
    std::string codeValue,
    std::string pointerValue,
    std::string detail
) : std::runtime_error(std::move(detail)),
    code(std::move(codeValue)),
    pointer(std::move(pointerValue)) {}

render::SourceFrame makeRenderSourceFrame(
    const DecodedVideoFrame& decoded,
    std::stop_token cancellation
) {
    return detail::makeRenderSourceFrame(decoded, cancellation, {});
}

render::SourceFrame detail::makeRenderSourceFrame(
    const DecodedVideoFrame& decoded,
    std::stop_token cancellation,
    const detail::RenderSourceAdapterHooks& hooks
) {
    requireNotCancelled(cancellation);
    if (decoded.width <= 0 || decoded.height <= 0) {
        fail(
            "invalidSourceSize",
            "/source",
            "decoded dimensions must be positive"
        );
    }
    if (!isPrototypeSrgbColor(decoded.color)) {
        fail(
            "unsupportedColorMetadata",
            "/source/color",
            "decoded color metadata is outside the sRGB prototype contract"
        );
    }
    if (decoded.alphaMode != AlphaMode::opaque
        && decoded.alphaMode != AlphaMode::straight) {
        fail(
            "unsupportedAlphaMode",
            "/source/alphaMode",
            "decoded alpha must be explicitly opaque or straight"
        );
    }

    const auto rotation = normalizedRotation(decoded);
    const auto sourceWidth = static_cast<std::uint32_t>(decoded.width);
    const auto sourceHeight = static_cast<std::uint32_t>(decoded.height);
    const bool swapsDimensions = rotation == 90 || rotation == -90;
    const auto destinationWidth = swapsDimensions ? sourceHeight : sourceWidth;
    const auto destinationHeight = swapsDimensions ? sourceWidth : sourceHeight;
    try {
        render::validateSourceFrameDimensions(
            destinationWidth,
            destinationHeight,
            "/source"
        );
    } catch (const render::RenderError& error) {
        fail(error.code, error.pointer, error.what());
    }

    const auto tightRowBytes = static_cast<std::size_t>(sourceWidth) * 4;
    if (decoded.rowBytes < tightRowBytes) {
        fail(
            "invalidSourceStride",
            "/source/rowBytes",
            "decoded row stride is smaller than the visible row"
        );
    }
    if (decoded.rowBytes > std::numeric_limits<std::size_t>::max() / sourceHeight
        || decoded.rowBytes * sourceHeight != decoded.rgba8.size()) {
        fail(
            "invalidSourceStorage",
            "/source/rgba8",
            "decoded storage does not exactly match stride and height"
        );
    }

    const auto pixelCount = static_cast<std::size_t>(destinationWidth)
        * destinationHeight;
    std::vector<render::Rgba32Float> pixels;
    try {
        pixels = hooks.allocatePixels
            ? hooks.allocatePixels(pixelCount)
            : std::vector<render::Rgba32Float>(pixelCount);
    } catch (const std::bad_alloc&) {
        fail(
            "resourceExhausted",
            "/source/rgba8",
            "render source allocation failed"
        );
    }
    if (pixels.size() != pixelCount) {
        fail(
            "invalidAllocation",
            "/source/rgba8",
            "render source allocation returned the wrong pixel count"
        );
    }

    constexpr float byteScale = 1.0F / 255.0F;
    for (std::uint32_t sourceY = 0; sourceY < sourceHeight; ++sourceY) {
        requireNotCancelled(cancellation);
        const auto rowOffset = static_cast<std::size_t>(sourceY) * decoded.rowBytes;
        for (std::uint32_t sourceX = 0; sourceX < sourceWidth; ++sourceX) {
            const auto byteOffset = rowOffset + static_cast<std::size_t>(sourceX) * 4;
            const auto alpha = decoded.rgba8[byteOffset + 3];
            if (decoded.alphaMode == AlphaMode::opaque && alpha != 255) {
                fail(
                    "inconsistentOpaqueAlpha",
                    "/source/rgba8",
                    "opaque decoded frames must contain only opaque alpha bytes"
                );
            }
            const auto outputIndex = destinationIndex(
                sourceX,
                sourceY,
                sourceWidth,
                sourceHeight,
                rotation,
                destinationWidth
            );
            pixels[outputIndex] = {
                decoded.rgba8[byteOffset] * byteScale,
                decoded.rgba8[byteOffset + 1] * byteScale,
                decoded.rgba8[byteOffset + 2] * byteScale,
                alpha * byteScale,
            };
        }
        if (hooks.didConvertRow) {
            hooks.didConvertRow(sourceY);
        }
    }
    if (hooks.willPublish) {
        hooks.willPublish();
    }
    requireNotCancelled(cancellation);

    render::SourceFrame result{
        destinationWidth,
        destinationHeight,
        std::move(pixels),
    };
    render::validateSourceFrame(result, "/source");
    return result;
}

}
