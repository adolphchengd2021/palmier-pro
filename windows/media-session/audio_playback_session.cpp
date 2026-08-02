#include "palmier/media/audio_playback_session.hpp"

#include "palmier/media/presentation_audio_decode_pump.hpp"

#include <Windows.h>

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace palmier::media {
namespace {

constexpr HRESULT cancelledResult = HRESULT_FROM_WIN32(ERROR_CANCELLED);
constexpr std::uint32_t maximumFillFrames = 65'536;
constexpr std::uint32_t maximumPumpFrames = 4'194'304;

struct CommandCompletion final {
    std::mutex mutex;
    std::condition_variable condition;
    std::optional<AudioPlaybackReceipt> value;
};

enum class CommandKind {
    play,
    pause,
    resume,
    cancel,
    close,
};

struct Command final {
    CommandKind kind{CommandKind::close};
    std::filesystem::path input;
    std::int64_t timelineFrame{};
    std::optional<DecodeFrameStart> decodeStart;
    std::uint64_t expectedGeneration{};
    bool startPaused{};
    std::stop_token cancellation;
    std::shared_ptr<CommandCompletion> completion;
};

struct PlaybackSourceAnchor final {
    std::int64_t presentationTimestamp{};
    Rational timeBase;
};

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

void complete(
    const std::shared_ptr<CommandCompletion>& completion,
    AudioPlaybackReceipt value
) {
    {
        std::lock_guard lock(completion->mutex);
        completion->value = std::move(value);
    }
    completion->condition.notify_all();
}

AudioPlaybackOutcome outputOutcome(audio::WasapiOutputOutcome outcome) {
    switch (outcome) {
    case audio::WasapiOutputOutcome::changed:
        return AudioPlaybackOutcome::changed;
    case audio::WasapiOutputOutcome::noOp:
        return AudioPlaybackOutcome::noOp;
    case audio::WasapiOutputOutcome::cancelled:
        return AudioPlaybackOutcome::cancelled;
    case audio::WasapiOutputOutcome::refused:
        return AudioPlaybackOutcome::refused;
    case audio::WasapiOutputOutcome::failed:
        return AudioPlaybackOutcome::failed;
    case audio::WasapiOutputOutcome::invalidated:
        return AudioPlaybackOutcome::invalidated;
    }
    return AudioPlaybackOutcome::failed;
}

AudioPlaybackState outputState(audio::WasapiOutputOutcome outcome) {
    switch (outcome) {
    case audio::WasapiOutputOutcome::invalidated:
        return AudioPlaybackState::invalidated;
    case audio::WasapiOutputOutcome::cancelled:
        return AudioPlaybackState::cancelled;
    case audio::WasapiOutputOutcome::changed:
    case audio::WasapiOutputOutcome::noOp:
    case audio::WasapiOutputOutcome::refused:
    case audio::WasapiOutputOutcome::failed:
        return AudioPlaybackState::failed;
    }
    return AudioPlaybackState::failed;
}

}

class AudioPlaybackSession::Impl final {
public:
    explicit Impl(std::unique_ptr<audio::WasapiOutputWorker> outputWorker)
        : output_(validatedOutput(std::move(outputWorker))),
          worker_([this](std::stop_token token) { run(token); }) {
    }

    ~Impl() noexcept {
        try {
            close();
        } catch (...) {
            worker_.request_stop();
            requestHandoffCancellation();
            condition_.notify_all();
            if (worker_.joinable()) {
                try {
                    worker_.join();
                } catch (...) {
                }
            }
        }
    }

    AudioPlaybackReceipt play(
        const std::filesystem::path& input,
        std::int64_t timelineFrame
    ) {
        return command(CommandKind::play, input, timelineFrame, 0);
    }

    AudioPlaybackReceipt play(
        const std::filesystem::path& input,
        std::int64_t timelineFrame,
        DecodeFrameStart start
    ) {
        return command(CommandKind::play, input, timelineFrame, 0, {}, start);
    }

    AudioPlaybackReceipt playExactGeneration(
        const std::filesystem::path& input,
        std::int64_t timelineFrame,
        std::uint64_t generation,
        std::stop_token cancellation
    ) {
        if (generation == 0) {
            auto value = snapshot();
            value.outcome = AudioPlaybackOutcome::refused;
            value.failure = AudioPlaybackFailureCode::invalidRequest;
            value.hresult = E_INVALIDARG;
            return value;
        }
        return command(
            CommandKind::play,
            input,
            timelineFrame,
            generation,
            cancellation
        );
    }

    AudioPlaybackReceipt playExactGeneration(
        const std::filesystem::path& input,
        std::int64_t timelineFrame,
        DecodeFrameStart start,
        std::uint64_t generation,
        std::stop_token cancellation
    ) {
        if (generation == 0) {
            auto value = snapshot();
            value.outcome = AudioPlaybackOutcome::refused;
            value.failure = AudioPlaybackFailureCode::invalidRequest;
            value.hresult = E_INVALIDARG;
            return value;
        }
        return command(
            CommandKind::play,
            input,
            timelineFrame,
            generation,
            cancellation,
            start,
            false
        );
    }

    AudioPlaybackReceipt preparePausedExactGeneration(
        const std::filesystem::path& input,
        std::int64_t timelineFrame,
        DecodeFrameStart start,
        std::uint64_t generation,
        std::stop_token cancellation
    ) {
        if (generation == 0) {
            auto value = snapshot();
            value.outcome = AudioPlaybackOutcome::refused;
            value.failure = AudioPlaybackFailureCode::invalidRequest;
            value.hresult = E_INVALIDARG;
            return value;
        }
        return command(
            CommandKind::play,
            input,
            timelineFrame,
            generation,
            cancellation,
            start,
            true
        );
    }

    AudioPlaybackReceipt cancel(std::uint64_t expectedGeneration) {
        return command(CommandKind::cancel, {}, 0, expectedGeneration);
    }

    AudioPlaybackReceipt pause(std::uint64_t expectedGeneration) {
        return command(CommandKind::pause, {}, 0, expectedGeneration);
    }

    AudioPlaybackReceipt resume(std::uint64_t expectedGeneration) {
        return command(CommandKind::resume, {}, 0, expectedGeneration);
    }

    AudioPlaybackReceipt snapshot() const {
        std::lock_guard lock(mutex_);
        return snapshot_;
    }

    AudioPlaybackPositionReceipt position(
        std::uint64_t expectedGeneration
    ) const {
        const auto current = snapshot();
        AudioPlaybackPositionReceipt value;
        value.generation = current.generation;
        value.state = current.state;
        value.failure = current.failure;
        value.hresult = current.hresult;
        value.hasClockAnchor = current.hasClockAnchor;
        value.clockAnchor = current.clockAnchor;
        if (expectedGeneration == 0
            || expectedGeneration != current.generation) {
            value.outcome = AudioPlaybackOutcome::refused;
            value.failure = AudioPlaybackFailureCode::invalidRequest;
            value.hresult = E_INVALIDARG;
            return value;
        }
        if (current.state == AudioPlaybackState::cancelled) {
            value.outcome = AudioPlaybackOutcome::cancelled;
            return value;
        }
        if (current.state == AudioPlaybackState::invalidated) {
            value.outcome = AudioPlaybackOutcome::invalidated;
            return value;
        }
        if (current.state == AudioPlaybackState::failed) {
            value.outcome = AudioPlaybackOutcome::failed;
            return value;
        }
        if (current.state == AudioPlaybackState::closed) {
            value.outcome = AudioPlaybackOutcome::noOp;
            return value;
        }
        if (!current.hasClockAnchor) {
            value.outcome = AudioPlaybackOutcome::noOp;
            value.hresult = E_PENDING;
            return value;
        }

        const auto clock = output_->clockPosition(expectedGeneration);
        value.generation = clock.generation;
        value.hresult = clock.hresult;
        switch (clock.outcome) {
        case audio::WasapiWorkerClockOutcome::available:
            if (!clock.hasSample
                || clock.generation != expectedGeneration
                || clock.sample.generation != expectedGeneration
                || clock.sample.devicePosition
                    < current.clockAnchor.value.devicePosition) {
                value.state = AudioPlaybackState::failed;
                value.outcome = AudioPlaybackOutcome::failed;
                value.failure = AudioPlaybackFailureCode::invariantFailure;
                value.hresult = E_UNEXPECTED;
                return value;
            }
            value.outcome = AudioPlaybackOutcome::noOp;
            value.hasClockSample = true;
            value.clockSample = clock.sample;
            return value;
        case audio::WasapiWorkerClockOutcome::noSample:
            value.outcome = AudioPlaybackOutcome::noOp;
            return value;
        case audio::WasapiWorkerClockOutcome::refused:
            value.outcome = AudioPlaybackOutcome::refused;
            value.failure = AudioPlaybackFailureCode::invalidRequest;
            return value;
        case audio::WasapiWorkerClockOutcome::unavailable:
            value.state = AudioPlaybackState::invalidated;
            value.outcome = AudioPlaybackOutcome::invalidated;
            value.failure = AudioPlaybackFailureCode::deviceUnavailable;
            return value;
        case audio::WasapiWorkerClockOutcome::failed:
            value.state = AudioPlaybackState::failed;
            value.outcome = AudioPlaybackOutcome::failed;
            value.failure = AudioPlaybackFailureCode::deviceFailure;
            return value;
        case audio::WasapiWorkerClockOutcome::closed:
            value.state = AudioPlaybackState::closed;
            value.outcome = AudioPlaybackOutcome::refused;
            value.failure = AudioPlaybackFailureCode::outputFailure;
            return value;
        }
        value.state = AudioPlaybackState::failed;
        value.outcome = AudioPlaybackOutcome::failed;
        value.failure = AudioPlaybackFailureCode::invariantFailure;
        value.hresult = E_UNEXPECTED;
        return value;
    }

    AudioPlaybackReceipt waitForTerminal(
        std::uint64_t generation,
        std::stop_token stopToken
    ) {
        std::unique_lock lock(mutex_);
        if (generation == 0) {
            auto value = snapshot_;
            value.outcome = AudioPlaybackOutcome::refused;
            value.failure = AudioPlaybackFailureCode::invalidRequest;
            value.hresult = E_INVALIDARG;
            return value;
        }
        const bool ready = condition_.wait(
            lock,
            stopToken,
            [this, generation] {
                return terminalForGenerationLocked(generation).has_value()
                    || (snapshot_.generation != 0
                        && snapshot_.generation != generation)
                    || exited_;
            }
        );
        if (ready) {
            const auto terminal = terminalForGenerationLocked(generation);
            if (terminal.has_value()) {
                return *terminal;
            }
        }
        if (snapshot_.generation != 0 && snapshot_.generation != generation) {
            auto value = snapshot_;
            value.outcome = AudioPlaybackOutcome::refused;
            value.stage = AudioPlaybackStage::installGeneration;
            value.failure = AudioPlaybackFailureCode::invalidRequest;
            value.hresult = E_INVALIDARG;
            return value;
        }
        if (exited_ && closeReceipt_.has_value()) {
            return *closeReceipt_;
        }
        auto value = snapshot_;
        value.generation = generation;
        value.outcome = stopToken.stop_requested()
            ? AudioPlaybackOutcome::cancelled
            : AudioPlaybackOutcome::refused;
        value.hresult = stopToken.stop_requested()
            ? cancelledResult
            : E_ILLEGAL_METHOD_CALL;
        return value;
    }

    AudioPlaybackReceipt close() {
        bool alreadyExited = false;
        {
            std::unique_lock lock(mutex_);
            if (closeFinished_) {
                return closeReceipt_.value_or(snapshot_);
            }
            if (closeStarted_) {
                condition_.wait(lock, [this] { return closeFinished_; });
                return closeReceipt_.value_or(snapshot_);
            }
            closeStarted_ = true;
            alreadyExited = exited_;
            handoffCancellation_.request_stop();
        }
        condition_.notify_all();

        AudioPlaybackReceipt value;
        if (alreadyExited) {
            value = closeReceipt_.value_or(snapshot());
        } else {
            try {
                value = command(CommandKind::close, {}, 0, 0);
            } catch (const std::bad_alloc&) {
                value = failureReceipt(
                    AudioPlaybackStage::close,
                    AudioPlaybackFailureCode::deviceFailure,
                    E_OUTOFMEMORY
                );
                worker_.request_stop();
                requestHandoffCancellation();
                condition_.notify_all();
            } catch (...) {
                value = failureReceipt(
                    AudioPlaybackStage::close,
                    AudioPlaybackFailureCode::deviceFailure,
                    E_UNEXPECTED
                );
                worker_.request_stop();
                requestHandoffCancellation();
                condition_.notify_all();
            }
        }
        try {
            if (worker_.joinable()) {
                worker_.join();
            }
        } catch (...) {
            value = failureReceipt(
                AudioPlaybackStage::close,
                AudioPlaybackFailureCode::deviceFailure,
                E_UNEXPECTED
            );
        }
        {
            std::lock_guard lock(mutex_);
            exited_ = true;
            closeFinished_ = true;
            closeReceipt_ = value;
        }
        condition_.notify_all();
        return value;
    }

private:
    static std::unique_ptr<audio::WasapiOutputWorker> validatedOutput(
        std::unique_ptr<audio::WasapiOutputWorker> output
    ) {
        if (!output) {
            throw std::invalid_argument("missing WASAPI output worker");
        }
        return output;
    }

    AudioPlaybackReceipt command(
        CommandKind kind,
        const std::filesystem::path& input,
        std::int64_t timelineFrame,
        std::uint64_t expectedGeneration,
        std::stop_token cancellation = {},
        std::optional<DecodeFrameStart> decodeStart = {},
        bool startPaused = false
    ) {
        auto completion = std::make_shared<CommandCompletion>();
        {
            std::lock_guard lock(mutex_);
            if (exited_ || (closeStarted_ && kind != CommandKind::close)) {
                auto value = snapshot_;
                value.outcome = AudioPlaybackOutcome::refused;
                value.stage = kind == CommandKind::close
                    ? AudioPlaybackStage::close
                    : AudioPlaybackStage::none;
                value.hresult = E_ILLEGAL_METHOD_CALL;
                return value;
            }
            constexpr std::size_t maximumPendingCommands = 8;
            if (kind != CommandKind::close
                && commands_.size() >= maximumPendingCommands) {
                auto value = snapshot_;
                value.outcome = AudioPlaybackOutcome::refused;
                value.failure = AudioPlaybackFailureCode::invalidRequest;
                value.hresult = HRESULT_FROM_WIN32(ERROR_RETRY);
                return value;
            }
            commands_.push_back({
                kind,
                input,
                timelineFrame,
                decodeStart,
                expectedGeneration,
                startPaused,
                cancellation,
                completion,
            });
            handoffCancellation_.request_stop();
        }
        condition_.notify_all();
        std::unique_lock completionLock(completion->mutex);
        completion->condition.wait(
            completionLock,
            [&completion] { return completion->value.has_value(); }
        );
        return *completion->value;
    }

    AudioPlaybackReceipt failureReceipt(
        AudioPlaybackStage stage,
        AudioPlaybackFailureCode failure,
        HRESULT result
    ) const {
        auto value = snapshot();
        value.state = AudioPlaybackState::failed;
        value.outcome = AudioPlaybackOutcome::failed;
        value.stage = stage;
        value.failure = failure;
        value.hresult = result;
        return value;
    }

    AudioPlaybackReceipt mediaFailureReceipt(
        const MediaError& error,
        AudioPlaybackStage stage,
        const AudioPlaybackReceipt& previous
    ) const {
        auto value = previous;
        if (error.code == MediaFailureCode::cancelled) {
            value.outcome = AudioPlaybackOutcome::cancelled;
            value.stage = stage;
            value.failure = AudioPlaybackFailureCode::none;
            value.mediaFailureCode = static_cast<std::int32_t>(error.code);
            value.hresult = cancelledResult;
            return value;
        }
        value.outcome = AudioPlaybackOutcome::refused;
        value.stage = stage;
        value.failure = AudioPlaybackFailureCode::mediaFailure;
        value.mediaFailureCode = static_cast<std::int32_t>(error.code);
        value.hresult = E_FAIL;
        return value;
    }

    void publish(AudioPlaybackReceipt value, bool terminal = false) {
        std::lock_guard lock(mutex_);
        snapshot_ = value;
        if (value.state == AudioPlaybackState::closed) {
            closeReceipt_ = value;
        } else if (terminal) {
            const auto existing = std::find_if(
                terminalHistory_.begin(),
                terminalHistory_.end(),
                [&value](const AudioPlaybackReceipt& receipt) {
                    return receipt.generation == value.generation;
                }
            );
            if (existing != terminalHistory_.end()) {
                *existing = value;
            } else {
                constexpr std::size_t terminalHistoryCapacity = 16;
                if (terminalHistory_.size() == terminalHistoryCapacity) {
                    terminalHistory_.pop_front();
                }
                terminalHistory_.push_back(value);
            }
        }
        condition_.notify_all();
    }

    std::optional<AudioPlaybackReceipt> terminalForGenerationLocked(
        std::uint64_t generation
    ) const {
        const auto value = std::find_if(
            terminalHistory_.begin(),
            terminalHistory_.end(),
            [generation](const AudioPlaybackReceipt& receipt) {
                return receipt.generation == generation;
            }
        );
        return value == terminalHistory_.end()
            ? std::nullopt
            : std::optional<AudioPlaybackReceipt>(*value);
    }

    void beginCommandHandoffCancellation(CommandKind kind) {
        std::lock_guard lock(mutex_);
        handoffCancellation_ = std::stop_source{};
        if (!commands_.empty()
            || (closeStarted_ && kind != CommandKind::close)
            || worker_.get_stop_token().stop_requested()) {
            handoffCancellation_.request_stop();
        }
    }

    void requestHandoffCancellation() {
        std::lock_guard lock(mutex_);
        handoffCancellation_.request_stop();
    }

    std::stop_token handoffToken() const {
        std::lock_guard lock(mutex_);
        return handoffCancellation_.get_token();
    }

    AudioPlaybackReceipt outputFailureReceipt(
        const audio::WasapiOutputReceipt& output,
        AudioPlaybackStage stage
    ) const {
        auto value = snapshot();
        value.generation = output.generation;
        value.state = outputState(output.outcome);
        value.outcome = outputOutcome(output.outcome);
        value.stage = stage;
        value.failure = AudioPlaybackFailureCode::outputFailure;
        value.hresult = output.hresult;
        value.acceptedFrames = outputCursor_;
        return value;
    }

    AudioPlaybackReceipt pcmFailureReceipt(
        const audio::WasapiWorkerPcmReceipt& output
    ) const {
        auto value = snapshot();
        value.generation = generation_;
        value.state = output.outcome == audio::WasapiWorkerPcmOutcome::cancelled
            ? AudioPlaybackState::cancelled
            : AudioPlaybackState::failed;
        value.outcome = output.outcome == audio::WasapiWorkerPcmOutcome::cancelled
            ? AudioPlaybackOutcome::cancelled
            : (output.outcome == audio::WasapiWorkerPcmOutcome::refused
                ? AudioPlaybackOutcome::refused
                : AudioPlaybackOutcome::failed);
        value.stage = AudioPlaybackStage::pcmHandoff;
        value.failure = output.outcome == audio::WasapiWorkerPcmOutcome::cancelled
            ? AudioPlaybackFailureCode::none
            : AudioPlaybackFailureCode::handoffFailure;
        value.hresult = output.hresult;
        value.acceptedFrames = outputCursor_;
        return value;
    }

    bool collectPendingBlock(
        PresentationAudioDecodePump& pump,
        std::uint64_t generation,
        std::uint32_t maximumFrames,
        std::stop_token stopToken,
        std::optional<DecodedAudioBlock>& destination,
        AudioPlaybackReceipt& failure
    ) {
        if (destination.has_value()) {
            failure = failureReceipt(
                AudioPlaybackStage::pcmHandoff,
                AudioPlaybackFailureCode::invariantFailure,
                E_UNEXPECTED
            );
            return false;
        }
        try {
            for (;;) {
                if (stopToken.stop_requested()) {
                    failure = snapshot();
                    failure.generation = generation;
                    failure.state = AudioPlaybackState::cancelled;
                    failure.outcome = AudioPlaybackOutcome::cancelled;
                    failure.stage = AudioPlaybackStage::pcmHandoff;
                    failure.failure = AudioPlaybackFailureCode::none;
                    failure.hresult = cancelledResult;
                    return false;
                }

                auto take = pump.dequeue(generation);
                if (take.outcome == PresentationAudioOutcome::noOp) {
                    break;
                }
                if (take.outcome != PresentationAudioOutcome::changed
                    || !take.block.has_value()) {
                    failure = failureReceipt(
                        AudioPlaybackStage::pcmHandoff,
                        AudioPlaybackFailureCode::invariantFailure,
                        E_UNEXPECTED
                    );
                    return false;
                }

                auto next = std::move(*take.block);
                const std::size_t nextByteCount = static_cast<std::size_t>(
                    next.frameCount
                ) * next.format.blockAlign;
                if (next.frameCount == 0
                    || next.frameCount > maximumFrames
                    || next.interleavedBytes.size() != nextByteCount) {
                    failure = failureReceipt(
                        AudioPlaybackStage::pcmHandoff,
                        AudioPlaybackFailureCode::invariantFailure,
                        E_UNEXPECTED
                    );
                    return false;
                }
                if (!destination.has_value()) {
                    destination = std::move(next);
                    continue;
                }

                if (destination->format != next.format
                    || destination->sourceTimeBase.numerator
                        != next.sourceTimeBase.numerator
                    || destination->sourceTimeBase.denominator
                        != next.sourceTimeBase.denominator
                    || destination->frameCount
                        > maximumFrames - next.frameCount
                    || destination->startOutputSample
                        > (std::numeric_limits<std::int64_t>::max)()
                            - static_cast<std::int64_t>(
                                destination->frameCount
                            )
                    || next.startOutputSample
                        != destination->startOutputSample
                            + static_cast<std::int64_t>(
                                destination->frameCount
                            )) {
                    failure = failureReceipt(
                        AudioPlaybackStage::pcmHandoff,
                        AudioPlaybackFailureCode::invariantFailure,
                        E_UNEXPECTED
                    );
                    return false;
                }
                destination->interleavedBytes.insert(
                    destination->interleavedBytes.end(),
                    next.interleavedBytes.begin(),
                    next.interleavedBytes.end()
                );
                destination->frameCount += next.frameCount;
            }
        } catch (...) {
            failure = failureReceipt(
                AudioPlaybackStage::pcmHandoff,
                AudioPlaybackFailureCode::invariantFailure,
                E_UNEXPECTED
            );
            return false;
        }
        return true;
    }

    bool acceptBlock(
        const DecodedAudioBlock& block,
        std::stop_token stopToken,
        AudioPlaybackReceipt& failure
    ) {
        const bool firstBlock = !sourceCursor_.has_value();
        if (block.format != configuration_->pcmFormat
            || block.frameCount == 0
            || block.sourceTimeBase.numerator <= 0
            || block.sourceTimeBase.denominator <= 0
            || (!firstBlock
                && (!sourceAnchor_.has_value()
                    || sourceAnchor_->timeBase.numerator
                        != block.sourceTimeBase.numerator
                    || sourceAnchor_->timeBase.denominator
                        != block.sourceTimeBase.denominator))) {
            failure = failureReceipt(
                AudioPlaybackStage::pcmHandoff,
                AudioPlaybackFailureCode::invariantFailure,
                E_UNEXPECTED
            );
            return false;
        }
        const auto expectedSourceSample = firstBlock
            ? block.startOutputSample
            : *sourceCursor_;
        if (block.startOutputSample != expectedSourceSample
            || block.frameCount
                > (std::numeric_limits<std::uint64_t>::max)() - outputCursor_
            || expectedSourceSample
                > (std::numeric_limits<std::int64_t>::max)()
                    - static_cast<std::int64_t>(block.frameCount)) {
            failure = failureReceipt(
                AudioPlaybackStage::pcmHandoff,
                AudioPlaybackFailureCode::invariantFailure,
                E_UNEXPECTED
            );
            return false;
        }
        const auto result = output_->submit(
            {
                generation_,
                outputCursor_,
                block.frameCount,
                block.format,
                block.interleavedBytes,
            },
            stopToken
        );
        if (result.outcome == audio::WasapiWorkerPcmOutcome::accepted) {
            if (firstBlock) {
                sourceCursor_ = block.startOutputSample;
                sourceAnchor_ = PlaybackSourceAnchor{
                    block.sourcePresentationTimestamp,
                    block.sourceTimeBase,
                };
            }
            outputCursor_ += block.frameCount;
            *sourceCursor_ += static_cast<std::int64_t>(block.frameCount);
            return true;
        }
        failure = pcmFailureReceipt(result);
        return false;
    }

    bool prebuffer(
        PresentationAudioDecodePump& candidate,
        std::uint64_t generation,
        std::uint32_t targetFrames,
        std::stop_token stopToken,
        AudioPlaybackReceipt& failure,
        const AudioPlaybackReceipt& previous
    ) {
        try {
            while (candidate.state() != PresentationAudioDecodeState::endOfStream) {
                if (stopToken.stop_requested()) {
                    failure = previous;
                    failure.outcome = AudioPlaybackOutcome::cancelled;
                    failure.stage = AudioPlaybackStage::prebuffer;
                    failure.hresult = cancelledResult;
                    return false;
                }
                const auto receipt = candidate.fill(generation, stopToken);
                if (stopToken.stop_requested()) {
                    failure = previous;
                    failure.outcome = AudioPlaybackOutcome::cancelled;
                    failure.stage = AudioPlaybackStage::prebuffer;
                    failure.hresult = cancelledResult;
                    return false;
                }
                if (receipt.queuedFrames >= targetFrames) {
                    return true;
                }
                if (receipt.outcome == PresentationAudioOutcome::noOp
                    || receipt.outcome == PresentationAudioOutcome::refused) {
                    failure = previous;
                    failure.outcome = AudioPlaybackOutcome::failed;
                    failure.stage = AudioPlaybackStage::prebuffer;
                    failure.failure = AudioPlaybackFailureCode::invariantFailure;
                    failure.hresult = E_UNEXPECTED;
                    return false;
                }
            }
            return true;
        } catch (const MediaError& error) {
            failure = mediaFailureReceipt(
                error,
                AudioPlaybackStage::prebuffer,
                previous
            );
            return false;
        } catch (...) {
            failure = previous;
            failure.outcome = AudioPlaybackOutcome::failed;
            failure.stage = AudioPlaybackStage::prebuffer;
            failure.failure = AudioPlaybackFailureCode::invariantFailure;
            failure.hresult = E_UNEXPECTED;
            return false;
        }
    }

    AudioPlaybackReceipt executePlay(const Command& commandValue) {
        std::stop_callback commandCancellation(
            commandValue.cancellation,
            [this] {
                requestHandoffCancellation();
                condition_.notify_all();
            }
        );
        const auto previous = snapshot();
        if (commandValue.input.empty() || commandValue.timelineFrame < 0
            || (commandValue.decodeStart.has_value()
                && (commandValue.decodeStart->frameIndex < 0
                    || commandValue.decodeStart->frameRate.numerator <= 0
                    || commandValue.decodeStart->frameRate.denominator <= 0))) {
            auto value = previous;
            value.outcome = AudioPlaybackOutcome::refused;
            value.failure = AudioPlaybackFailureCode::invalidRequest;
            value.hresult = E_INVALIDARG;
            return value;
        }
        const auto requestedState = commandValue.startPaused
            ? AudioPlaybackState::paused
            : AudioPlaybackState::playing;
        if (previous.state == requestedState
            && commandValue.input == input_
            && commandValue.timelineFrame == timelineFrame_
            && sameStart(commandValue.decodeStart, decodeStart_)
            && (commandValue.expectedGeneration == 0
                || commandValue.expectedGeneration == generation_)) {
            auto value = previous;
            value.outcome = AudioPlaybackOutcome::noOp;
            return value;
        }

        const auto commandToken = handoffToken();
        const auto device = output_->configuration(commandToken);
        if (device.outcome == audio::WasapiWorkerConfigurationOutcome::cancelled) {
            auto value = previous;
            value.outcome = AudioPlaybackOutcome::cancelled;
            value.stage = AudioPlaybackStage::deviceConfiguration;
            value.hresult = cancelledResult;
            publish(value);
            return value;
        }
        if (device.outcome != audio::WasapiWorkerConfigurationOutcome::available) {
            auto value = previous;
            if (generation_ == 0) {
                value.state = device.outcome
                        == audio::WasapiWorkerConfigurationOutcome::unavailable
                    ? AudioPlaybackState::invalidated
                    : AudioPlaybackState::failed;
            }
            value.outcome = device.outcome
                    == audio::WasapiWorkerConfigurationOutcome::unavailable
                ? AudioPlaybackOutcome::invalidated
                : AudioPlaybackOutcome::failed;
            value.stage = AudioPlaybackStage::deviceConfiguration;
            value.failure = device.outcome
                    == audio::WasapiWorkerConfigurationOutcome::unavailable
                ? AudioPlaybackFailureCode::deviceUnavailable
                : AudioPlaybackFailureCode::deviceFailure;
            value.hresult = device.hresult;
            publish(value);
            return value;
        }
        configuration_ = device;
        const std::uint64_t nextGeneration = generation_ == 0
            ? device.generation
            : (generation_ == (std::numeric_limits<std::uint64_t>::max)()
                ? 0
                : generation_ + 1);
        if (nextGeneration == 0
            || (generation_ != 0 && device.generation != generation_)) {
            auto value = previous;
            value.outcome = AudioPlaybackOutcome::failed;
            value.stage = AudioPlaybackStage::installGeneration;
            value.failure = generation_ == (std::numeric_limits<std::uint64_t>::max)()
                ? AudioPlaybackFailureCode::generationExhausted
                : AudioPlaybackFailureCode::invariantFailure;
            value.hresult = E_UNEXPECTED;
            publish(value);
            return value;
        }
        if (commandValue.expectedGeneration != 0
            && commandValue.expectedGeneration != nextGeneration) {
            auto value = previous;
            value.outcome = AudioPlaybackOutcome::refused;
            value.stage = AudioPlaybackStage::installGeneration;
            value.failure = AudioPlaybackFailureCode::invalidRequest;
            value.hresult = E_INVALIDARG;
            return value;
        }

        const std::uint64_t capacity64 = static_cast<std::uint64_t>(
            device.bufferFrames
        ) * 2;
        if (device.bufferFrames == 0
            || device.clockFrequency == 0
            || !audio::isValidPcmFormat(device.pcmFormat)
            || capacity64 > maximumPumpFrames) {
            auto value = previous;
            value.outcome = AudioPlaybackOutcome::failed;
            value.stage = AudioPlaybackStage::deviceConfiguration;
            value.failure = AudioPlaybackFailureCode::invariantFailure;
            value.hresult = E_UNEXPECTED;
            publish(value);
            return value;
        }

        auto preparing = previous;
        preparing.state = AudioPlaybackState::preparing;
        preparing.outcome = AudioPlaybackOutcome::changed;
        preparing.stage = AudioPlaybackStage::openInput;
        preparing.failure = AudioPlaybackFailureCode::none;
        preparing.hresult = S_OK;
        publish(preparing);

        std::unique_ptr<PresentationAudioDecodePump> candidate;
        try {
            candidate = std::make_unique<PresentationAudioDecodePump>(
                PresentationAudioDecodeLimits{
                    device.pcmFormat,
                    static_cast<std::uint32_t>(capacity64),
                    std::min(device.bufferFrames, maximumFillFrames),
                    {},
                }
            );
            if (commandValue.decodeStart.has_value()) {
                candidate->start(
                    nextGeneration,
                    commandValue.input,
                    *commandValue.decodeStart,
                    commandToken
                );
            } else {
                candidate->start(
                    nextGeneration,
                    commandValue.input,
                    commandToken
                );
            }
        } catch (const MediaError& error) {
            const auto value = mediaFailureReceipt(
                error,
                AudioPlaybackStage::openInput,
                previous
            );
            publish(value);
            return value;
        } catch (...) {
            auto value = previous;
            value.outcome = AudioPlaybackOutcome::failed;
            value.stage = AudioPlaybackStage::openInput;
            value.failure = AudioPlaybackFailureCode::invariantFailure;
            value.hresult = E_UNEXPECTED;
            publish(value);
            return value;
        }

        AudioPlaybackReceipt preparationFailure;
        if (!prebuffer(
                *candidate,
                nextGeneration,
                std::min(device.bufferFrames, maximumFillFrames),
                commandToken,
                preparationFailure,
                previous
            )) {
            publish(preparationFailure);
            return preparationFailure;
        }
        if (commandToken.stop_requested()) {
            auto value = previous;
            value.outcome = AudioPlaybackOutcome::cancelled;
            value.stage = AudioPlaybackStage::prebuffer;
            value.hresult = cancelledResult;
            publish(value);
            return value;
        }

        std::optional<DecodedAudioBlock> candidateBlock;
        AudioPlaybackReceipt handoffFailure;
        if (!collectPendingBlock(
                *candidate,
                nextGeneration,
                std::min(device.bufferFrames, maximumFillFrames),
                commandToken,
                candidateBlock,
                handoffFailure
            )) {
            auto value = previous;
            value.outcome = handoffFailure.outcome;
            value.stage = handoffFailure.stage;
            value.failure = handoffFailure.failure;
            value.hresult = handoffFailure.hresult;
            publish(value);
            return value;
        }
        if (commandToken.stop_requested()) {
            auto value = previous;
            value.outcome = AudioPlaybackOutcome::cancelled;
            value.stage = AudioPlaybackStage::pcmHandoff;
            value.hresult = cancelledResult;
            publish(value);
            return value;
        }

        if (generation_ != 0) {
            const auto installed = output_->installGeneration(
                generation_,
                nextGeneration
            );
            if (installed.outcome != audio::WasapiOutputOutcome::changed) {
                const bool outputTerminal =
                    installed.currentState == audio::WasapiOutputState::failed
                    || installed.currentState
                        == audio::WasapiOutputState::invalidated
                    || installed.currentState == audio::WasapiOutputState::closed;
                auto value = previous;
                if (outputTerminal) {
                    value = outputFailureReceipt(
                        installed,
                        AudioPlaybackStage::installGeneration
                    );
                } else {
                    value.outcome = outputOutcome(installed.outcome);
                    value.stage = AudioPlaybackStage::installGeneration;
                    value.failure = AudioPlaybackFailureCode::outputFailure;
                    value.hresult = installed.hresult;
                }
                publish(value, outputTerminal);
                return value;
            }
        }

        pump_ = std::move(candidate);
        pendingBlock_ = std::move(candidateBlock);
        generation_ = nextGeneration;
        input_ = commandValue.input;
        timelineFrame_ = commandValue.timelineFrame;
        decodeStart_ = commandValue.decodeStart;
        sourceCursor_.reset();
        sourceAnchor_.reset();
        outputCursor_ = 0;
        eosSubmitted_ = false;
        clockAnchor_.reset();

        if (pendingBlock_.has_value()
            && !acceptBlock(*pendingBlock_, commandToken, handoffFailure)) {
            handoffFailure.generation = generation_;
            return terminateActiveFailure(handoffFailure);
        }
        pendingBlock_.reset();

        if (pump_->state() == PresentationAudioDecodeState::endOfStream) {
            const auto ended = output_->markEndOfStream(
                generation_,
                outputCursor_,
                commandToken
            );
            if (ended.outcome != audio::WasapiWorkerPcmOutcome::accepted
                && ended.outcome != audio::WasapiWorkerPcmOutcome::noOp) {
                auto value = pcmFailureReceipt(ended);
                return terminateActiveFailure(value);
            }
            eosSubmitted_ = true;
        }

        if (commandValue.startPaused) {
            if (!sourceAnchor_.has_value()) {
                auto value = failureReceipt(
                    AudioPlaybackStage::preparePaused,
                    AudioPlaybackFailureCode::invariantFailure,
                    E_UNEXPECTED
                );
                value.generation = generation_;
                return terminateActiveFailure(value);
            }
            auto value = previous;
            value.generation = generation_;
            value.state = AudioPlaybackState::paused;
            value.outcome = AudioPlaybackOutcome::changed;
            value.stage = AudioPlaybackStage::preparePaused;
            value.failure = AudioPlaybackFailureCode::none;
            value.hresult = S_OK;
            value.acceptedFrames = outputCursor_;
            value.hasClockAnchor = false;
            publish(value);
            return value;
        }

        const auto started = output_->start(generation_);
        if (started.currentState == audio::WasapiOutputState::completed) {
            auto value = previous;
            value.generation = generation_;
            if (started.generation != generation_) {
                value.state = AudioPlaybackState::failed;
                value.outcome = AudioPlaybackOutcome::failed;
                value.stage = AudioPlaybackStage::startDevice;
                value.failure = AudioPlaybackFailureCode::invariantFailure;
                value.hresult = E_UNEXPECTED;
                return terminateActiveFailure(value);
            }
            value.state = AudioPlaybackState::completed;
            value.outcome = AudioPlaybackOutcome::changed;
            value.stage = AudioPlaybackStage::drain;
            value.acceptedFrames = outputCursor_;
            publish(value, true);
            pump_.reset();
            return value;
        }
        if (started.outcome != audio::WasapiOutputOutcome::changed
            || !started.hasClockSample
            || started.generation != generation_
            || started.clockSample.generation != generation_) {
            auto value = outputFailureReceipt(
                started,
                AudioPlaybackStage::startDevice
            );
            if (started.outcome == audio::WasapiOutputOutcome::changed) {
                value.failure = AudioPlaybackFailureCode::invariantFailure;
                value.hresult = E_UNEXPECTED;
            }
            return terminateActiveFailure(value);
        }

        if (!sourceAnchor_.has_value()) {
            auto value = failureReceipt(
                AudioPlaybackStage::startDevice,
                AudioPlaybackFailureCode::invariantFailure,
                E_UNEXPECTED
            );
            value.generation = generation_;
            return terminateActiveFailure(value);
        }
        clockAnchor_ = AudioPlaybackClockAnchor{
            {
                generation_,
                started.clockSample.devicePosition,
                device.clockFrequency,
                timelineFrame_,
            },
            sourceAnchor_->presentationTimestamp,
            sourceAnchor_->timeBase.numerator,
            sourceAnchor_->timeBase.denominator,
            started.clockSample.qpc100Nanoseconds,
            started.clockSample.precisionDegraded,
        };
        auto value = previous;
        value.generation = generation_;
        value.state = AudioPlaybackState::playing;
        value.outcome = AudioPlaybackOutcome::changed;
        value.stage = AudioPlaybackStage::startDevice;
        value.failure = AudioPlaybackFailureCode::none;
        value.hresult = S_OK;
        value.acceptedFrames = outputCursor_;
        value.hasClockAnchor = true;
        value.clockAnchor = *clockAnchor_;
        publish(value);
        return value;
    }

    AudioPlaybackReceipt executeCancel(const Command& commandValue) {
        auto value = snapshot();
        if (generation_ == 0
            || commandValue.expectedGeneration != generation_) {
            value.outcome = AudioPlaybackOutcome::refused;
            value.failure = AudioPlaybackFailureCode::invalidRequest;
            value.hresult = E_INVALIDARG;
            return value;
        }
        if (value.state == AudioPlaybackState::completed
            || value.state == AudioPlaybackState::cancelled) {
            value.outcome = AudioPlaybackOutcome::noOp;
            return value;
        }
        if (pump_) {
            pump_->cancel(generation_);
        }
        pendingBlock_.reset();
        const auto discarded = output_->discardGeneration(generation_);
        if (discarded.outcome != audio::WasapiOutputOutcome::changed
            && discarded.outcome != audio::WasapiOutputOutcome::noOp) {
            value = outputFailureReceipt(
                discarded,
                AudioPlaybackStage::installGeneration
            );
            publish(value, true);
            return value;
        }
        pump_.reset();
        value.state = AudioPlaybackState::cancelled;
        value.outcome = AudioPlaybackOutcome::changed;
        value.stage = AudioPlaybackStage::drain;
        value.failure = AudioPlaybackFailureCode::none;
        value.hresult = S_OK;
        value.acceptedFrames = outputCursor_;
        publish(value, true);
        return value;
    }

    AudioPlaybackReceipt executePause(const Command& commandValue) {
        auto value = snapshot();
        if (generation_ == 0
            || commandValue.expectedGeneration != generation_) {
            value.outcome = AudioPlaybackOutcome::refused;
            value.failure = AudioPlaybackFailureCode::invalidRequest;
            value.hresult = E_INVALIDARG;
            return value;
        }
        if (value.state == AudioPlaybackState::paused) {
            value.outcome = AudioPlaybackOutcome::noOp;
            value.stage = AudioPlaybackStage::pauseDevice;
            return value;
        }
        if (value.state != AudioPlaybackState::playing) {
            value.outcome = AudioPlaybackOutcome::refused;
            value.stage = AudioPlaybackStage::pauseDevice;
            value.failure = AudioPlaybackFailureCode::invalidRequest;
            value.hresult = E_ILLEGAL_METHOD_CALL;
            return value;
        }
        const auto paused = output_->pause(generation_);
        if (paused.outcome != audio::WasapiOutputOutcome::changed
            && paused.outcome != audio::WasapiOutputOutcome::noOp) {
            return terminateActiveFailure(outputFailureReceipt(
                paused,
                AudioPlaybackStage::pauseDevice
            ));
        }
        value.state = AudioPlaybackState::paused;
        value.outcome = outputOutcome(paused.outcome);
        value.stage = AudioPlaybackStage::pauseDevice;
        value.failure = AudioPlaybackFailureCode::none;
        value.hresult = paused.hresult;
        value.acceptedFrames = outputCursor_;
        publish(value);
        return value;
    }

    AudioPlaybackReceipt executeResume(const Command& commandValue) {
        auto value = snapshot();
        if (generation_ == 0
            || commandValue.expectedGeneration != generation_) {
            value.outcome = AudioPlaybackOutcome::refused;
            value.failure = AudioPlaybackFailureCode::invalidRequest;
            value.hresult = E_INVALIDARG;
            return value;
        }
        if (value.state == AudioPlaybackState::playing) {
            value.outcome = AudioPlaybackOutcome::noOp;
            value.stage = AudioPlaybackStage::resumeDevice;
            return value;
        }
        if (value.state != AudioPlaybackState::paused) {
            value.outcome = AudioPlaybackOutcome::refused;
            value.stage = AudioPlaybackStage::resumeDevice;
            value.failure = AudioPlaybackFailureCode::invalidRequest;
            value.hresult = E_ILLEGAL_METHOD_CALL;
            return value;
        }
        const auto resumed = output_->start(generation_);
        if ((resumed.outcome != audio::WasapiOutputOutcome::changed
                && resumed.outcome != audio::WasapiOutputOutcome::noOp)
            || resumed.generation != generation_) {
            auto failure = outputFailureReceipt(
                resumed,
                AudioPlaybackStage::resumeDevice
            );
            if (resumed.generation != generation_) {
                failure.failure = AudioPlaybackFailureCode::invariantFailure;
                failure.hresult = E_UNEXPECTED;
            }
            return terminateActiveFailure(failure);
        }
        if (!clockAnchor_.has_value()) {
            if (!resumed.hasClockSample || !configuration_.has_value()
                || !sourceAnchor_.has_value()
                || resumed.clockSample.generation != generation_) {
                auto failure = outputFailureReceipt(
                    resumed,
                    AudioPlaybackStage::resumeDevice
                );
                failure.failure = AudioPlaybackFailureCode::invariantFailure;
                failure.hresult = E_UNEXPECTED;
                return terminateActiveFailure(failure);
            }
            clockAnchor_ = AudioPlaybackClockAnchor{
                {
                    generation_,
                    resumed.clockSample.devicePosition,
                    configuration_->clockFrequency,
                    timelineFrame_,
                },
                sourceAnchor_->presentationTimestamp,
                sourceAnchor_->timeBase.numerator,
                sourceAnchor_->timeBase.denominator,
                resumed.clockSample.qpc100Nanoseconds,
                resumed.clockSample.precisionDegraded,
            };
        }
        value.state = AudioPlaybackState::playing;
        value.outcome = outputOutcome(resumed.outcome);
        value.stage = AudioPlaybackStage::resumeDevice;
        value.failure = AudioPlaybackFailureCode::none;
        value.hresult = resumed.hresult;
        value.acceptedFrames = outputCursor_;
        value.hasClockAnchor = true;
        value.clockAnchor = *clockAnchor_;
        publish(value);
        return value;
    }

    AudioPlaybackReceipt executeClose() {
        const auto previous = snapshot();
        if (pump_) {
            pump_->cancel(generation_);
        }
        pendingBlock_.reset();
        pump_.reset();
        if (generation_ != 0
            && (previous.state == AudioPlaybackState::playing
                || previous.state == AudioPlaybackState::paused)) {
            const auto discarded = output_->discardGeneration(generation_);
            const bool discardedSuccessfully =
                discarded.outcome == audio::WasapiOutputOutcome::changed
                || discarded.outcome == audio::WasapiOutputOutcome::noOp;
            auto terminal = previous;
            terminal.state = discardedSuccessfully
                ? AudioPlaybackState::cancelled
                : outputState(discarded.outcome);
            terminal.outcome = discardedSuccessfully
                ? AudioPlaybackOutcome::cancelled
                : outputOutcome(discarded.outcome);
            terminal.stage = AudioPlaybackStage::drain;
            terminal.failure = discardedSuccessfully
                ? AudioPlaybackFailureCode::none
                : AudioPlaybackFailureCode::outputFailure;
            terminal.hresult = discarded.hresult;
            terminal.acceptedFrames = outputCursor_;
            publish(terminal, true);
        }
        const auto output = output_->close();
        auto value = snapshot();
        value.state = AudioPlaybackState::closed;
        value.outcome = outputOutcome(output.outcome);
        value.stage = AudioPlaybackStage::close;
        value.failure = output.outcome == audio::WasapiOutputOutcome::changed
            || output.outcome == audio::WasapiOutputOutcome::noOp
            ? AudioPlaybackFailureCode::none
            : AudioPlaybackFailureCode::outputFailure;
        value.hresult = output.hresult;
        publish(value);
        return value;
    }

    AudioPlaybackReceipt terminateActiveFailure(AudioPlaybackReceipt value) {
        if (generation_ != 0) {
            const auto discarded = output_->discardGeneration(generation_);
            if (discarded.outcome != audio::WasapiOutputOutcome::changed
                && discarded.outcome != audio::WasapiOutputOutcome::noOp) {
                const auto configuration = output_->configuration();
                if (configuration.outcome
                    == audio::WasapiWorkerConfigurationOutcome::unavailable) {
                    value.state = AudioPlaybackState::invalidated;
                    value.outcome = AudioPlaybackOutcome::invalidated;
                    value.hresult = configuration.hresult;
                } else if (configuration.outcome
                    == audio::WasapiWorkerConfigurationOutcome::failed) {
                    value.state = AudioPlaybackState::failed;
                    value.outcome = AudioPlaybackOutcome::failed;
                    value.hresult = configuration.hresult;
                } else {
                    value.state = outputState(discarded.outcome);
                    value.outcome = outputOutcome(discarded.outcome);
                    value.hresult = discarded.hresult;
                }
                value.failure = AudioPlaybackFailureCode::outputFailure;
            }
        }
        publish(value, true);
        pump_.reset();
        pendingBlock_.reset();
        return value;
    }

    void failActive(
        AudioPlaybackStage stage,
        AudioPlaybackFailureCode failure,
        HRESULT result,
        std::int32_t mediaCode = -1
    ) {
        auto value = snapshot();
        value.generation = generation_;
        value.state = AudioPlaybackState::failed;
        value.outcome = AudioPlaybackOutcome::failed;
        value.stage = stage;
        value.failure = failure;
        value.hresult = result;
        value.mediaFailureCode = mediaCode;
        value.acceptedFrames = outputCursor_;
        terminateActiveFailure(value);
    }

    void pumpActive() {
        if (!pump_ || generation_ == 0) {
            failActive(
                AudioPlaybackStage::pcmHandoff,
                AudioPlaybackFailureCode::invariantFailure,
                E_UNEXPECTED
            );
            return;
        }
        if (!pendingBlock_.has_value()) {
            const auto token = handoffToken();
            AudioPlaybackReceipt collectionFailure;
            if (!collectPendingBlock(
                    *pump_,
                    generation_,
                    std::min(configuration_->bufferFrames, maximumFillFrames),
                    token,
                    pendingBlock_,
                    collectionFailure
                )) {
                if (collectionFailure.outcome
                    != AudioPlaybackOutcome::cancelled) {
                    terminateActiveFailure(collectionFailure);
                }
                return;
            }
            if (!pendingBlock_.has_value()
                && pump_->state() != PresentationAudioDecodeState::endOfStream) {
                try {
                    const auto filled = pump_->fill(generation_);
                    if (filled.outcome == PresentationAudioOutcome::noOp
                        && pump_->state()
                            != PresentationAudioDecodeState::endOfStream) {
                        failActive(
                            AudioPlaybackStage::prebuffer,
                            AudioPlaybackFailureCode::invariantFailure,
                            E_UNEXPECTED
                        );
                        return;
                    }
                    if (!collectPendingBlock(
                            *pump_,
                            generation_,
                            std::min(
                                configuration_->bufferFrames,
                                maximumFillFrames
                            ),
                            token,
                            pendingBlock_,
                            collectionFailure
                        )) {
                        if (collectionFailure.outcome
                            != AudioPlaybackOutcome::cancelled) {
                            terminateActiveFailure(collectionFailure);
                        }
                        return;
                    }
                } catch (const MediaError& error) {
                    failActive(
                        AudioPlaybackStage::prebuffer,
                        AudioPlaybackFailureCode::mediaFailure,
                        E_FAIL,
                        static_cast<std::int32_t>(error.code)
                    );
                    return;
                } catch (...) {
                    failActive(
                        AudioPlaybackStage::prebuffer,
                        AudioPlaybackFailureCode::invariantFailure,
                        E_UNEXPECTED
                    );
                    return;
                }
            }
        }

        if (pendingBlock_.has_value()) {
            AudioPlaybackReceipt failure;
            if (acceptBlock(*pendingBlock_, handoffToken(), failure)) {
                pendingBlock_.reset();
                auto value = snapshot();
                value.acceptedFrames = outputCursor_;
                publish(value);
            } else if (failure.outcome != AudioPlaybackOutcome::cancelled) {
                terminateActiveFailure(failure);
            }
            return;
        }

        if (pump_->state() != PresentationAudioDecodeState::endOfStream) {
            return;
        }
        if (!eosSubmitted_) {
            const auto ended = output_->markEndOfStream(
                generation_,
                outputCursor_,
                handoffToken()
            );
            if (ended.outcome == audio::WasapiWorkerPcmOutcome::cancelled) {
                return;
            }
            if (ended.outcome != audio::WasapiWorkerPcmOutcome::accepted
                && ended.outcome != audio::WasapiWorkerPcmOutcome::noOp) {
                auto value = pcmFailureReceipt(ended);
                terminateActiveFailure(value);
                return;
            }
            eosSubmitted_ = true;
        }

        const auto output = output_->waitForTerminal(
            generation_,
            handoffToken()
        );
        if (output.outcome == audio::WasapiOutputOutcome::cancelled) {
            return;
        }
        if (output.currentState == audio::WasapiOutputState::completed) {
            auto value = snapshot();
            value.state = AudioPlaybackState::completed;
            value.outcome = AudioPlaybackOutcome::changed;
            value.stage = AudioPlaybackStage::drain;
            value.acceptedFrames = outputCursor_;
            publish(value, true);
            pump_.reset();
            return;
        }
        auto value = outputFailureReceipt(output, AudioPlaybackStage::drain);
        publish(value, true);
        pump_.reset();
    }

    void rejectPending() {
        std::deque<Command> commands;
        {
            std::lock_guard lock(mutex_);
            commands.swap(commands_);
        }
        for (const auto& commandValue : commands) {
            auto value = snapshot();
            value.outcome = AudioPlaybackOutcome::refused;
            value.hresult = E_ILLEGAL_METHOD_CALL;
            complete(commandValue.completion, value);
        }
    }

    void run(std::stop_token stopToken) noexcept {
        bool closing = false;
        while (!stopToken.stop_requested() && !closing) {
            std::optional<Command> commandValue;
            {
                std::unique_lock lock(mutex_);
                if (!commands_.empty()) {
                    commandValue = std::move(commands_.front());
                    commands_.pop_front();
                } else if (snapshot_.state != AudioPlaybackState::playing) {
                    condition_.wait(
                        lock,
                        stopToken,
                        [this] { return !commands_.empty(); }
                    );
                    continue;
                }
            }

            if (commandValue.has_value()) {
                beginCommandHandoffCancellation(commandValue->kind);
                AudioPlaybackReceipt value;
                const auto generationBeforeCommand = generation_;
                try {
                    switch (commandValue->kind) {
                    case CommandKind::play:
                        value = executePlay(*commandValue);
                        break;
                    case CommandKind::pause:
                        value = executePause(*commandValue);
                        break;
                    case CommandKind::resume:
                        value = executeResume(*commandValue);
                        break;
                    case CommandKind::cancel:
                        value = executeCancel(*commandValue);
                        break;
                    case CommandKind::close:
                        value = executeClose();
                        closing = true;
                        break;
                    }
                } catch (...) {
                    value = failureReceipt(
                        AudioPlaybackStage::none,
                        AudioPlaybackFailureCode::invariantFailure,
                        E_UNEXPECTED
                    );
                    value.state = AudioPlaybackState::failed;
                    publish(value, true);
                    if (commandValue->kind == CommandKind::close) {
                        closing = true;
                    }
                }
                if (commandValue->kind == CommandKind::play
                    && generation_ == generationBeforeCommand
                    && value.state == AudioPlaybackState::playing) {
                    beginCommandHandoffCancellation(CommandKind::play);
                }
                complete(commandValue->completion, value);
                continue;
            }

            try {
                pumpActive();
            } catch (...) {
                failActive(
                    AudioPlaybackStage::none,
                    AudioPlaybackFailureCode::invariantFailure,
                    E_UNEXPECTED
                );
            }
        }
        rejectPending();
        {
            std::lock_guard lock(mutex_);
            exited_ = true;
        }
        condition_.notify_all();
    }

    std::unique_ptr<audio::WasapiOutputWorker> output_;
    mutable std::mutex mutex_;
    std::condition_variable_any condition_;
    std::deque<Command> commands_;
    AudioPlaybackReceipt snapshot_;
    std::deque<AudioPlaybackReceipt> terminalHistory_;
    std::optional<AudioPlaybackReceipt> closeReceipt_;
    std::stop_source handoffCancellation_;
    bool closeStarted_{};
    bool closeFinished_{};
    bool exited_{};
    std::jthread worker_;

    std::optional<audio::WasapiWorkerConfiguration> configuration_;
    std::unique_ptr<PresentationAudioDecodePump> pump_;
    std::optional<DecodedAudioBlock> pendingBlock_;
    std::filesystem::path input_;
    std::uint64_t generation_{};
    std::int64_t timelineFrame_{};
    std::optional<DecodeFrameStart> decodeStart_;
    std::optional<std::int64_t> sourceCursor_;
    std::optional<PlaybackSourceAnchor> sourceAnchor_;
    std::uint64_t outputCursor_{};
    bool eosSubmitted_{};
    std::optional<AudioPlaybackClockAnchor> clockAnchor_;
};

AudioPlaybackSession::AudioPlaybackSession()
    : AudioPlaybackSession(std::make_unique<audio::WasapiOutputWorker>()) {}

AudioPlaybackSession::AudioPlaybackSession(
    std::unique_ptr<audio::WasapiOutputWorker> outputWorker
) : impl_(std::make_unique<Impl>(std::move(outputWorker))) {}

AudioPlaybackSession::~AudioPlaybackSession() = default;

AudioPlaybackReceipt AudioPlaybackSession::play(
    const std::filesystem::path& input,
    std::int64_t timelineFrame
) {
    return impl_->play(input, timelineFrame);
}

AudioPlaybackReceipt AudioPlaybackSession::play(
    const std::filesystem::path& input,
    std::int64_t timelineFrame,
    DecodeFrameStart start
) {
    return impl_->play(input, timelineFrame, start);
}

AudioPlaybackReceipt AudioPlaybackSession::playExactGeneration(
    const std::filesystem::path& input,
    std::int64_t timelineFrame,
    std::uint64_t generation,
    std::stop_token cancellation
) {
    return impl_->playExactGeneration(
        input,
        timelineFrame,
        generation,
        cancellation
    );
}

AudioPlaybackReceipt AudioPlaybackSession::preparePausedExactGeneration(
    const std::filesystem::path& input,
    std::int64_t timelineFrame,
    DecodeFrameStart start,
    std::uint64_t generation,
    std::stop_token cancellation
) {
    return impl_->preparePausedExactGeneration(
        input,
        timelineFrame,
        start,
        generation,
        cancellation
    );
}

AudioPlaybackReceipt AudioPlaybackSession::playExactGeneration(
    const std::filesystem::path& input,
    std::int64_t timelineFrame,
    DecodeFrameStart start,
    std::uint64_t generation,
    std::stop_token cancellation
) {
    return impl_->playExactGeneration(
        input,
        timelineFrame,
        start,
        generation,
        cancellation
    );
}

AudioPlaybackReceipt AudioPlaybackSession::cancel(
    std::uint64_t expectedGeneration
) {
    return impl_->cancel(expectedGeneration);
}

AudioPlaybackReceipt AudioPlaybackSession::pause(
    std::uint64_t expectedGeneration
) {
    return impl_->pause(expectedGeneration);
}

AudioPlaybackReceipt AudioPlaybackSession::resume(
    std::uint64_t expectedGeneration
) {
    return impl_->resume(expectedGeneration);
}

AudioPlaybackReceipt AudioPlaybackSession::waitForTerminal(
    std::uint64_t generation,
    std::stop_token stopToken
) {
    return impl_->waitForTerminal(generation, stopToken);
}

AudioPlaybackPositionReceipt AudioPlaybackSession::position(
    std::uint64_t expectedGeneration
) const {
    return impl_->position(expectedGeneration);
}

AudioPlaybackReceipt AudioPlaybackSession::snapshot() const {
    return impl_->snapshot();
}

AudioPlaybackReceipt AudioPlaybackSession::close() {
    return impl_->close();
}

}
