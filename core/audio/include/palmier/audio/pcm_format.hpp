#pragma once

#include <bit>
#include <cstdint>
#include <limits>

namespace palmier::audio {

enum class PcmSampleEncoding {
    unknown,
    integer,
    ieeeFloat,
};

struct PcmFormat final {
    std::uint32_t sampleRate{};
    std::uint16_t channelCount{};
    std::uint16_t containerBitsPerSample{};
    std::uint16_t validBitsPerSample{};
    std::uint16_t blockAlign{};
    std::uint32_t channelMask{};
    PcmSampleEncoding encoding{PcmSampleEncoding::unknown};
    bool interleaved{true};

    friend bool operator==(const PcmFormat&, const PcmFormat&) = default;
};

constexpr bool isValidPcmFormat(const PcmFormat& format) noexcept {
    if (format.sampleRate == 0 || format.sampleRate > 384'000
        || format.channelCount == 0 || format.channelCount > 32
        || format.containerBitsPerSample == 0
        || format.containerBitsPerSample % 8 != 0
        || format.validBitsPerSample == 0
        || format.validBitsPerSample > format.containerBitsPerSample
        || format.encoding == PcmSampleEncoding::unknown
        || !format.interleaved
        || (format.channelCount > 2 && format.channelMask == 0)
        || (format.channelMask != 0
            && std::popcount(format.channelMask) != format.channelCount)) {
        return false;
    }
    if (format.encoding == PcmSampleEncoding::ieeeFloat
        && (format.validBitsPerSample != format.containerBitsPerSample
            || (format.containerBitsPerSample != 32
                && format.containerBitsPerSample != 64))) {
        return false;
    }
    const auto bytesPerFrame = static_cast<std::uint32_t>(format.channelCount)
        * (format.containerBitsPerSample / 8);
    return bytesPerFrame <= std::numeric_limits<std::uint16_t>::max()
        && format.blockAlign == bytesPerFrame;
}

}
