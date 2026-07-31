#pragma once

#include "palmier/media/ffmpeg_media_reader.hpp"
#include "palmier/render/render_plan.hpp"

#include <stdexcept>
#include <stop_token>
#include <string>

namespace palmier::media {

class RenderSourceError final : public std::runtime_error {
public:
    RenderSourceError(std::string code, std::string pointer, std::string detail);

    const std::string code;
    const std::string pointer;
};

render::SourceFrame makeRenderSourceFrame(
    const DecodedVideoFrame& decoded,
    std::stop_token cancellation = {}
);

}
