#include "palmier/media/presentation_video_decode_pump.hpp"

#include <utility>

namespace palmier::media {
namespace {

constexpr std::size_t maximumConfigurableFramesPerFill = 32;

[[noreturn]] void fail(
    PresentationVideoDecodeErrorCode code,
    const char* message
) {
    throw PresentationVideoDecodeError(code, message);
}

bool isTerminal(PresentationVideoDecodeState state) noexcept {
    return state == PresentationVideoDecodeState::cancelled
        || state == PresentationVideoDecodeState::failed;
}

}

PresentationVideoDecodeError::PresentationVideoDecodeError(
    PresentationVideoDecodeErrorCode codeValue,
    const char* message
) : std::runtime_error(message), code(codeValue) {}

PresentationVideoDecodePump::PresentationVideoDecodePump(
    PresentationVideoDecodeLimits limits
) : PresentationVideoDecodePump(limits, {}) {}

PresentationVideoDecodePump::PresentationVideoDecodePump(
    PresentationVideoDecodeLimits limits,
    StartCommitCheckpoint startCommitCheckpoint
) : limits_(limits),
    buffer_(limits.buffer),
    startCommitCheckpoint_(std::move(startCommitCheckpoint)) {
    if (limits_.decode.maximumPixels > render::maximumRenderFramePixels) {
        fail(
            PresentationVideoDecodeErrorCode::decodeLimitExceedsRenderBudget,
            "video decode pixel limit exceeds the render budget"
        );
    }
    if (limits_.maximumFramesPerFill == 0
        || limits_.maximumFramesPerFill > maximumConfigurableFramesPerFill) {
        fail(
            PresentationVideoDecodeErrorCode::invalidFillBudget,
            "video decode fill budget must be between one and 32 frames"
        );
    }
}

PresentationVideoReceipt PresentationVideoDecodePump::start(
    std::uint64_t generation,
    const std::filesystem::path& input,
    std::stop_token cancellation
) {
    if (generation < buffer_.generation()) {
        return buffer_.start(generation);
    }
    if (generation == buffer_.generation()) {
        if (generation != 0 && input != inputIdentity_) {
            fail(
                PresentationVideoDecodeErrorCode::changedInputWithinGeneration,
                "video input cannot change within one generation"
            );
        }
        return buffer_.start(generation);
    }

    auto nextInputIdentity = input;
    auto nextReader = std::make_unique<FfmpegVideoFrameReader>(
        input,
        limits_.decode,
        cancellation
    );
    if (startCommitCheckpoint_) {
        startCommitCheckpoint_();
    }
    if (cancellation.stop_requested()) {
        throw MediaError(
            MediaFailureCode::cancelled,
            "before-generation-commit",
            0
        );
    }
    auto receipt = buffer_.start(generation);
    reader_ = std::move(nextReader);
    pendingFrame_.reset();
    inputIdentity_.swap(nextInputIdentity);
    state_ = PresentationVideoDecodeState::ready;
    return receipt;
}

PresentationVideoFillReceipt PresentationVideoDecodePump::fill(
    std::uint64_t generation,
    std::stop_token cancellation
) {
    if (buffer_.generation() == 0) {
        fail(
            PresentationVideoDecodeErrorCode::notStarted,
            "video decode pump has not started"
        );
    }
    if (generation != buffer_.generation()) {
        return fillReceipt(PresentationVideoOutcome::stale, 0);
    }
    if (isTerminal(state_)) {
        fail(
            PresentationVideoDecodeErrorCode::terminalState,
            "video decode generation is terminal"
        );
    }
    if (state_ == PresentationVideoDecodeState::endOfStream) {
        return fillReceipt(PresentationVideoOutcome::noOp, 0);
    }
    if (reader_ == nullptr) {
        fail(
            PresentationVideoDecodeErrorCode::invariantViolation,
            "active video decode generation has no reader"
        );
    }

    std::size_t admittedFrames = 0;
    try {
        for (;;) {
            if (!pendingFrame_) {
                pendingFrame_ = reader_->nextFrame(cancellation);
                if (cancellation.stop_requested()) {
                    throw MediaError(
                        MediaFailureCode::cancelled,
                        "after-decode-frame",
                        0
                    );
                }
                if (!pendingFrame_) {
                    reader_.reset();
                    state_ = PresentationVideoDecodeState::endOfStream;
                    return fillReceipt(
                        admittedFrames == 0
                            ? PresentationVideoOutcome::noOp
                            : PresentationVideoOutcome::changed,
                        admittedFrames
                    );
                }
            }

            const auto enqueue = buffer_.enqueue(
                generation,
                *pendingFrame_,
                cancellation
            );
            if (enqueue.outcome == PresentationVideoOutcome::refused) {
                state_ = PresentationVideoDecodeState::blocked;
                return fillReceipt(
                    admittedFrames == 0
                        ? PresentationVideoOutcome::refused
                        : PresentationVideoOutcome::changed,
                    admittedFrames
                );
            }
            if (enqueue.outcome == PresentationVideoOutcome::cancelled) {
                throw MediaError(
                    MediaFailureCode::cancelled,
                    "enqueue-presentation-frame",
                    0
                );
            }
            if (enqueue.outcome != PresentationVideoOutcome::changed) {
                fail(
                    PresentationVideoDecodeErrorCode::invariantViolation,
                    "video decode pump received an inconsistent enqueue receipt"
                );
            }
            pendingFrame_.reset();
            ++admittedFrames;
            state_ = PresentationVideoDecodeState::ready;
            if (admittedFrames >= limits_.maximumFramesPerFill) {
                return fillReceipt(
                    PresentationVideoOutcome::changed,
                    admittedFrames
                );
            }
        }
    } catch (const MediaError& error) {
        terminate(
            error.code == MediaFailureCode::cancelled
                ? PresentationVideoDecodeState::cancelled
                : PresentationVideoDecodeState::failed
        );
        throw;
    } catch (...) {
        terminate(PresentationVideoDecodeState::failed);
        throw;
    }
}

PresentationVideoTake PresentationVideoDecodePump::dequeue(
    std::uint64_t generation
) {
    return buffer_.dequeue(generation);
}

PresentationVideoSelection PresentationVideoDecodePump::select(
    std::uint64_t generation,
    std::uint64_t expectedRevision,
    const PresentationVideoClockPosition& clock
) {
    return buffer_.select(generation, expectedRevision, clock);
}

PresentationVideoReceipt PresentationVideoDecodePump::cancel(
    std::uint64_t generation
) {
    auto receipt = buffer_.cancel(generation);
    if (generation == buffer_.generation()) {
        reader_.reset();
        pendingFrame_.reset();
        state_ = PresentationVideoDecodeState::cancelled;
    }
    return receipt;
}

std::uint64_t PresentationVideoDecodePump::generation() const noexcept {
    return buffer_.generation();
}

std::uint64_t PresentationVideoDecodePump::revision() const noexcept {
    return buffer_.revision();
}

PresentationVideoDecodeState PresentationVideoDecodePump::state() const noexcept {
    return state_;
}

PresentationVideoFillReceipt PresentationVideoDecodePump::fillReceipt(
    PresentationVideoOutcome outcome,
    std::size_t admittedFrames
) const noexcept {
    return {
        buffer_.generation(),
        state_,
        outcome,
        admittedFrames,
        pendingFrame_.has_value(),
        buffer_.queuedFrames(),
        buffer_.queuedBytes(),
    };
}

void PresentationVideoDecodePump::terminate(
    PresentationVideoDecodeState state
) {
    reader_.reset();
    pendingFrame_.reset();
    buffer_.cancel(buffer_.generation());
    state_ = state;
}

}
