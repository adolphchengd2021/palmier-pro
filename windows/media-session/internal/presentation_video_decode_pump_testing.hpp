#pragma once

#include "palmier/media/presentation_video_decode_pump.hpp"

#include <utility>

namespace palmier::media::detail {

class PresentationVideoDecodePumpTestAccess final {
public:
    static PresentationVideoDecodePump create(
        PresentationVideoDecodeLimits limits,
        PresentationVideoDecodePump::StartCommitCheckpoint checkpoint
    ) {
        return PresentationVideoDecodePump(
            limits,
            std::move(checkpoint)
        );
    }
};

}
