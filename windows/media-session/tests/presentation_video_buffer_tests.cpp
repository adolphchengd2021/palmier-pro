#include "palmier/media/presentation_video_buffer.hpp"
#include "internal/presentation_video_buffer_testing.hpp"
#include "internal/render_source_adapter_testing.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <type_traits>
#include <utility>

namespace {

using palmier::media::AlphaMode;
using palmier::media::DecodedVideoFrame;
using palmier::media::PresentationVideoBuffer;
using palmier::media::PresentationVideoError;
using palmier::media::PresentationVideoErrorCode;
using palmier::media::PresentationVideoOperation;
using palmier::media::PresentationVideoOutcome;
using palmier::media::PresentationVideoReason;
using palmier::media::PresentationVideoReceipt;
using palmier::media::RenderSourceError;
using palmier::media::detail::PresentationVideoBufferTestAccess;

static_assert(!std::is_copy_constructible_v<PresentationVideoBuffer>);
static_assert(!std::is_copy_assignable_v<PresentationVideoBuffer>);
static_assert(!std::is_move_constructible_v<PresentationVideoBuffer>);
static_assert(!std::is_move_assignable_v<PresentationVideoBuffer>);

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void requireReceipt(
    const PresentationVideoReceipt& receipt,
    PresentationVideoOperation operation,
    PresentationVideoOutcome outcome,
    PresentationVideoReason reason,
    std::uint64_t generation,
    std::uint64_t revision,
    std::size_t frames,
    std::uint64_t bytes
) {
    require(receipt.operation == operation, "unexpected receipt operation");
    require(receipt.outcome == outcome, "unexpected receipt outcome");
    require(receipt.reason == reason, "unexpected receipt reason");
    require(receipt.generation == generation, "unexpected receipt generation");
    require(receipt.revision == revision, "unexpected receipt revision");
    require(receipt.queuedFrames == frames, "unexpected receipt frame count");
    require(receipt.queuedBytes == bytes, "unexpected receipt byte count");
}

template<typename Operation>
void requireError(Operation operation, PresentationVideoErrorCode code) {
    try {
        operation();
    } catch (const PresentationVideoError& error) {
        require(error.code == code, "unexpected presentation-video error code");
        return;
    }
    throw std::runtime_error("expected presentation-video failure");
}

template<typename Operation>
void requireAdapterError(Operation operation, const std::string& code) {
    try {
        operation();
    } catch (const RenderSourceError& error) {
        require(error.code == code, "unexpected render-source error code");
        return;
    }
    throw std::runtime_error("expected render-source failure");
}

DecodedVideoFrame frame(
    std::int64_t timestamp,
    std::uint8_t red = 255,
    std::int32_t height = 1
) {
    DecodedVideoFrame result;
    result.width = 1;
    result.height = height;
    result.rowBytes = 4;
    result.rgba8.resize(static_cast<std::size_t>(height) * 4, 0);
    for (std::int32_t row = 0; row < height; ++row) {
        const auto offset = static_cast<std::size_t>(row) * 4;
        result.rgba8[offset] = red;
        result.rgba8[offset + 3] = 255;
    }
    result.presentationTimestamp = timestamp;
    result.timeBase = {1, 30};
    result.color = {1, 13, 0, 2, 0};
    result.alphaMode = AlphaMode::opaque;
    return result;
}

void validatesLimitsAndGenerations() {
    requireError(
        [] { PresentationVideoBuffer({0, 16}); },
        PresentationVideoErrorCode::invalidLimits
    );
    requireError(
        [] { PresentationVideoBuffer({1, 0}); },
        PresentationVideoErrorCode::invalidLimits
    );
    requireError(
        [] { PresentationVideoBufferTestAccess::create({1, 16}, {}, {}); },
        PresentationVideoErrorCode::invalidAdapter
    );

    PresentationVideoBuffer buffer;
    requireError(
        [&] { buffer.start(0); },
        PresentationVideoErrorCode::invalidGeneration
    );
    const auto started = buffer.start(2);
    requireReceipt(
        started,
        PresentationVideoOperation::start,
        PresentationVideoOutcome::changed,
        PresentationVideoReason::none,
        2,
        1,
        0,
        0
    );
    buffer.enqueue(2, frame(10));
    const auto repeated = buffer.start(2);
    requireReceipt(
        repeated,
        PresentationVideoOperation::start,
        PresentationVideoOutcome::noOp,
        PresentationVideoReason::none,
        2,
        2,
        1,
        16
    );
    requireError(
        [&] { buffer.start(1); },
        PresentationVideoErrorCode::invalidGeneration
    );
    requireError(
        [&] { buffer.enqueue(0, frame(20)); },
        PresentationVideoErrorCode::invalidGeneration
    );
    requireError(
        [&] { buffer.dequeue(0); },
        PresentationVideoErrorCode::invalidGeneration
    );
    requireError(
        [&] { buffer.cancel(0); },
        PresentationVideoErrorCode::invalidGeneration
    );

    PresentationVideoBuffer exhausted;
    exhausted.start(1);
    PresentationVideoBufferTestAccess::setRevision(
        exhausted,
        std::numeric_limits<std::uint64_t>::max()
    );
    requireError(
        [&] { exhausted.start(2); },
        PresentationVideoErrorCode::revisionOverflow
    );
    requireReceipt(
        exhausted.start(1),
        PresentationVideoOperation::start,
        PresentationVideoOutcome::noOp,
        PresentationVideoReason::none,
        1,
        std::numeric_limits<std::uint64_t>::max(),
        0,
        0
    );
}

void revisionOverflowPreservesState() {
    PresentationVideoBuffer enqueueBuffer;
    enqueueBuffer.start(1);
    PresentationVideoBufferTestAccess::setRevision(
        enqueueBuffer,
        std::numeric_limits<std::uint64_t>::max()
    );
    requireError(
        [&] { enqueueBuffer.enqueue(1, frame(0)); },
        PresentationVideoErrorCode::revisionOverflow
    );
    require(enqueueBuffer.queuedFrames() == 0, "overflowing enqueue committed a frame");
    PresentationVideoBufferTestAccess::setRevision(enqueueBuffer, 1);
    require(
        enqueueBuffer.enqueue(1, frame(0)).outcome == PresentationVideoOutcome::changed,
        "overflowing enqueue polluted timestamp cursor"
    );

    PresentationVideoBuffer dequeueBuffer;
    dequeueBuffer.start(1);
    dequeueBuffer.enqueue(1, frame(0));
    PresentationVideoBufferTestAccess::setRevision(
        dequeueBuffer,
        std::numeric_limits<std::uint64_t>::max()
    );
    requireError(
        [&] { dequeueBuffer.dequeue(1); },
        PresentationVideoErrorCode::revisionOverflow
    );
    require(dequeueBuffer.queuedFrames() == 1, "overflowing dequeue removed a frame");
    PresentationVideoBufferTestAccess::setRevision(dequeueBuffer, 2);
    require(dequeueBuffer.dequeue(1).frame.has_value(), "overflowing dequeue lost its frame");

    PresentationVideoBuffer cancelBuffer;
    cancelBuffer.start(1);
    cancelBuffer.enqueue(1, frame(0));
    PresentationVideoBufferTestAccess::setRevision(
        cancelBuffer,
        std::numeric_limits<std::uint64_t>::max()
    );
    requireError(
        [&] { cancelBuffer.cancel(1); },
        PresentationVideoErrorCode::revisionOverflow
    );
    require(cancelBuffer.queuedFrames() == 1, "overflowing cancel cleared the queue");
    PresentationVideoBufferTestAccess::setRevision(cancelBuffer, 2);
    require(
        cancelBuffer.cancel(1).outcome == PresentationVideoOutcome::changed,
        "overflowing cancel closed the generation"
    );
}

void ordersAndDequeuesFrames() {
    PresentationVideoBuffer buffer({2, 128});
    buffer.start(1);
    require(buffer.enqueue(1, frame(10)).outcome == PresentationVideoOutcome::changed, "first frame refused");
    require(buffer.enqueue(1, frame(20, 128)).outcome == PresentationVideoOutcome::changed, "second frame refused");
    const auto full = buffer.enqueue(1, frame(30));
    requireReceipt(
        full,
        PresentationVideoOperation::enqueue,
        PresentationVideoOutcome::refused,
        PresentationVideoReason::frameCapacity,
        1,
        3,
        2,
        32
    );

    auto first = buffer.dequeue(1);
    require(first.frame.has_value(), "first dequeue is empty");
    require(first.frame->presentationTimestamp == 10, "first frame is not presentation ordered");
    require(first.frame->timeBase.numerator == 1, "first frame time-base numerator changed");
    require(first.frame->timeBase.denominator == 30, "first frame time-base denominator changed");
    requireReceipt(
        first.receipt,
        PresentationVideoOperation::dequeue,
        PresentationVideoOutcome::changed,
        PresentationVideoReason::none,
        1,
        4,
        1,
        16
    );
    auto second = buffer.dequeue(1);
    require(second.frame.has_value(), "second dequeue is empty");
    require(second.frame->presentationTimestamp == 20, "second frame is not presentation ordered");
    require(second.receipt.queuedBytes == 0, "second dequeue did not release bytes");
    require(buffer.enqueue(1, frame(30)).outcome == PresentationVideoOutcome::changed, "released frame capacity was not reusable");
    const auto reused = buffer.dequeue(1);
    require(reused.frame.has_value(), "reused capacity did not return a frame");
    require(reused.frame->presentationTimestamp == 30, "reused capacity returned wrong frame");
    const auto empty = buffer.dequeue(1);
    requireReceipt(
        empty.receipt,
        PresentationVideoOperation::dequeue,
        PresentationVideoOutcome::noOp,
        PresentationVideoReason::none,
        1,
        7,
        0,
        0
    );
    require(!empty.frame, "empty dequeue returned a frame");
}

void validatesTimestampAndTimeBaseBeforeMutation() {
    PresentationVideoBuffer buffer;
    buffer.start(1);
    auto missingTimestamp = frame(0);
    missingTimestamp.presentationTimestamp.reset();
    requireError(
        [&] { buffer.enqueue(1, missingTimestamp); },
        PresentationVideoErrorCode::missingPresentationTimestamp
    );
    for (const auto invalid : {palmier::media::Rational{0, 30}, {-1, 30}, {1, 0}, {1, -30}}) {
        auto invalidTimeBase = frame(0);
        invalidTimeBase.timeBase = invalid;
        requireError(
            [&] { buffer.enqueue(1, invalidTimeBase); },
            PresentationVideoErrorCode::invalidTimeBase
        );
    }
    require(buffer.enqueue(1, frame(10)).outcome == PresentationVideoOutcome::changed, "valid frame refused");
    auto changedTimeBase = frame(20);
    changedTimeBase.timeBase = {1, 60};
    requireError(
        [&] { buffer.enqueue(1, changedTimeBase); },
        PresentationVideoErrorCode::changedTimeBase
    );
    requireError(
        [&] { buffer.enqueue(1, frame(10)); },
        PresentationVideoErrorCode::nonIncreasingTimestamp
    );
    requireError(
        [&] { buffer.enqueue(1, frame(9)); },
        PresentationVideoErrorCode::nonIncreasingTimestamp
    );
    require(buffer.queuedFrames() == 1, "failed enqueue mutated queue");
    require(buffer.enqueue(1, frame(20)).outcome == PresentationVideoOutcome::changed, "failed enqueue polluted timestamp cursor");
}

void enforcesCapacityBeforeAdaptation() {
    std::size_t adaptationCount = 0;
    const auto countingAdapter = [&adaptationCount](
        const DecodedVideoFrame& decoded,
        std::stop_token cancellation
    ) {
        ++adaptationCount;
        return palmier::media::makeRenderSourceFrame(decoded, cancellation);
    };

    auto frameBounded = PresentationVideoBufferTestAccess::create(
        {1, 128},
        countingAdapter,
        {}
    );
    frameBounded.start(1);
    frameBounded.enqueue(1, frame(0));
    auto invalidAlpha = frame(1);
    invalidAlpha.alphaMode = AlphaMode::unspecified;
    const auto frameRefusal = frameBounded.enqueue(1, invalidAlpha);
    require(frameRefusal.reason == PresentationVideoReason::frameCapacity, "frame limit lost precedence");
    require(adaptationCount == 1, "frame-capacity refusal invoked adapter");

    auto byteBounded = PresentationVideoBufferTestAccess::create(
        {3, 32},
        countingAdapter,
        {}
    );
    byteBounded.start(1);
    require(byteBounded.enqueue(1, frame(0)).outcome == PresentationVideoOutcome::changed, "first exact-byte frame refused");
    require(byteBounded.enqueue(1, frame(1)).outcome == PresentationVideoOutcome::changed, "second exact-byte frame refused");
    auto byteInvalidAlpha = frame(2);
    byteInvalidAlpha.alphaMode = AlphaMode::unspecified;
    const auto byteRefusal = byteBounded.enqueue(1, byteInvalidAlpha);
    require(byteRefusal.reason == PresentationVideoReason::byteCapacity, "byte limit lost precedence");
    require(byteBounded.queuedBytes() == 32, "exact byte accounting changed");
    require(adaptationCount == 3, "byte-capacity refusal invoked adapter");
    require(byteBounded.dequeue(1).receipt.queuedBytes == 16, "dequeue did not release byte capacity");
    require(byteBounded.enqueue(1, frame(2)).outcome == PresentationVideoOutcome::changed, "released byte capacity was not reusable");

    auto invalidDimensions = frame(0);
    invalidDimensions.width = 0;
    PresentationVideoBuffer dimensions;
    dimensions.start(1);
    requireError(
        [&] { dimensions.enqueue(1, invalidDimensions); },
        PresentationVideoErrorCode::invalidFrameDimensions
    );
    invalidDimensions.width = 1;
    invalidDimensions.height = -1;
    requireError(
        [&] { dimensions.enqueue(1, invalidDimensions); },
        PresentationVideoErrorCode::invalidFrameDimensions
    );
    auto overflowing = frame(0);
    overflowing.width = std::numeric_limits<std::int32_t>::max();
    overflowing.height = std::numeric_limits<std::int32_t>::max();
    requireError(
        [&] { dimensions.enqueue(1, overflowing); },
        PresentationVideoErrorCode::frameByteOverflow
    );
}

void adapterFailurePreservesQueue() {
    PresentationVideoBuffer buffer;
    buffer.start(1);
    buffer.enqueue(1, frame(0));
    auto invalidAlpha = frame(1);
    invalidAlpha.alphaMode = AlphaMode::unspecified;
    requireAdapterError(
        [&] { buffer.enqueue(1, invalidAlpha); },
        "unsupportedAlphaMode"
    );
    require(buffer.queuedFrames() == 1, "adapter failure changed frame count");
    require(buffer.queuedBytes() == 16, "adapter failure changed byte count");
    require(buffer.enqueue(1, frame(1)).outcome == PresentationVideoOutcome::changed, "adapter failure polluted timestamp cursor");
}

void invalidAdapterResultPreservesQueue() {
    auto buffer = PresentationVideoBufferTestAccess::create(
        {2, 128},
        [](const DecodedVideoFrame&, std::stop_token) {
            return palmier::render::SourceFrame{1, 1, {}};
        },
        {}
    );
    buffer.start(1);
    requireError(
        [&] { buffer.enqueue(1, frame(0)); },
        PresentationVideoErrorCode::invalidAdapterResult
    );
    require(buffer.queuedFrames() == 0, "invalid adapter result changed frame count");
    require(buffer.queuedBytes() == 0, "invalid adapter result changed byte count");

    auto invalidDimensions = PresentationVideoBufferTestAccess::create(
        {2, 128},
        [](const DecodedVideoFrame&, std::stop_token) {
            return palmier::render::SourceFrame{0, 1, {{0, 0, 0, 1}}};
        },
        {}
    );
    invalidDimensions.start(1);
    requireError(
        [&] { invalidDimensions.enqueue(1, frame(0)); },
        PresentationVideoErrorCode::invalidAdapterResult
    );
    require(
        invalidDimensions.queuedFrames() == 0,
        "invalid adapter dimensions changed queue"
    );

    bool nonFiniteOnce = true;
    auto nonFinite = PresentationVideoBufferTestAccess::create(
        {2, 128},
        [&nonFiniteOnce](const DecodedVideoFrame& decoded, std::stop_token cancellation) {
            if (nonFiniteOnce) {
                nonFiniteOnce = false;
                const auto nan = std::numeric_limits<float>::quiet_NaN();
                return palmier::render::SourceFrame{1, 1, {{nan, 0, 0, 1}}};
            }
            return palmier::media::makeRenderSourceFrame(decoded, cancellation);
        },
        {}
    );
    nonFinite.start(1);
    requireError(
        [&] { nonFinite.enqueue(1, frame(0)); },
        PresentationVideoErrorCode::invalidAdapterResult
    );
    require(
        nonFinite.enqueue(1, frame(0)).outcome == PresentationVideoOutcome::changed,
        "invalid adapter output polluted timestamp cursor"
    );
}

void cancellationDuringAndAfterAdaptationPreservesQueue() {
    std::stop_source duringSource;
    bool stopDuringOnce = true;
    const auto duringAdapter = [&duringSource, &stopDuringOnce](
        const DecodedVideoFrame& decoded,
        std::stop_token cancellation
    ) {
        palmier::media::detail::RenderSourceAdapterHooks hooks;
        hooks.didConvertRow = [&duringSource, &stopDuringOnce](std::uint32_t) {
            if (stopDuringOnce) {
                stopDuringOnce = false;
                duringSource.request_stop();
            }
        };
        return palmier::media::detail::makeRenderSourceFrame(decoded, cancellation, hooks);
    };
    auto during = PresentationVideoBufferTestAccess::create(
        {2, 128},
        duringAdapter,
        {}
    );
    during.start(1);
    const auto duringCancelled = during.enqueue(1, frame(0, 255, 2), duringSource.get_token());
    require(duringCancelled.outcome == PresentationVideoOutcome::cancelled, "during-adapt cancellation escaped");
    require(duringCancelled.reason == PresentationVideoReason::operationCancelled, "wrong during-adapt reason");
    require(during.queuedFrames() == 0 && during.queuedBytes() == 0, "during-adapt cancellation mutated queue");
    require(during.enqueue(1, frame(0, 255, 2)).outcome == PresentationVideoOutcome::changed, "during-adapt cancellation polluted cursor");

    std::stop_source afterSource;
    bool stopAfterOnce = true;
    auto after = PresentationVideoBufferTestAccess::create(
        {2, 128},
        [](const DecodedVideoFrame& decoded, std::stop_token cancellation) {
            return palmier::media::makeRenderSourceFrame(decoded, cancellation);
        },
        [&afterSource, &stopAfterOnce] {
            if (stopAfterOnce) {
                stopAfterOnce = false;
                afterSource.request_stop();
            }
        }
    );
    after.start(1);
    const auto afterCancelled = after.enqueue(1, frame(0), afterSource.get_token());
    require(afterCancelled.outcome == PresentationVideoOutcome::cancelled, "post-adapt cancellation escaped");
    require(afterCancelled.reason == PresentationVideoReason::operationCancelled, "wrong post-adapt reason");
    require(after.queuedFrames() == 0 && after.queuedBytes() == 0, "post-adapt cancellation mutated queue");
    require(after.enqueue(1, frame(0)).outcome == PresentationVideoOutcome::changed, "post-adapt cancellation polluted cursor");
}

void reentrantGenerationChangeRejectsOuterCommit() {
    PresentationVideoBuffer* current = nullptr;
    bool switchOnce = true;
    auto buffer = PresentationVideoBufferTestAccess::create(
        {2, 128},
        [](const DecodedVideoFrame& decoded, std::stop_token cancellation) {
            return palmier::media::makeRenderSourceFrame(decoded, cancellation);
        },
        [&] {
            if (switchOnce) {
                switchOnce = false;
                current->start(2);
            }
        }
    );
    current = &buffer;
    buffer.start(1);
    const auto stale = buffer.enqueue(1, frame(10));
    requireReceipt(
        stale,
        PresentationVideoOperation::enqueue,
        PresentationVideoOutcome::stale,
        PresentationVideoReason::staleGeneration,
        2,
        2,
        0,
        0
    );
    require(
        buffer.enqueue(2, frame(10)).outcome == PresentationVideoOutcome::changed,
        "new generation rejected after reentrant switch"
    );
}

void adapterReentrancyRejectsOuterCommit() {
    PresentationVideoBuffer* current = nullptr;
    bool switchOnce = true;
    auto buffer = PresentationVideoBufferTestAccess::create(
        {2, 128},
        [&](const DecodedVideoFrame& decoded, std::stop_token cancellation) {
            auto source = palmier::media::makeRenderSourceFrame(decoded, cancellation);
            if (switchOnce) {
                switchOnce = false;
                current->start(2);
            }
            return source;
        },
        {}
    );
    current = &buffer;
    buffer.start(1);
    const auto stale = buffer.enqueue(1, frame(10));
    requireReceipt(
        stale,
        PresentationVideoOperation::enqueue,
        PresentationVideoOutcome::stale,
        PresentationVideoReason::staleGeneration,
        2,
        2,
        0,
        0
    );
}

void reentrantCancellationRejectsOuterCommit() {
    PresentationVideoBuffer* current = nullptr;
    bool cancelOnce = true;
    auto buffer = PresentationVideoBufferTestAccess::create(
        {2, 128},
        [](const DecodedVideoFrame& decoded, std::stop_token cancellation) {
            return palmier::media::makeRenderSourceFrame(decoded, cancellation);
        },
        [&] {
            if (cancelOnce) {
                cancelOnce = false;
                current->cancel(1);
            }
        }
    );
    current = &buffer;
    buffer.start(1);
    const auto cancelled = buffer.enqueue(1, frame(10));
    requireReceipt(
        cancelled,
        PresentationVideoOperation::enqueue,
        PresentationVideoOutcome::cancelled,
        PresentationVideoReason::generationCancelled,
        1,
        2,
        0,
        0
    );
}

void reentrantRevisionChangeRejectsOuterCommit() {
    PresentationVideoBuffer* current = nullptr;
    bool enqueueOnce = true;
    auto buffer = PresentationVideoBufferTestAccess::create(
        {3, 128},
        [](const DecodedVideoFrame& decoded, std::stop_token cancellation) {
            return palmier::media::makeRenderSourceFrame(decoded, cancellation);
        },
        [&] {
            if (enqueueOnce) {
                enqueueOnce = false;
                current->enqueue(1, frame(5));
            }
        }
    );
    current = &buffer;
    buffer.start(1);
    const auto stale = buffer.enqueue(1, frame(10));
    requireReceipt(
        stale,
        PresentationVideoOperation::enqueue,
        PresentationVideoOutcome::stale,
        PresentationVideoReason::stateChanged,
        1,
        2,
        1,
        16
    );
    const auto inner = buffer.dequeue(1);
    require(
        inner.frame && inner.frame->presentationTimestamp == 5,
        "reentrant frame was not preserved"
    );
    require(
        buffer.enqueue(1, frame(6)).outcome == PresentationVideoOutcome::changed,
        "reentrant mutation corrupted timestamp cursor"
    );
}

void rejectsStaleAndCancelledResultsBeforeValidation() {
    PresentationVideoBuffer buffer;
    buffer.start(1);
    buffer.enqueue(1, frame(0));
    buffer.start(2);
    require(buffer.queuedFrames() == 0, "new generation retained old frames");
    buffer.enqueue(2, frame(1));
    require(buffer.cancel(1).outcome == PresentationVideoOutcome::stale, "stale cancel changed state");
    require(buffer.dequeue(1).receipt.outcome == PresentationVideoOutcome::stale, "stale dequeue was not reported");
    require(buffer.queuedFrames() == 1, "stale operation removed current frame");
    const auto current = buffer.dequeue(2);
    require(current.frame && current.frame->presentationTimestamp == 1, "stale operation changed current frame");
    auto invalid = frame(1);
    invalid.presentationTimestamp.reset();
    const auto stale = buffer.enqueue(1, invalid);
    requireReceipt(
        stale,
        PresentationVideoOperation::enqueue,
        PresentationVideoOutcome::stale,
        PresentationVideoReason::staleGeneration,
        2,
        5,
        0,
        0
    );

    std::stop_source source;
    source.request_stop();
    const auto preCancelled = buffer.enqueue(2, frame(2), source.get_token());
    require(preCancelled.outcome == PresentationVideoOutcome::cancelled, "pre-cancelled frame was accepted");
    require(preCancelled.reason == PresentationVideoReason::operationCancelled, "wrong pre-cancel reason");
    buffer.enqueue(2, frame(3));
    const auto cancelled = buffer.cancel(2);
    requireReceipt(
        cancelled,
        PresentationVideoOperation::cancel,
        PresentationVideoOutcome::changed,
        PresentationVideoReason::generationCancelled,
        2,
        7,
        0,
        0
    );
    require(buffer.cancel(2).outcome == PresentationVideoOutcome::noOp, "repeated cancel changed state");
    require(buffer.start(2).outcome == PresentationVideoOutcome::cancelled, "cancelled generation appeared restarted");
    require(buffer.enqueue(2, invalid).outcome == PresentationVideoOutcome::cancelled, "cancelled invalid frame was validated");
    require(buffer.dequeue(2).receipt.outcome == PresentationVideoOutcome::cancelled, "cancelled dequeue was not reported");
}

}

int main() {
    try {
        validatesLimitsAndGenerations();
        revisionOverflowPreservesState();
        ordersAndDequeuesFrames();
        validatesTimestampAndTimeBaseBeforeMutation();
        enforcesCapacityBeforeAdaptation();
        adapterFailurePreservesQueue();
        invalidAdapterResultPreservesQueue();
        cancellationDuringAndAfterAdaptationPreservesQueue();
        reentrantGenerationChangeRejectsOuterCommit();
        adapterReentrancyRejectsOuterCommit();
        reentrantCancellationRejectsOuterCommit();
        reentrantRevisionChangeRejectsOuterCommit();
        rejectsStaleAndCancelledResultsBeforeValidation();
        std::cout << "presentation video buffer tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
