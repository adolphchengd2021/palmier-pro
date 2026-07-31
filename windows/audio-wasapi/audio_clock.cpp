#include "palmier/audio/audio_clock.hpp"

#include <intrin.h>

#include <limits>
#include <numeric>

namespace palmier::audio {
namespace {

[[noreturn]] void fail(AudioClockFailureCode code) {
    throw AudioClockError(code);
}

std::uint64_t checkedProduct(std::uint64_t lhs, std::uint64_t rhs) {
    if (lhs != 0 && rhs > std::numeric_limits<std::uint64_t>::max() / lhs) {
        fail(AudioClockFailureCode::arithmeticOverflow);
    }
    return lhs * rhs;
}

std::uint64_t multiplyDivideFloor(
    std::uint64_t lhs,
    std::uint64_t rhs,
    std::uint64_t divisor
) {
    unsigned __int64 high = 0;
    const unsigned __int64 low = _umul128(lhs, rhs, &high);
    if (high >= divisor) {
        fail(AudioClockFailureCode::arithmeticOverflow);
    }
    unsigned __int64 remainder = 0;
    return _udiv128(high, low, divisor, &remainder);
}

}

AudioClockError::AudioClockError(AudioClockFailureCode codeValue)
    : std::runtime_error("audio clock conversion failed"), code(codeValue) {}

std::int64_t timelineFrame(
    const AudioClockAnchor& anchor,
    const AudioClockSample& sample,
    FrameRate frameRate
) {
    if (frameRate.numerator == 0 || frameRate.denominator == 0) {
        fail(AudioClockFailureCode::invalidFrameRate);
    }
    if (anchor.frequency == 0) {
        fail(AudioClockFailureCode::invalidFrequency);
    }
    if (anchor.timelineFrame < 0) {
        fail(AudioClockFailureCode::invalidAnchorFrame);
    }
    if (sample.generation != anchor.generation) {
        fail(AudioClockFailureCode::staleGeneration);
    }
    if (sample.devicePosition < anchor.devicePosition) {
        fail(AudioClockFailureCode::positionDiscontinuity);
    }

    std::uint64_t positionDelta = sample.devicePosition - anchor.devicePosition;
    std::uint64_t fpsNumerator = frameRate.numerator;
    std::uint64_t frequency = anchor.frequency;
    std::uint64_t fpsDenominator = frameRate.denominator;

    auto divisor = std::gcd(positionDelta, frequency);
    positionDelta /= divisor;
    frequency /= divisor;
    divisor = std::gcd(positionDelta, fpsDenominator);
    positionDelta /= divisor;
    fpsDenominator /= divisor;
    divisor = std::gcd(fpsNumerator, frequency);
    fpsNumerator /= divisor;
    frequency /= divisor;
    divisor = std::gcd(fpsNumerator, fpsDenominator);
    fpsNumerator /= divisor;
    fpsDenominator /= divisor;

    const std::uint64_t denominator = checkedProduct(frequency, fpsDenominator);
    const std::uint64_t elapsedFrames = multiplyDivideFloor(
        positionDelta,
        fpsNumerator,
        denominator
    );
    const auto maximumAddition = static_cast<std::uint64_t>(
        std::numeric_limits<std::int64_t>::max() - anchor.timelineFrame
    );
    if (elapsedFrames > maximumAddition) {
        fail(AudioClockFailureCode::arithmeticOverflow);
    }
    return anchor.timelineFrame + static_cast<std::int64_t>(elapsedFrames);
}

}
