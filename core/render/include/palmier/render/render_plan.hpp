#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace palmier::render {

struct Rgba32Float final {
    float red;
    float green;
    float blue;
    float alpha;

    bool operator==(const Rgba32Float&) const = default;
};

struct SourceFrame final {
    std::uint32_t width;
    std::uint32_t height;
    std::vector<Rgba32Float> pixels;
};

struct RenderedFrame final {
    std::uint32_t width;
    std::uint32_t height;
    std::vector<Rgba32Float> pixels;
};

enum class BlendMode {
    normal,
};

struct Transform2D final {
    float centerX = 0.5F;
    float centerY = 0.5F;
    float width = 1;
    float height = 1;
    float rotationDegrees = 0;
};

struct RenderLayer final {
    std::string id;
    std::string trackId;
    std::string mediaId;
    std::int64_t sourceFrame = 0;
    Transform2D transform;
    float opacity = 1;
    BlendMode blendMode = BlendMode::normal;
    std::optional<float> exposureEv;
};

class RenderError final : public std::runtime_error {
public:
    RenderError(std::string code, std::string pointer, std::string detail);

    const std::string code;
    const std::string pointer;
};

class RenderPlan final {
public:
    static RenderPlan create(
        std::uint32_t canvasWidth,
        std::uint32_t canvasHeight,
        std::int32_t fps,
        std::int64_t timelineFrame,
        std::vector<RenderLayer> layers
    );

    std::uint32_t canvasWidth() const noexcept;
    std::uint32_t canvasHeight() const noexcept;
    std::int32_t fps() const noexcept;
    std::int64_t timelineFrame() const noexcept;
    const std::vector<RenderLayer>& layers() const noexcept;

private:
    RenderPlan(
        std::uint32_t canvasWidth,
        std::uint32_t canvasHeight,
        std::int32_t fps,
        std::int64_t timelineFrame,
        std::vector<RenderLayer> layers
    );

    std::uint32_t canvasWidth_;
    std::uint32_t canvasHeight_;
    std::int32_t fps_;
    std::int64_t timelineFrame_;
    std::vector<RenderLayer> layers_;
};

using FrameResolver = std::function<const SourceFrame*(std::string_view, std::int64_t)>;

class RenderBackend {
public:
    virtual ~RenderBackend() = default;
    virtual RenderedFrame render(
        const RenderPlan& plan,
        const FrameResolver& resolveFrame
    ) = 0;
};

RenderedFrame renderPreviewFrame(
    const RenderPlan& plan,
    const FrameResolver& resolveFrame,
    RenderBackend& backend
);

RenderedFrame renderExportFrame(
    const RenderPlan& plan,
    const FrameResolver& resolveFrame,
    RenderBackend& backend
);

void validateSourceFrame(const SourceFrame& frame, std::string_view layerPointer);

std::vector<const SourceFrame*> resolveAndValidateSourceFrames(
    const RenderPlan& plan,
    const FrameResolver& resolveFrame
);

}
