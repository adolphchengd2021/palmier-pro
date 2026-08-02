#include "palmier/media/presentation_audio_decode_pump.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace palmier::media {
namespace {

constexpr std::uint64_t maximumQueuedAudioBytes = 256ULL * 1024 * 1024;
constexpr std::uint32_t maximumConfigurableAudioCapacityFrames = 4'194'304;
constexpr std::uint32_t maximumConfigurableAudioFramesPerFill = 65'536;

[[noreturn]] void fail(
    PresentationAudioDecodeErrorCode code,
    const char* message
) {
    throw PresentationAudioDecodeError(code, message);
}

bool isTerminal(PresentationAudioDecodeState state) noexcept {
    return state == PresentationAudioDecodeState::cancelled
        || state == PresentationAudioDecodeState::failed;
}

bool sameStart(
    const std::optional<DecodeFrameStart>& lhs,
    const std::optional<DecodeFrameStart>& rhs
) noexcept {
    if (lhs.has_value() != rhs.has_value()) return false;
    return !lhs.has_value()
        || (lhs->frameIndex == rhs->frameIndex
            && lhs->frameRate.numerator == rhs->frameRate.numerator
            && lhs->frameRate.denominator == rhs->frameRate.denominator);
}

}

PresentationAudioDecodeError::PresentationAudioDecodeError(
    PresentationAudioDecodeErrorCode codeValue,
    const char* message
) : std::runtime_error(message), code(codeValue) {}

PresentationAudioDecodePump::PresentationAudioDecodePump(
    PresentationAudioDecodeLimits limits
) : limits_(limits) {
    if (!audio::isValidPcmFormat(limits_.targetFormat)
        || limits_.capacityFrames == 0
        || limits_.capacityFrames > maximumConfigurableAudioCapacityFrames
        || limits_.maximumFramesPerFill == 0
        || limits_.maximumFramesPerFill > limits_.capacityFrames
        || limits_.maximumFramesPerFill
            > maximumConfigurableAudioFramesPerFill
        || limits_.maximumFramesPerFill
            > limits_.decode.maximumAudioFramesPerBlock
        || limits_.decode.maximumAudioFramesPerBlock
            > maximumConfigurableAudioFramesPerFill
        || static_cast<std::uint64_t>(
                limits_.decode.maximumAudioFramesPerBlock
            ) * limits_.targetFormat.blockAlign
            > maximumQueuedAudioBytes
        || static_cast<std::uint64_t>(limits_.capacityFrames)
                * limits_.targetFormat.blockAlign
            > maximumQueuedAudioBytes) {
        fail(
            PresentationAudioDecodeErrorCode::invalidLimits,
            "invalid presentation audio decode limits"
        );
    }
}

PresentationAudioReceipt PresentationAudioDecodePump::start(
    std::uint64_t generation,
    const std::filesystem::path& input,
    std::stop_token cancellation
) {
    return startInternal(generation, input, {}, cancellation);
}

PresentationAudioReceipt PresentationAudioDecodePump::start(
    std::uint64_t generation,
    const std::filesystem::path& input,
    DecodeFrameStart start,
    std::stop_token cancellation
) {
    return startInternal(generation, input, start, cancellation);
}

PresentationAudioReceipt PresentationAudioDecodePump::startInternal(
    std::uint64_t generation,
    const std::filesystem::path& input,
    std::optional<DecodeFrameStart> start,
    std::stop_token cancellation
) {
    if (generation == 0 || generation < generation_) {
        return receipt(PresentationAudioOutcome::stale);
    }
    if (generation == generation_) {
        if (input != inputIdentity_ || !sameStart(start, decodeStart_)) {
            fail(
                PresentationAudioDecodeErrorCode::changedInputWithinGeneration,
                "audio input cannot change within one generation"
            );
        }
        return receipt(PresentationAudioOutcome::noOp);
    }

    auto nextInputIdentity = input;
    auto nextReader = start.has_value()
        ? std::make_unique<FfmpegAudioFrameReader>(
            input,
            limits_.targetFormat,
            *start,
            limits_.decode,
            cancellation
        )
        : std::make_unique<FfmpegAudioFrameReader>(
            input,
            limits_.targetFormat,
            limits_.decode,
            cancellation
        );
    if (cancellation.stop_requested()) {
        throw MediaError(MediaFailureCode::cancelled, "before-audio-generation-commit", 0);
    }
    reader_ = std::move(nextReader);
    pendingBlock_.reset();
    pendingFrameOffset_ = 0;
    queue_.clear();
    queuedFrames_ = 0;
    inputIdentity_.swap(nextInputIdentity);
    decodeStart_ = start;
    generation_ = generation;
    state_ = PresentationAudioDecodeState::ready;
    return receipt(PresentationAudioOutcome::changed);
}

PresentationAudioReceipt PresentationAudioDecodePump::fill(
    std::uint64_t generation,
    std::stop_token cancellation
) {
    if (generation_ == 0) {
        fail(
            PresentationAudioDecodeErrorCode::notStarted,
            "audio decode pump has not started"
        );
    }
    if (generation != generation_) {
        return receipt(PresentationAudioOutcome::stale);
    }
    if (isTerminal(state_)) {
        fail(
            PresentationAudioDecodeErrorCode::terminalState,
            "audio decode generation is terminal"
        );
    }
    if (state_ == PresentationAudioDecodeState::endOfStream) {
        return receipt(PresentationAudioOutcome::noOp);
    }
    if (reader_ == nullptr) {
        fail(
            PresentationAudioDecodeErrorCode::invariantViolation,
            "active audio decode generation has no reader"
        );
    }

    std::uint32_t admitted = 0;
    try {
        while (admitted < limits_.maximumFramesPerFill) {
            if (!pendingBlock_.has_value()) {
                pendingBlock_ = reader_->nextBlock(cancellation);
                pendingFrameOffset_ = 0;
                if (cancellation.stop_requested()) {
                    throw MediaError(MediaFailureCode::cancelled, "after-audio-decode", 0);
                }
                if (!pendingBlock_.has_value()) {
                    reader_.reset();
                    state_ = PresentationAudioDecodeState::endOfStream;
                    return receipt(
                        admitted == 0
                            ? PresentationAudioOutcome::noOp
                            : PresentationAudioOutcome::changed,
                        admitted
                    );
                }
            }

            if (queuedFrames_ > limits_.capacityFrames
                || pendingFrameOffset_ > pendingBlock_->frameCount) {
                fail(
                    PresentationAudioDecodeErrorCode::invariantViolation,
                    "audio decode pump accounting is invalid"
                );
            }
            const std::uint32_t freeFrames = limits_.capacityFrames - queuedFrames_;
            if (freeFrames == 0) {
                state_ = PresentationAudioDecodeState::blocked;
                return receipt(
                    admitted == 0
                        ? PresentationAudioOutcome::refused
                        : PresentationAudioOutcome::changed,
                    admitted
                );
            }
            const auto remaining = pendingBlock_->frameCount
                - static_cast<std::uint32_t>(pendingFrameOffset_);
            const auto fillBudget = limits_.maximumFramesPerFill - admitted;
            const std::uint32_t frameCount = std::min({
                remaining,
                freeFrames,
                fillBudget,
            });
            if (frameCount == 0) {
                fail(
                    PresentationAudioDecodeErrorCode::invariantViolation,
                    "audio decode pump made no admission progress"
                );
            }
            const std::size_t firstByte = pendingFrameOffset_
                * limits_.targetFormat.blockAlign;
            const std::size_t byteCount = static_cast<std::size_t>(frameCount)
                * limits_.targetFormat.blockAlign;
            std::vector<std::byte> bytes(
                pendingBlock_->interleavedBytes.begin()
                    + static_cast<std::ptrdiff_t>(firstByte),
                pendingBlock_->interleavedBytes.begin()
                    + static_cast<std::ptrdiff_t>(firstByte + byteCount)
            );
            if (cancellation.stop_requested()) {
                throw MediaError(
                    MediaFailureCode::cancelled,
                    "before-audio-block-admission",
                    0
                );
            }
            queue_.push_back({
                pendingBlock_->sourcePresentationTimestamp,
                pendingBlock_->sourceTimeBase,
                pendingBlock_->startOutputSample
                    + static_cast<std::int64_t>(pendingFrameOffset_),
                frameCount,
                pendingBlock_->format,
                std::move(bytes),
            });
            pendingFrameOffset_ += frameCount;
            queuedFrames_ += frameCount;
            admitted += frameCount;
            state_ = PresentationAudioDecodeState::ready;
            if (pendingFrameOffset_ == pendingBlock_->frameCount) {
                pendingBlock_.reset();
                pendingFrameOffset_ = 0;
            }
        }
        return receipt(PresentationAudioOutcome::changed, admitted);
    } catch (const MediaError& error) {
        terminate(
            error.code == MediaFailureCode::cancelled
                ? PresentationAudioDecodeState::cancelled
                : PresentationAudioDecodeState::failed
        );
        throw;
    } catch (...) {
        terminate(PresentationAudioDecodeState::failed);
        throw;
    }
}

PresentationAudioTake PresentationAudioDecodePump::dequeue(
    std::uint64_t generation
) {
    if (generation != generation_) {
        return {PresentationAudioOutcome::stale, std::nullopt};
    }
    if (queue_.empty()) {
        return {PresentationAudioOutcome::noOp, std::nullopt};
    }
    auto block = std::move(queue_.front());
    queue_.pop_front();
    queuedFrames_ -= block.frameCount;
    if (state_ == PresentationAudioDecodeState::blocked) {
        state_ = PresentationAudioDecodeState::ready;
    }
    return {PresentationAudioOutcome::changed, std::move(block)};
}

PresentationAudioReceipt PresentationAudioDecodePump::cancel(
    std::uint64_t generation
) {
    if (generation != generation_) {
        return receipt(PresentationAudioOutcome::stale);
    }
    if (state_ == PresentationAudioDecodeState::cancelled) {
        return receipt(PresentationAudioOutcome::noOp);
    }
    terminate(PresentationAudioDecodeState::cancelled);
    return receipt(PresentationAudioOutcome::changed);
}

std::uint64_t PresentationAudioDecodePump::generation() const noexcept {
    return generation_;
}

PresentationAudioDecodeState PresentationAudioDecodePump::state() const noexcept {
    return state_;
}

PresentationAudioReceipt PresentationAudioDecodePump::receipt(
    PresentationAudioOutcome outcome,
    std::uint32_t admittedFrames
) const noexcept {
    return {
        generation_,
        state_,
        outcome,
        admittedFrames,
        queuedFrames_,
        pendingBlock_.has_value(),
    };
}

void PresentationAudioDecodePump::terminate(
    PresentationAudioDecodeState state
) {
    reader_.reset();
    pendingBlock_.reset();
    pendingFrameOffset_ = 0;
    queue_.clear();
    queuedFrames_ = 0;
    state_ = state;
}

}
