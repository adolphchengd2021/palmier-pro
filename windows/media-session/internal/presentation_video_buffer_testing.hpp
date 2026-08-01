#pragma once

#include "palmier/media/presentation_video_buffer.hpp"

#include <cstdint>
#include <functional>
#include <stop_token>
#include <utility>

namespace palmier::media::detail {

class PresentationVideoBufferTestAccess final {
public:
    using FrameAdapter = std::function<render::SourceFrame(
        const DecodedVideoFrame&,
        std::stop_token
    )>;

    static PresentationVideoBuffer create(
        PresentationVideoBufferLimits limits,
        FrameAdapter frameAdapter,
        std::function<void()> adaptedFrameCheckpoint
    ) {
        return PresentationVideoBuffer(
            limits,
            std::move(frameAdapter),
            std::move(adaptedFrameCheckpoint),
            true
        );
    }

    static void setRevision(
        PresentationVideoBuffer& buffer,
        std::uint64_t revision
    ) noexcept {
        buffer.revision_ = revision;
    }
};

}
