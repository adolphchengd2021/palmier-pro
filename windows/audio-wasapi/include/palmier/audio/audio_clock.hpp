#pragma once

#include <cstdint>
#include <stdexcept>

namespace palmier::audio {

struct FrameRate final {
    std::uint32_t numerator{};
    std::uint32_t denominator{};
};

struct AudioClockAnchor final {
    std::uint64_t generation{};
    std::uint64_t devicePosition{};
    std::uint64_t frequency{};
    std::int64_t timelineFrame{};
};

struct AudioClockSample final {
    std::uint64_t generation{};
    std::uint64_t devicePosition{};
    std::uint64_t qpc100Nanoseconds{};
    bool precisionDegraded{};
};

enum class AudioClockFailureCode {
    invalidFrameRate,
    invalidFrequency,
    invalidAnchorFrame,
    staleGeneration,
    positionDiscontinuity,
    arithmeticOverflow,
};

class AudioClockError final : public std::runtime_error {
public:
    explicit AudioClockError(AudioClockFailureCode code);

    AudioClockFailureCode code;
};

std::int64_t timelineFrame(
    const AudioClockAnchor& anchor,
    const AudioClockSample& sample,
    FrameRate frameRate
);

}
