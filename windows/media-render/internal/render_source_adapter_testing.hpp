#pragma once

#include "palmier/media/ffmpeg_media_reader.hpp"
#include "palmier/render/render_plan.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <stop_token>
#include <vector>

namespace palmier::media::detail {

struct RenderSourceAdapterHooks final {
    std::function<std::vector<render::Rgba32Float>(std::size_t)> allocatePixels;
    std::function<void(std::uint32_t)> didConvertRow;
    std::function<void()> willPublish;
};

render::SourceFrame makeRenderSourceFrame(
    const DecodedVideoFrame& decoded,
    std::stop_token cancellation,
    const RenderSourceAdapterHooks& hooks
);

}
