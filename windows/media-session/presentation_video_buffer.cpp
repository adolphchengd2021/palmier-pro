#include "palmier/media/presentation_video_buffer.hpp"

#include <intrin.h>

#include <array>
#include <limits>
#include <numeric>
#include <utility>

namespace palmier::media {
namespace {

[[noreturn]] void fail(PresentationVideoErrorCode code, const char* message) {
    throw PresentationVideoError(code, message);
}

bool sameTimeBase(const Rational& lhs, const Rational& rhs) noexcept {
    return lhs.numerator == rhs.numerator && lhs.denominator == rhs.denominator;
}

struct Unsigned128 final {
    std::uint64_t high{};
    std::uint64_t low{};
};

struct FloorFraction final {
    std::int64_t floor{};
    Unsigned128 remainder{};
    Unsigned128 denominator{0, 1};
};

struct Unsigned256 final {
    std::array<std::uint64_t, 4> limbs{};
};

Unsigned128 unsignedProduct(std::uint64_t lhs, std::uint64_t rhs) noexcept {
    Unsigned128 value;
    value.low = _umul128(lhs, rhs, &value.high);
    return value;
}

bool lessThan(const Unsigned128& lhs, const Unsigned128& rhs) noexcept {
    return lhs.high < rhs.high
        || (lhs.high == rhs.high && lhs.low < rhs.low);
}

bool isZero(const Unsigned128& value) noexcept {
    return value.high == 0 && value.low == 0;
}

Unsigned128 subtract(const Unsigned128& lhs, const Unsigned128& rhs) noexcept {
    const auto low = lhs.low - rhs.low;
    const auto borrow = lhs.low < rhs.low ? 1ULL : 0ULL;
    return {lhs.high - rhs.high - borrow, low};
}

void addLimb(
    Unsigned256& value,
    std::size_t index,
    std::uint64_t addition
) noexcept {
    while (addition != 0 && index < value.limbs.size()) {
        const auto previous = value.limbs[index];
        value.limbs[index] += addition;
        addition = value.limbs[index] < previous ? 1ULL : 0ULL;
        ++index;
    }
}

Unsigned256 wideProduct(
    const Unsigned128& lhs,
    const Unsigned128& rhs
) noexcept {
    Unsigned256 value;
    const std::array<std::uint64_t, 2> left{lhs.low, lhs.high};
    const std::array<std::uint64_t, 2> right{rhs.low, rhs.high};
    for (std::size_t leftIndex = 0; leftIndex < left.size(); ++leftIndex) {
        for (std::size_t rightIndex = 0; rightIndex < right.size(); ++rightIndex) {
            const auto product = unsignedProduct(
                left[leftIndex],
                right[rightIndex]
            );
            const auto resultIndex = leftIndex + rightIndex;
            addLimb(value, resultIndex, product.low);
            addLimb(value, resultIndex + 1, product.high);
        }
    }
    return value;
}

bool lessThan(const Unsigned256& lhs, const Unsigned256& rhs) noexcept {
    for (std::size_t index = lhs.limbs.size(); index > 0; --index) {
        if (lhs.limbs[index - 1] != rhs.limbs[index - 1]) {
            return lhs.limbs[index - 1] < rhs.limbs[index - 1];
        }
    }
    return false;
}

struct UnsignedDivision final {
    Unsigned128 quotient{};
    Unsigned128 remainder{};
};

UnsignedDivision divide(
    const Unsigned128& numerator,
    const Unsigned128& denominator
) {
    if (isZero(denominator)) {
        fail(
            PresentationVideoErrorCode::clockArithmeticOverflow,
            "video clock denominator is zero"
        );
    }
    if (denominator.high == 0 && numerator.high < denominator.low) {
        std::uint64_t remainder{};
        return {
            {0, _udiv128(
                numerator.high,
                numerator.low,
                denominator.low,
                &remainder
            )},
            {0, remainder},
        };
    }

    UnsignedDivision result;
    for (int bit = 127; bit >= 0; --bit) {
        const auto overflow = (result.remainder.high >> 63) != 0;
        result.remainder.high = (result.remainder.high << 1)
            | (result.remainder.low >> 63);
        result.remainder.low <<= 1;
        if (bit >= 64) {
            result.remainder.low |= (numerator.high >> (bit - 64)) & 1ULL;
        } else {
            result.remainder.low |= (numerator.low >> bit) & 1ULL;
        }
        if (overflow || !lessThan(result.remainder, denominator)) {
            result.remainder = subtract(result.remainder, denominator);
            if (bit >= 64) {
                result.quotient.high |= 1ULL << (bit - 64);
            } else {
                result.quotient.low |= 1ULL << bit;
            }
        }
    }
    return result;
}

std::uint64_t checkedUnsignedProduct(std::uint64_t lhs, std::uint64_t rhs) {
    if (lhs != 0 && rhs > (std::numeric_limits<std::uint64_t>::max)() / lhs) {
        fail(
            PresentationVideoErrorCode::clockArithmeticOverflow,
            "video clock denominator overflows"
        );
    }
    return lhs * rhs;
}

std::int64_t checkedClockSum(std::int64_t lhs, std::int64_t rhs) {
    if ((rhs > 0 && lhs > (std::numeric_limits<std::int64_t>::max)() - rhs)
        || (rhs < 0
            && lhs < (std::numeric_limits<std::int64_t>::min)() - rhs)) {
        fail(
            PresentationVideoErrorCode::clockArithmeticOverflow,
            "video clock sum overflows"
        );
    }
    return lhs + rhs;
}

std::uint64_t unsignedMagnitude(std::int64_t value) noexcept {
    return value >= 0
        ? static_cast<std::uint64_t>(value)
        : 0 - static_cast<std::uint64_t>(value);
}

FloorFraction floorFraction(
    Unsigned128 numerator,
    Unsigned128 denominator,
    bool negative
) {
    const auto division = divide(numerator, denominator);
    if (division.quotient.high != 0) {
        fail(
            PresentationVideoErrorCode::clockArithmeticOverflow,
            "video presentation timestamp overflows"
        );
    }
    const auto quotient = division.quotient.low;
    const auto remainder = division.remainder;
    constexpr auto signedMaximum = static_cast<std::uint64_t>(
        (std::numeric_limits<std::int64_t>::max)()
    );
    constexpr auto signedMinimumMagnitude = signedMaximum + 1;
    if (!negative) {
        if (quotient > signedMaximum) {
            fail(
                PresentationVideoErrorCode::clockArithmeticOverflow,
                "video presentation timestamp overflows"
            );
        }
        return {
            static_cast<std::int64_t>(quotient),
            remainder,
            denominator,
        };
    }
    if (isZero(remainder)) {
        if (quotient > signedMinimumMagnitude) {
            fail(
                PresentationVideoErrorCode::clockArithmeticOverflow,
                "video presentation timestamp overflows"
            );
        }
        const auto floor = quotient == signedMinimumMagnitude
            ? (std::numeric_limits<std::int64_t>::min)()
            : -static_cast<std::int64_t>(quotient);
        return {floor, {}, denominator};
    }
    if (quotient > signedMaximum) {
        fail(
            PresentationVideoErrorCode::clockArithmeticOverflow,
            "video presentation timestamp overflows"
        );
    }
    return {
        -static_cast<std::int64_t>(quotient) - 1,
        subtract(denominator, remainder),
        denominator,
    };
}

bool fractionSumCarries(
    const FloorFraction& lhs,
    const FloorFraction& rhs
) noexcept {
    const auto left = wideProduct(lhs.remainder, rhs.denominator);
    const auto right = wideProduct(
        subtract(rhs.denominator, rhs.remainder),
        lhs.denominator
    );
    return !lessThan(left, right);
}

void cancelFactor(std::uint64_t& numerator, std::uint64_t& denominator) {
    const auto divisor = std::gcd(numerator, denominator);
    numerator /= divisor;
    denominator /= divisor;
}

std::int64_t targetTimelineFrame(
    const PresentationVideoClockPosition& clock
) {
    try {
        return audio::timelineFrame(
            clock.deviceAnchor,
            clock.deviceSample,
            clock.timelineFrameRate
        );
    } catch (const audio::AudioClockError& error) {
        switch (error.code) {
        case audio::AudioClockFailureCode::positionDiscontinuity:
            fail(
                PresentationVideoErrorCode::clockPositionDiscontinuity,
                "video clock position precedes its anchor"
            );
        case audio::AudioClockFailureCode::arithmeticOverflow:
            fail(
                PresentationVideoErrorCode::clockArithmeticOverflow,
                "video timeline frame overflows"
            );
        default:
            fail(
                PresentationVideoErrorCode::invalidClock,
                "video selection clock is invalid"
            );
        }
    }
}

std::int64_t targetPresentationTimestamp(
    const PresentationVideoClockPosition& clock,
    const Rational& videoTimeBase
) {
    if (clock.sourceTimeBase.numerator <= 0
        || clock.sourceTimeBase.denominator <= 0) {
        fail(
            PresentationVideoErrorCode::invalidClockSourceTimeBase,
            "video selection source time base must be positive"
        );
    }
    if (videoTimeBase.numerator <= 0 || videoTimeBase.denominator <= 0) {
        fail(
            PresentationVideoErrorCode::invalidTimeBase,
            "queued video time base must be positive"
        );
    }
    if (clock.deviceAnchor.frequency == 0) {
        fail(
            PresentationVideoErrorCode::invalidClock,
            "video selection clock frequency must be positive"
        );
    }
    if (clock.deviceSample.devicePosition < clock.deviceAnchor.devicePosition) {
        fail(
            PresentationVideoErrorCode::clockPositionDiscontinuity,
            "video clock position precedes its anchor"
        );
    }

    const auto sourceNumerator = static_cast<std::uint64_t>(
        clock.sourceTimeBase.numerator
    );
    const auto sourceDenominator = static_cast<std::uint64_t>(
        clock.sourceTimeBase.denominator
    );
    const auto videoNumerator = static_cast<std::uint64_t>(
        videoTimeBase.numerator
    );
    const auto videoDenominator = static_cast<std::uint64_t>(
        videoTimeBase.denominator
    );

    const auto sourceScale = checkedUnsignedProduct(
        sourceNumerator,
        videoDenominator
    );
    const auto sourceDivisor = checkedUnsignedProduct(
        sourceDenominator,
        videoNumerator
    );
    const auto source = floorFraction(
        unsignedProduct(
            unsignedMagnitude(clock.sourcePresentationTimestamp),
            sourceScale
        ),
        {0, sourceDivisor},
        clock.sourcePresentationTimestamp < 0
    );

    auto elapsedSamples = clock.deviceSample.devicePosition
        - clock.deviceAnchor.devicePosition;
    auto elapsedScale = videoDenominator;
    auto elapsedFrequency = clock.deviceAnchor.frequency;
    auto elapsedDivisor = videoNumerator;
    cancelFactor(elapsedSamples, elapsedFrequency);
    cancelFactor(elapsedSamples, elapsedDivisor);
    cancelFactor(elapsedScale, elapsedFrequency);
    cancelFactor(elapsedScale, elapsedDivisor);
    const auto elapsed = floorFraction(
        unsignedProduct(elapsedSamples, elapsedScale),
        unsignedProduct(elapsedFrequency, elapsedDivisor),
        false
    );

    auto target = checkedClockSum(source.floor, elapsed.floor);
    if (fractionSumCarries(source, elapsed)) {
        target = checkedClockSum(target, 1);
    }
    return target;
}

std::uint64_t presentedFrameBytes(const PresentedVideoFrame& frame) noexcept {
    return static_cast<std::uint64_t>(frame.source.pixels.size())
        * sizeof(render::Rgba32Float);
}

void requireValidGeneration(std::uint64_t generation) {
    if (generation == 0) {
        fail(
            PresentationVideoErrorCode::invalidGeneration,
            "video buffer generation must be positive"
        );
    }
}

std::uint64_t sourceByteCount(const DecodedVideoFrame& decoded) {
    if (decoded.width <= 0 || decoded.height <= 0) {
        fail(
            PresentationVideoErrorCode::invalidFrameDimensions,
            "decoded frame dimensions must be positive"
        );
    }
    const auto width = static_cast<std::uint64_t>(decoded.width);
    const auto height = static_cast<std::uint64_t>(decoded.height);
    if (width > std::numeric_limits<std::uint64_t>::max() / height) {
        fail(PresentationVideoErrorCode::frameByteOverflow, "decoded frame pixel count overflows");
    }
    const auto pixels = width * height;
    if (pixels > std::numeric_limits<std::uint64_t>::max() / sizeof(render::Rgba32Float)) {
        fail(PresentationVideoErrorCode::frameByteOverflow, "render source byte count overflows");
    }
    return pixels * sizeof(render::Rgba32Float);
}

void validateAdapterResult(
    const DecodedVideoFrame& decoded,
    const render::SourceFrame& source,
    std::uint64_t expectedBytes
) {
    const auto rotation = decoded.displayTransform
        ? decoded.displayTransform->counterClockwiseDegrees
        : 0;
    const bool swapsDimensions = rotation == 90 || rotation == -90;
    if (!swapsDimensions && rotation != 0 && rotation != 180 && rotation != -180) {
        fail(
            PresentationVideoErrorCode::invalidAdapterResult,
            "video frame adapter returned an unsupported display transform"
        );
    }
    const auto expectedWidth = static_cast<std::uint32_t>(
        swapsDimensions ? decoded.height : decoded.width
    );
    const auto expectedHeight = static_cast<std::uint32_t>(
        swapsDimensions ? decoded.width : decoded.height
    );
    if (source.width != expectedWidth || source.height != expectedHeight) {
        fail(
            PresentationVideoErrorCode::invalidAdapterResult,
            "video frame adapter returned inconsistent dimensions"
        );
    }
    if (source.pixels.size() != expectedBytes / sizeof(render::Rgba32Float)) {
        fail(
            PresentationVideoErrorCode::invalidAdapterResult,
            "video frame adapter returned an inconsistent pixel count"
        );
    }
    try {
        render::validateSourceFrame(source, "/source");
    } catch (const render::RenderError&) {
        fail(
            PresentationVideoErrorCode::invalidAdapterResult,
            "video frame adapter returned an invalid render source"
        );
    }
}

}

PresentationVideoError::PresentationVideoError(
    PresentationVideoErrorCode codeValue,
    const char* message
) : std::runtime_error(message), code(codeValue) {}

PresentationVideoBuffer::PresentationVideoBuffer(PresentationVideoBufferLimits limits)
    : PresentationVideoBuffer(
          limits,
          [](const DecodedVideoFrame& decoded, std::stop_token cancellation) {
              return makeRenderSourceFrame(decoded, cancellation);
          },
          {},
          false
      ) {}

PresentationVideoBuffer::PresentationVideoBuffer(
    PresentationVideoBufferLimits limits,
    FrameAdapter frameAdapter,
    AdaptedFrameCheckpoint adaptedFrameCheckpoint,
    bool validatesAdapterOutput
) : limits_(limits),
    frameAdapter_(std::move(frameAdapter)),
    adaptedFrameCheckpoint_(std::move(adaptedFrameCheckpoint)),
    validatesAdapterOutput_(validatesAdapterOutput) {
    if (limits_.maximumFrames == 0 || limits_.maximumBytes == 0) {
        fail(PresentationVideoErrorCode::invalidLimits, "video buffer limits must be positive");
    }
    if (!frameAdapter_) {
        fail(PresentationVideoErrorCode::invalidAdapter, "video buffer frame adapter is required");
    }
}

PresentationVideoReceipt PresentationVideoBuffer::start(std::uint64_t generation) {
    requireValidGeneration(generation);
    if (generation < generation_) {
        fail(
            PresentationVideoErrorCode::invalidGeneration,
            "video buffer generation must increase monotonically"
        );
    }
    if (generation == generation_) {
        const auto outcome = accepting_
            ? PresentationVideoOutcome::noOp
            : PresentationVideoOutcome::cancelled;
        const auto reason = accepting_
            ? PresentationVideoReason::none
            : PresentationVideoReason::generationCancelled;
        return receipt(PresentationVideoOperation::start, outcome, reason);
    }
    requireRevisionCapacity();
    clear();
    generation_ = generation;
    accepting_ = true;
    ++revision_;
    return receipt(PresentationVideoOperation::start, PresentationVideoOutcome::changed);
}

PresentationVideoReceipt PresentationVideoBuffer::enqueue(
    std::uint64_t generation,
    const DecodedVideoFrame& decoded,
    std::stop_token cancellation
) {
    requireValidGeneration(generation);
    if (generation != generation_) {
        return receipt(
            PresentationVideoOperation::enqueue,
            PresentationVideoOutcome::stale,
            PresentationVideoReason::staleGeneration
        );
    }
    if (!accepting_) {
        return receipt(
            PresentationVideoOperation::enqueue,
            PresentationVideoOutcome::cancelled,
            PresentationVideoReason::generationCancelled
        );
    }
    if (cancellation.stop_requested()) {
        return receipt(
            PresentationVideoOperation::enqueue,
            PresentationVideoOutcome::cancelled,
            PresentationVideoReason::operationCancelled
        );
    }
    if (!decoded.presentationTimestamp) {
        fail(
            PresentationVideoErrorCode::missingPresentationTimestamp,
            "decoded frame requires a presentation timestamp"
        );
    }
    if (decoded.timeBase.numerator <= 0 || decoded.timeBase.denominator <= 0) {
        fail(PresentationVideoErrorCode::invalidTimeBase, "decoded frame time base must be positive");
    }
    if (timeBase_ && !sameTimeBase(*timeBase_, decoded.timeBase)) {
        fail(PresentationVideoErrorCode::changedTimeBase, "decoded frame time base changed within a generation");
    }
    if (lastAcceptedTimestamp_ && *decoded.presentationTimestamp <= *lastAcceptedTimestamp_) {
        fail(
            PresentationVideoErrorCode::nonIncreasingTimestamp,
            "decoded frame timestamps must be strictly increasing"
        );
    }
    if (frames_.size() >= limits_.maximumFrames) {
        return receipt(
            PresentationVideoOperation::enqueue,
            PresentationVideoOutcome::refused,
            PresentationVideoReason::frameCapacity
        );
    }
    const auto frameBytes = sourceByteCount(decoded);
    if (frameBytes > limits_.maximumBytes - queuedBytes_) {
        return receipt(
            PresentationVideoOperation::enqueue,
            PresentationVideoOutcome::refused,
            PresentationVideoReason::byteCapacity
        );
    }
    const auto expectedRevision = revision_;

    std::optional<render::SourceFrame> source;
    try {
        source = frameAdapter_(decoded, cancellation);
    } catch (const RenderSourceError& error) {
        if (auto changed = revalidationReceipt(generation, expectedRevision)) {
            return *changed;
        }
        if (cancellation.stop_requested() && error.code == "cancelled") {
            return receipt(
                PresentationVideoOperation::enqueue,
                PresentationVideoOutcome::cancelled,
                PresentationVideoReason::operationCancelled
            );
        }
        throw;
    }
    if (auto changed = revalidationReceipt(generation, expectedRevision)) {
        return *changed;
    }
    if (cancellation.stop_requested()) {
        return receipt(
            PresentationVideoOperation::enqueue,
            PresentationVideoOutcome::cancelled,
            PresentationVideoReason::operationCancelled
        );
    }
    if (validatesAdapterOutput_) {
        validateAdapterResult(decoded, *source, frameBytes);
    }
    if (adaptedFrameCheckpoint_) adaptedFrameCheckpoint_();
    if (auto changed = revalidationReceipt(generation, expectedRevision)) {
        return *changed;
    }
    if (cancellation.stop_requested()) {
        return receipt(
            PresentationVideoOperation::enqueue,
            PresentationVideoOutcome::cancelled,
            PresentationVideoReason::operationCancelled
        );
    }
    requireRevisionCapacity();
    frames_.push_back({
        generation,
        *decoded.presentationTimestamp,
        decoded.timeBase,
        std::move(*source),
    });
    queuedBytes_ += frameBytes;
    timeBase_ = decoded.timeBase;
    lastAcceptedTimestamp_ = decoded.presentationTimestamp;
    ++revision_;
    return receipt(PresentationVideoOperation::enqueue, PresentationVideoOutcome::changed);
}

PresentationVideoTake PresentationVideoBuffer::dequeue(std::uint64_t generation) {
    requireValidGeneration(generation);
    if (generation != generation_) {
        return {
            receipt(
                PresentationVideoOperation::dequeue,
                PresentationVideoOutcome::stale,
                PresentationVideoReason::staleGeneration
            ),
            std::nullopt,
        };
    }
    if (frames_.empty()) {
        const auto outcome = accepting_
            ? PresentationVideoOutcome::noOp
            : PresentationVideoOutcome::cancelled;
        const auto reason = accepting_
            ? PresentationVideoReason::none
            : PresentationVideoReason::generationCancelled;
        return {
            receipt(PresentationVideoOperation::dequeue, outcome, reason),
            std::nullopt,
        };
    }
    requireRevisionCapacity();
    auto frame = std::move(frames_.front());
    frames_.pop_front();
    const auto bytes = presentedFrameBytes(frame);
    queuedBytes_ -= bytes;
    ++revision_;
    return {
        receipt(PresentationVideoOperation::dequeue, PresentationVideoOutcome::changed),
        std::move(frame),
    };
}

PresentationVideoSelection PresentationVideoBuffer::select(
    std::uint64_t generation,
    std::uint64_t expectedRevision,
    const PresentationVideoClockPosition& clock
) {
    requireValidGeneration(generation);
    if (generation != generation_) {
        return {
            receipt(
                PresentationVideoOperation::select,
                PresentationVideoOutcome::stale,
                PresentationVideoReason::staleGeneration
            ),
        };
    }
    if (expectedRevision != revision_) {
        return {
            receipt(
                PresentationVideoOperation::select,
                PresentationVideoOutcome::stale,
                PresentationVideoReason::stateChanged
            ),
        };
    }
    if (!accepting_) {
        return {
            receipt(
                PresentationVideoOperation::select,
                PresentationVideoOutcome::cancelled,
                PresentationVideoReason::generationCancelled
            ),
        };
    }
    if (clock.deviceAnchor.generation != generation
        || clock.deviceSample.generation != generation) {
        return {
            receipt(
                PresentationVideoOperation::select,
                PresentationVideoOutcome::stale,
                PresentationVideoReason::staleClock
            ),
        };
    }

    const auto timelineTarget = targetTimelineFrame(clock);
    if (clock.sourceTimeBase.numerator <= 0
        || clock.sourceTimeBase.denominator <= 0) {
        fail(
            PresentationVideoErrorCode::invalidClockSourceTimeBase,
            "video selection source time base must be positive"
        );
    }
    if (frames_.empty()) {
        return {
            receipt(
                PresentationVideoOperation::select,
                PresentationVideoOutcome::noOp
            ),
            std::nullopt,
            0,
            true,
            timelineTarget,
        };
    }
    if (!timeBase_.has_value()) {
        fail(
            PresentationVideoErrorCode::invalidTimeBase,
            "queued video frames lost their time base"
        );
    }

    const auto presentationTarget = targetPresentationTimestamp(
        clock,
        *timeBase_
    );
    std::size_t dueFrames = 0;
    while (dueFrames < frames_.size()
        && frames_[dueFrames].presentationTimestamp <= presentationTarget) {
        ++dueFrames;
    }
    if (dueFrames == 0) {
        return {
            receipt(
                PresentationVideoOperation::select,
                PresentationVideoOutcome::noOp,
                PresentationVideoReason::frameEarly
            ),
            std::nullopt,
            0,
            true,
            timelineTarget,
        };
    }

    std::uint64_t removedBytes = 0;
    for (std::size_t index = 0; index < dueFrames; ++index) {
        const auto bytes = presentedFrameBytes(frames_[index]);
        if (bytes > queuedBytes_ - removedBytes) {
            fail(
                PresentationVideoErrorCode::invalidAdapterResult,
                "video selection byte accounting is invalid"
            );
        }
        removedBytes += bytes;
    }
    requireRevisionCapacity();
    auto selected = std::move(frames_[dueFrames - 1]);
    for (std::size_t index = 0; index < dueFrames; ++index) {
        frames_.pop_front();
    }
    queuedBytes_ -= removedBytes;
    ++revision_;
    return {
        receipt(
            PresentationVideoOperation::select,
            PresentationVideoOutcome::changed
        ),
        std::move(selected),
        dueFrames - 1,
        true,
        timelineTarget,
    };
}

PresentationVideoReceipt PresentationVideoBuffer::cancel(std::uint64_t generation) {
    requireValidGeneration(generation);
    if (generation != generation_) {
        return receipt(
            PresentationVideoOperation::cancel,
            PresentationVideoOutcome::stale,
            PresentationVideoReason::staleGeneration
        );
    }
    if (!accepting_) {
        return receipt(
            PresentationVideoOperation::cancel,
            PresentationVideoOutcome::noOp,
            PresentationVideoReason::generationCancelled
        );
    }
    requireRevisionCapacity();
    clear();
    accepting_ = false;
    ++revision_;
    return receipt(
        PresentationVideoOperation::cancel,
        PresentationVideoOutcome::changed,
        PresentationVideoReason::generationCancelled
    );
}

std::uint64_t PresentationVideoBuffer::generation() const noexcept { return generation_; }
std::uint64_t PresentationVideoBuffer::revision() const noexcept { return revision_; }
std::size_t PresentationVideoBuffer::queuedFrames() const noexcept { return frames_.size(); }
std::uint64_t PresentationVideoBuffer::queuedBytes() const noexcept { return queuedBytes_; }

PresentationVideoReceipt PresentationVideoBuffer::receipt(
    PresentationVideoOperation operation,
    PresentationVideoOutcome outcome,
    PresentationVideoReason reason
) const noexcept {
    return {
        operation,
        outcome,
        reason,
        generation_,
        revision_,
        frames_.size(),
        queuedBytes_,
    };
}

std::optional<PresentationVideoReceipt> PresentationVideoBuffer::revalidationReceipt(
    std::uint64_t generation,
    std::uint64_t expectedRevision
) const noexcept {
    if (generation != generation_) {
        return receipt(
            PresentationVideoOperation::enqueue,
            PresentationVideoOutcome::stale,
            PresentationVideoReason::staleGeneration
        );
    }
    if (!accepting_) {
        return receipt(
            PresentationVideoOperation::enqueue,
            PresentationVideoOutcome::cancelled,
            PresentationVideoReason::generationCancelled
        );
    }
    if (revision_ != expectedRevision) {
        return receipt(
            PresentationVideoOperation::enqueue,
            PresentationVideoOutcome::stale,
            PresentationVideoReason::stateChanged
        );
    }
    return std::nullopt;
}

void PresentationVideoBuffer::requireRevisionCapacity() const {
    if (revision_ == std::numeric_limits<std::uint64_t>::max()) {
        fail(
            PresentationVideoErrorCode::revisionOverflow,
            "video buffer revision cannot advance"
        );
    }
}

void PresentationVideoBuffer::clear() noexcept {
    frames_.clear();
    queuedBytes_ = 0;
    timeBase_.reset();
    lastAcceptedTimestamp_.reset();
}

}
