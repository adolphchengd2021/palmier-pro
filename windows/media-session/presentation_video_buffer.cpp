#include "palmier/media/presentation_video_buffer.hpp"

#include <limits>
#include <utility>

namespace palmier::media {
namespace {

[[noreturn]] void fail(PresentationVideoErrorCode code, const char* message) {
    throw PresentationVideoError(code, message);
}

bool sameTimeBase(const Rational& lhs, const Rational& rhs) noexcept {
    return lhs.numerator == rhs.numerator && lhs.denominator == rhs.denominator;
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
    const auto bytes = static_cast<std::uint64_t>(frame.source.pixels.size())
        * sizeof(render::Rgba32Float);
    queuedBytes_ -= bytes;
    ++revision_;
    return {
        receipt(PresentationVideoOperation::dequeue, PresentationVideoOutcome::changed),
        std::move(frame),
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
