#pragma once

#include "palmier/audio/audio_clock.hpp"
#include "palmier/media/render_source_adapter.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <stdexcept>
#include <stop_token>

namespace palmier::media {

namespace detail {
class PresentationVideoBufferTestAccess;
}

struct PresentationVideoBufferLimits final {
    std::size_t maximumFrames{8};
    std::uint64_t maximumBytes{256ULL * 1024ULL * 1024ULL};
};

enum class PresentationVideoOperation {
    start,
    enqueue,
    dequeue,
    select,
    cancel,
};

enum class PresentationVideoOutcome {
    changed,
    noOp,
    stale,
    cancelled,
    refused,
};

enum class PresentationVideoReason {
    none,
    staleGeneration,
    stateChanged,
    operationCancelled,
    generationCancelled,
    staleClock,
    frameEarly,
    frameCapacity,
    byteCapacity,
};

enum class PresentationVideoErrorCode {
    invalidLimits,
    invalidAdapter,
    invalidAdapterResult,
    revisionOverflow,
    invalidGeneration,
    invalidClock,
    invalidClockSourceTimeBase,
    clockPositionDiscontinuity,
    clockArithmeticOverflow,
    missingPresentationTimestamp,
    invalidTimeBase,
    changedTimeBase,
    nonIncreasingTimestamp,
    invalidFrameDimensions,
    frameByteOverflow,
};

class PresentationVideoError final : public std::runtime_error {
public:
    PresentationVideoError(PresentationVideoErrorCode code, const char* message);

    PresentationVideoErrorCode code;
};

struct PresentationVideoReceipt final {
    PresentationVideoOperation operation{PresentationVideoOperation::enqueue};
    PresentationVideoOutcome outcome{PresentationVideoOutcome::noOp};
    PresentationVideoReason reason{PresentationVideoReason::none};
    std::uint64_t generation{};
    std::uint64_t revision{};
    std::size_t queuedFrames{};
    std::uint64_t queuedBytes{};
};

struct PresentedVideoFrame final {
    std::uint64_t generation{};
    std::int64_t presentationTimestamp{};
    Rational timeBase;
    render::SourceFrame source;
};

struct PresentationVideoTake final {
    PresentationVideoReceipt receipt;
    std::optional<PresentedVideoFrame> frame;
};

struct PresentationVideoClockPosition final {
    audio::AudioClockAnchor deviceAnchor;
    audio::AudioClockSample deviceSample;
    audio::FrameRate timelineFrameRate;
    std::int64_t sourcePresentationTimestamp{};
    Rational sourceTimeBase;
};

struct PresentationVideoSelection final {
    PresentationVideoReceipt receipt;
    std::optional<PresentedVideoFrame> frame;
    std::size_t droppedFrames{};
    bool hasTargetTimelineFrame{};
    std::int64_t targetTimelineFrame{};
};

class PresentationVideoBuffer final {
public:
    explicit PresentationVideoBuffer(PresentationVideoBufferLimits limits = {});
    PresentationVideoBuffer(const PresentationVideoBuffer&) = delete;
    PresentationVideoBuffer& operator=(const PresentationVideoBuffer&) = delete;
    PresentationVideoBuffer(PresentationVideoBuffer&&) = delete;
    PresentationVideoBuffer& operator=(PresentationVideoBuffer&&) = delete;

    PresentationVideoReceipt start(std::uint64_t generation);
    PresentationVideoReceipt enqueue(
        std::uint64_t generation,
        const DecodedVideoFrame& decoded,
        std::stop_token cancellation = {}
    );
    PresentationVideoTake dequeue(std::uint64_t generation);
    PresentationVideoSelection select(
        std::uint64_t generation,
        std::uint64_t expectedRevision,
        const PresentationVideoClockPosition& clock
    );
    PresentationVideoReceipt cancel(std::uint64_t generation);

    std::uint64_t generation() const noexcept;
    std::uint64_t revision() const noexcept;
    std::size_t queuedFrames() const noexcept;
    std::uint64_t queuedBytes() const noexcept;

private:
    friend class detail::PresentationVideoBufferTestAccess;

    using FrameAdapter = std::function<render::SourceFrame(
        const DecodedVideoFrame&,
        std::stop_token
    )>;
    using AdaptedFrameCheckpoint = std::function<void()>;

    PresentationVideoBuffer(
        PresentationVideoBufferLimits limits,
        FrameAdapter frameAdapter,
        AdaptedFrameCheckpoint adaptedFrameCheckpoint,
        bool validatesAdapterOutput
    );
    PresentationVideoReceipt receipt(
        PresentationVideoOperation operation,
        PresentationVideoOutcome outcome,
        PresentationVideoReason reason = PresentationVideoReason::none
    ) const noexcept;
    std::optional<PresentationVideoReceipt> revalidationReceipt(
        std::uint64_t generation,
        std::uint64_t expectedRevision
    ) const noexcept;
    void requireRevisionCapacity() const;
    void clear() noexcept;

    PresentationVideoBufferLimits limits_;
    FrameAdapter frameAdapter_;
    AdaptedFrameCheckpoint adaptedFrameCheckpoint_;
    std::deque<PresentedVideoFrame> frames_;
    std::uint64_t generation_{};
    std::uint64_t revision_{};
    std::uint64_t queuedBytes_{};
    std::optional<Rational> timeBase_;
    std::optional<std::int64_t> lastAcceptedTimestamp_;
    bool accepting_{};
    bool validatesAdapterOutput_{};
};

}
