#include "palmier/media/headless_av_playback_session.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace palmier::media {
namespace {

constexpr std::size_t maximumConfigurableFillCallsPerTick = 4;
constexpr HRESULT cancelledResult = HRESULT_FROM_WIN32(ERROR_CANCELLED);

bool isTerminal(HeadlessAvPlaybackState state) noexcept {
    return state == HeadlessAvPlaybackState::completed
        || state == HeadlessAvPlaybackState::cancelled
        || state == HeadlessAvPlaybackState::invalidated
        || state == HeadlessAvPlaybackState::failed
        || state == HeadlessAvPlaybackState::closed;
}

HeadlessAvPlaybackOutcome terminalOutcome(
    HeadlessAvPlaybackState state
) noexcept {
    switch (state) {
    case HeadlessAvPlaybackState::cancelled:
        return HeadlessAvPlaybackOutcome::cancelled;
    case HeadlessAvPlaybackState::invalidated:
        return HeadlessAvPlaybackOutcome::invalidated;
    case HeadlessAvPlaybackState::failed:
        return HeadlessAvPlaybackOutcome::failed;
    case HeadlessAvPlaybackState::idle:
    case HeadlessAvPlaybackState::playing:
    case HeadlessAvPlaybackState::paused:
    case HeadlessAvPlaybackState::completed:
    case HeadlessAvPlaybackState::closed:
        return HeadlessAvPlaybackOutcome::noOp;
    }
    return HeadlessAvPlaybackOutcome::failed;
}

class ProductionAudioPort final
    : public detail::HeadlessAvPlaybackAudioPort {
public:
    AudioPlaybackReceipt playExactGeneration(
        const std::filesystem::path& input,
        std::int64_t timelineFrame,
        std::optional<DecodeFrameStart> decodeStart,
        std::uint64_t generation,
        std::stop_token cancellation
    ) override {
        return decodeStart.has_value()
            ? session_.playExactGeneration(
                input,
                timelineFrame,
                *decodeStart,
                generation,
                cancellation
            )
            : session_.playExactGeneration(
                input,
                timelineFrame,
                generation,
                cancellation
            );
    }

    AudioPlaybackPositionReceipt position(
        std::uint64_t generation
    ) const override {
        return session_.position(generation);
    }

    AudioPlaybackReceipt cancel(std::uint64_t generation) override {
        return session_.cancel(generation);
    }

    AudioPlaybackReceipt pause(std::uint64_t generation) override {
        return session_.pause(generation);
    }

    AudioPlaybackReceipt resume(std::uint64_t generation) override {
        return session_.resume(generation);
    }

    AudioPlaybackReceipt snapshot() const override {
        return session_.snapshot();
    }

    AudioPlaybackReceipt close() override { return session_.close(); }

private:
    AudioPlaybackSession session_;
};

HeadlessAvPlaybackState stateForAudio(AudioPlaybackState state) noexcept {
    switch (state) {
    case AudioPlaybackState::completed:
        return HeadlessAvPlaybackState::completed;
    case AudioPlaybackState::cancelled:
        return HeadlessAvPlaybackState::cancelled;
    case AudioPlaybackState::invalidated:
        return HeadlessAvPlaybackState::invalidated;
    case AudioPlaybackState::failed:
        return HeadlessAvPlaybackState::failed;
    case AudioPlaybackState::closed:
        return HeadlessAvPlaybackState::closed;
    case AudioPlaybackState::idle:
    case AudioPlaybackState::preparing:
    case AudioPlaybackState::playing:
        return HeadlessAvPlaybackState::playing;
    case AudioPlaybackState::paused:
        return HeadlessAvPlaybackState::paused;
    }
    return HeadlessAvPlaybackState::failed;
}

HeadlessAvPlaybackOutcome outcomeForAudio(AudioPlaybackOutcome outcome) noexcept {
    switch (outcome) {
    case AudioPlaybackOutcome::changed:
        return HeadlessAvPlaybackOutcome::changed;
    case AudioPlaybackOutcome::noOp:
        return HeadlessAvPlaybackOutcome::noOp;
    case AudioPlaybackOutcome::cancelled:
        return HeadlessAvPlaybackOutcome::cancelled;
    case AudioPlaybackOutcome::refused:
        return HeadlessAvPlaybackOutcome::refused;
    case AudioPlaybackOutcome::failed:
        return HeadlessAvPlaybackOutcome::failed;
    case AudioPlaybackOutcome::invalidated:
        return HeadlessAvPlaybackOutcome::invalidated;
    }
    return HeadlessAvPlaybackOutcome::failed;
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

class ActiveOperationScope final {
public:
    ActiveOperationScope(
        std::mutex& mutex,
        std::shared_ptr<detail::HeadlessAvPlaybackActiveOperation>& slot,
        bool& closeRequested,
        const std::optional<std::uint64_t>& pendingCancellation,
        std::uint64_t admittedGeneration
    ) : mutex_(mutex),
        slot_(slot),
        operation_(
            std::make_shared<detail::HeadlessAvPlaybackActiveOperation>()
        ) {
        std::lock_guard lock(mutex_);
        operation_->admittedGeneration = admittedGeneration;
        slot_ = operation_;
        if (closeRequested || pendingCancellation.has_value()) {
            operation_->cancellation.request_stop();
        }
    }

    ~ActiveOperationScope() {
        std::lock_guard lock(mutex_);
        if (slot_ == operation_) {
            slot_.reset();
        }
    }

    std::shared_ptr<detail::HeadlessAvPlaybackActiveOperation> operation() const {
        return operation_;
    }

    std::stop_token token() const noexcept {
        return operation_->cancellation.get_token();
    }

private:
    std::mutex& mutex_;
    std::shared_ptr<detail::HeadlessAvPlaybackActiveOperation>& slot_;
    std::shared_ptr<detail::HeadlessAvPlaybackActiveOperation> operation_;
};

}

HeadlessAvPlaybackSession::HeadlessAvPlaybackSession(
    HeadlessAvPlaybackLimits limits
) : HeadlessAvPlaybackSession(
        std::make_unique<ProductionAudioPort>(),
        limits
    ) {}

HeadlessAvPlaybackSession::HeadlessAvPlaybackSession(
    std::unique_ptr<detail::HeadlessAvPlaybackAudioPort> audio,
    HeadlessAvPlaybackLimits limits
) : audio_(std::move(audio)), limits_(limits) {
    if (audio_ == nullptr) {
        throw std::invalid_argument("missing headless A/V audio port");
    }
    if (limits_.maximumVideoFillCallsPerTick == 0
        || limits_.maximumVideoFillCallsPerTick
            > maximumConfigurableFillCallsPerTick) {
        throw std::invalid_argument(
            "video fill calls per tick must be between one and four"
        );
    }
}

HeadlessAvPlaybackSession::~HeadlessAvPlaybackSession() {
    try {
        close();
    } catch (...) {
    }
}

HeadlessAvPlaybackReceipt HeadlessAvPlaybackSession::play(
    const std::filesystem::path& input,
    std::int64_t timelineFrame,
    audio::FrameRate timelineFrameRate,
    std::stop_token cancellation
) {
    return playInternal(input, timelineFrame, timelineFrameRate, {}, cancellation);
}

HeadlessAvPlaybackReceipt HeadlessAvPlaybackSession::play(
    const std::filesystem::path& input,
    std::int64_t timelineFrame,
    audio::FrameRate timelineFrameRate,
    DecodeFrameStart start,
    std::stop_token cancellation
) {
    return playInternal(input, timelineFrame, timelineFrameRate, start, cancellation);
}

HeadlessAvPlaybackReceipt HeadlessAvPlaybackSession::playInternal(
    const std::filesystem::path& input,
    std::int64_t timelineFrame,
    audio::FrameRate timelineFrameRate,
    std::optional<DecodeFrameStart> decodeStart,
    std::stop_token cancellation
) {
    std::lock_guard lock(mutex_);
    bool closeRequested;
    {
        std::lock_guard operationLock(operationMutex_);
        closeRequested = closeRequested_;
    }
    if (closeRequested || state_ == HeadlessAvPlaybackState::closed) {
        auto value = baseReceipt(
            HeadlessAvPlaybackOutcome::refused,
            HeadlessAvPlaybackStage::prepareVideo
        );
        value.failure = HeadlessAvPlaybackFailureCode::invalidRequest;
        value.hresult = E_ILLEGAL_METHOD_CALL;
        return value;
    }
    if (input.empty() || timelineFrame < 0 || timelineFrameRate.numerator == 0
        || timelineFrameRate.denominator == 0
        || (decodeStart.has_value()
            && (decodeStart->frameIndex < 0
                || decodeStart->frameRate.numerator <= 0
                || decodeStart->frameRate.denominator <= 0))) {
        auto value = baseReceipt(
            HeadlessAvPlaybackOutcome::refused,
            HeadlessAvPlaybackStage::prepareVideo
        );
        value.failure = HeadlessAvPlaybackFailureCode::invalidRequest;
        value.hresult = E_INVALIDARG;
        return value;
    }
    if (state_ == HeadlessAvPlaybackState::playing && input == input_
        && timelineFrame == timelineFrame_
        && timelineFrameRate.numerator == timelineFrameRate_.numerator
        && timelineFrameRate.denominator == timelineFrameRate_.denominator
        && sameStart(decodeStart, decodeStart_)) {
        return baseReceipt(HeadlessAvPlaybackOutcome::noOp);
    }
    if (generation_ == (std::numeric_limits<std::uint64_t>::max)()) {
        auto value = baseReceipt(
            HeadlessAvPlaybackOutcome::refused,
            HeadlessAvPlaybackStage::startAudio
        );
        value.failure = HeadlessAvPlaybackFailureCode::generationExhausted;
        value.hresult = E_UNEXPECTED;
        return value;
    }
    const auto nextGeneration = generation_ + 1;
    ActiveOperationScope activeOperation(
        operationMutex_,
        activeOperation_,
        closeRequested_,
        pendingCancellation_,
        generation_
    );
    const auto operation = activeOperation.operation();
    std::stop_callback externalCancellation(
        cancellation,
        [operation] { operation->cancellation.request_stop(); }
    );
    const auto operationToken = activeOperation.token();

    auto candidate = std::make_unique<PresentationVideoDecodePump>(limits_.video);
    PresentationVideoFillReceipt prefill;
    try {
        if (decodeStart.has_value()) {
            candidate->start(nextGeneration, input, *decodeStart, operationToken);
        } else {
            candidate->start(nextGeneration, input, operationToken);
        }
        prefill = candidate->fill(nextGeneration, operationToken);
    } catch (const MediaError& error) {
        auto value = baseReceipt(
            error.code == MediaFailureCode::cancelled
                ? HeadlessAvPlaybackOutcome::cancelled
                : HeadlessAvPlaybackOutcome::refused,
            HeadlessAvPlaybackStage::prepareVideo
        );
        value.failure = error.code == MediaFailureCode::cancelled
            ? HeadlessAvPlaybackFailureCode::none
            : HeadlessAvPlaybackFailureCode::videoFailure;
        value.hresult = error.code == MediaFailureCode::cancelled
            ? cancelledResult
            : E_FAIL;
        value.mediaFailureCode = static_cast<std::int32_t>(error.code);
        return value;
    } catch (...) {
        auto value = baseReceipt(
            HeadlessAvPlaybackOutcome::failed,
            HeadlessAvPlaybackStage::prepareVideo
        );
        value.failure = HeadlessAvPlaybackFailureCode::videoFailure;
        value.hresult = E_UNEXPECTED;
        return value;
    }
    if (operationToken.stop_requested()) {
        auto value = baseReceipt(
            HeadlessAvPlaybackOutcome::cancelled,
            HeadlessAvPlaybackStage::prepareVideo
        );
        value.hresult = cancelledResult;
        return value;
    }

    AudioPlaybackReceipt audioStart;
    try {
        audioStart = audio_->playExactGeneration(
            input,
            timelineFrame,
            decodeStart,
            nextGeneration,
            operationToken
        );
    } catch (...) {
        auto value = baseReceipt(
            HeadlessAvPlaybackOutcome::failed,
            HeadlessAvPlaybackStage::startAudio
        );
        value.failure = HeadlessAvPlaybackFailureCode::audioFailure;
        value.audioFailure = AudioPlaybackFailureCode::invariantFailure;
        value.hresult = E_UNEXPECTED;
        return value;
    }
    if (operationToken.stop_requested()
        && audioStart.generation == nextGeneration
        && audioStart.outcome == AudioPlaybackOutcome::changed) {
        try {
            audioStart = audio_->cancel(nextGeneration);
        } catch (...) {
            audioStart.generation = nextGeneration;
            audioStart.state = AudioPlaybackState::failed;
            audioStart.outcome = AudioPlaybackOutcome::failed;
            audioStart.failure = AudioPlaybackFailureCode::invariantFailure;
            audioStart.hresult = E_UNEXPECTED;
        }
    }
    if (audioStart.outcome != AudioPlaybackOutcome::changed
        || audioStart.generation != nextGeneration
        || (audioStart.state != AudioPlaybackState::playing
            && audioStart.state != AudioPlaybackState::completed)) {
        if (audioStart.generation == nextGeneration) {
            if (video_ != nullptr && generation_ != 0) {
                video_->cancel(generation_);
            }
            video_ = std::move(candidate);
            input_ = input;
            generation_ = nextGeneration;
            {
                std::lock_guard operationLock(operationMutex_);
                publishedGeneration_ = generation_;
            }
            timelineFrame_ = timelineFrame;
            timelineFrameRate_ = timelineFrameRate;
            decodeStart_ = decodeStart;
            video_->cancel(generation_);
            state_ = stateForAudio(audioStart.state);
            if (state_ == HeadlessAvPlaybackState::playing) {
                switch (audioStart.outcome) {
                case AudioPlaybackOutcome::cancelled:
                    state_ = HeadlessAvPlaybackState::cancelled;
                    break;
                case AudioPlaybackOutcome::invalidated:
                    state_ = HeadlessAvPlaybackState::invalidated;
                    break;
                case AudioPlaybackOutcome::changed:
                case AudioPlaybackOutcome::noOp:
                case AudioPlaybackOutcome::refused:
                case AudioPlaybackOutcome::failed:
                    state_ = HeadlessAvPlaybackState::failed;
                    break;
                }
            }
            auto value = baseReceipt(
                terminalOutcome(state_),
                HeadlessAvPlaybackStage::startAudio
            );
            value.audioState = audioStart.state;
            value.hresult = audioStart.hresult;
            value.mediaFailureCode = audioStart.mediaFailureCode;
            value.audioFailure = audioStart.failure;
            value.failure = state_ == HeadlessAvPlaybackState::cancelled
                ? HeadlessAvPlaybackFailureCode::none
                : HeadlessAvPlaybackFailureCode::audioFailure;
            value.fillCalls = 1;
            value.admittedFrames = prefill.admittedFrames;
            return value;
        }
        auto value = baseReceipt(
            operationToken.stop_requested()
                ? HeadlessAvPlaybackOutcome::cancelled
                : outcomeForAudio(audioStart.outcome),
            HeadlessAvPlaybackStage::startAudio
        );
        value.audioState = audioStart.state;
        value.audioFailure = audioStart.failure;
        value.hresult = operationToken.stop_requested()
            ? cancelledResult
            : audioStart.hresult;
        value.failure = operationToken.stop_requested()
            || audioStart.outcome == AudioPlaybackOutcome::cancelled
            ? HeadlessAvPlaybackFailureCode::none
            : HeadlessAvPlaybackFailureCode::audioFailure;
        if (audioStart.outcome == AudioPlaybackOutcome::changed
            || audioStart.outcome == AudioPlaybackOutcome::noOp) {
            value.outcome = HeadlessAvPlaybackOutcome::failed;
            value.failure = HeadlessAvPlaybackFailureCode::invariantFailure;
            value.hresult = E_UNEXPECTED;
            if (audioStart.outcome == AudioPlaybackOutcome::changed) {
                try {
                    audio_->cancel(nextGeneration);
                } catch (...) {
                }
            }
        }
        return value;
    }

    video_ = std::move(candidate);
    input_ = input;
    generation_ = nextGeneration;
    {
        std::lock_guard operationLock(operationMutex_);
        publishedGeneration_ = generation_;
    }
    timelineFrame_ = timelineFrame;
    timelineFrameRate_ = timelineFrameRate;
    decodeStart_ = decodeStart;
    state_ = stateForAudio(audioStart.state);
    if (state_ == HeadlessAvPlaybackState::completed) {
        video_->cancel(generation_);
    }
    auto value = baseReceipt(
        HeadlessAvPlaybackOutcome::changed,
        HeadlessAvPlaybackStage::commitVideo
    );
    value.audioState = audioStart.state;
    value.audioFailure = audioStart.failure;
    value.videoState = video_->state();
    value.fillCalls = 1;
    value.admittedFrames = prefill.admittedFrames;
    return value;
}

HeadlessAvPlaybackReceipt HeadlessAvPlaybackSession::tick(
    std::uint64_t expectedGeneration,
    std::stop_token cancellation
) {
    std::lock_guard lock(mutex_);
    bool closeRequested;
    {
        std::lock_guard operationLock(operationMutex_);
        closeRequested = closeRequested_;
    }
    if (closeRequested) {
        auto value = baseReceipt(
            HeadlessAvPlaybackOutcome::refused,
            HeadlessAvPlaybackStage::audioPosition
        );
        value.failure = HeadlessAvPlaybackFailureCode::invalidRequest;
        value.hresult = E_ILLEGAL_METHOD_CALL;
        return value;
    }
    if (expectedGeneration == 0 || expectedGeneration != generation_) {
        auto value = baseReceipt(
            HeadlessAvPlaybackOutcome::stale,
            HeadlessAvPlaybackStage::audioPosition
        );
        value.failure = HeadlessAvPlaybackFailureCode::invalidRequest;
        value.hresult = E_INVALIDARG;
        return value;
    }
    if (isTerminal(state_)) {
        return baseReceipt(terminalOutcome(state_));
    }
    if (state_ == HeadlessAvPlaybackState::paused) {
        return baseReceipt(
            HeadlessAvPlaybackOutcome::noOp,
            HeadlessAvPlaybackStage::audioPosition
        );
    }
    if (video_ == nullptr) {
        return failActive(
            HeadlessAvPlaybackStage::selectVideo,
            HeadlessAvPlaybackFailureCode::invariantFailure,
            E_UNEXPECTED
        );
    }
    ActiveOperationScope activeOperation(
        operationMutex_,
        activeOperation_,
        closeRequested_,
        pendingCancellation_,
        generation_
    );
    const auto operation = activeOperation.operation();
    std::stop_callback externalCancellation(
        cancellation,
        [operation] { operation->cancellation.request_stop(); }
    );
    const auto operationToken = activeOperation.token();
    if (operationToken.stop_requested()) {
        return cancelCurrent();
    }

    AudioPlaybackPositionReceipt position;
    try {
        position = audio_->position(expectedGeneration);
    } catch (...) {
        return failActive(
            HeadlessAvPlaybackStage::audioPosition,
            HeadlessAvPlaybackFailureCode::audioFailure,
            E_UNEXPECTED
        );
    }
    if (position.generation != generation_) {
        return failActive(
            HeadlessAvPlaybackStage::audioPosition,
            HeadlessAvPlaybackFailureCode::invariantFailure,
            E_UNEXPECTED
        );
    }
    if (position.outcome == AudioPlaybackOutcome::refused) {
        return failActive(
            HeadlessAvPlaybackStage::audioPosition,
            HeadlessAvPlaybackFailureCode::invariantFailure,
            position.hresult
        );
    }
    if (position.outcome == AudioPlaybackOutcome::cancelled
        || position.outcome == AudioPlaybackOutcome::invalidated
        || position.outcome == AudioPlaybackOutcome::failed
    ) {
        return terminateFromAudioPosition(
            position,
            HeadlessAvPlaybackStage::audioPosition
        );
    }
    if (!position.hasClockAnchor || !position.hasClockSample) {
        if (position.state == AudioPlaybackState::completed) {
            return terminateFromAudioPosition(
                position,
                HeadlessAvPlaybackStage::audioPosition
            );
        }
        auto value = baseReceipt(
            HeadlessAvPlaybackOutcome::noOp,
            HeadlessAvPlaybackStage::audioPosition
        );
        value.audioState = position.state;
        value.audioFailure = position.failure;
        value.hresult = position.hresult;
        return value;
    }

    const PresentationVideoClockPosition clock{
        position.clockAnchor.value,
        position.clockSample,
        timelineFrameRate_,
        position.clockAnchor.sourcePresentationTimestamp,
        {
            position.clockAnchor.sourceTimeBaseNumerator,
            position.clockAnchor.sourceTimeBaseDenominator,
        },
    };
    HeadlessAvPlaybackReceipt value = baseReceipt(
        HeadlessAvPlaybackOutcome::noOp,
        HeadlessAvPlaybackStage::selectVideo
    );
    value.audioState = position.state;
    value.audioFailure = position.failure;
    try {
        for (;;) {
            if (operationToken.stop_requested()) {
                return cancelCurrent();
            }
            auto selection = video_->select(
                generation_,
                video_->revision(),
                clock
            );
            value.videoState = video_->state();
            value.hasTargetTimelineFrame = selection.hasTargetTimelineFrame;
            value.targetTimelineFrame = selection.targetTimelineFrame;
            value.droppedFrames += selection.droppedFrames;
            if (selection.receipt.outcome == PresentationVideoOutcome::stale
                || selection.receipt.outcome
                    == PresentationVideoOutcome::cancelled) {
                return failActive(
                    HeadlessAvPlaybackStage::selectVideo,
                    HeadlessAvPlaybackFailureCode::invariantFailure,
                    E_UNEXPECTED
                );
            }
            if (selection.frame.has_value()) {
                if (value.frame.has_value()) {
                    ++value.droppedFrames;
                }
                value.frame = std::move(selection.frame);
                value.outcome = HeadlessAvPlaybackOutcome::changed;
            }
            if (selection.receipt.reason == PresentationVideoReason::frameEarly
                || video_->state()
                    == PresentationVideoDecodeState::endOfStream) {
                break;
            }
            if (value.fillCalls >= limits_.maximumVideoFillCallsPerTick) {
                value.fillBudgetExhausted = true;
                break;
            }
            const auto fill = video_->fill(generation_, operationToken);
            ++value.fillCalls;
            value.admittedFrames += fill.admittedFrames;
            value.videoState = fill.state;
            value.stage = HeadlessAvPlaybackStage::fillVideo;
            if (fill.outcome == PresentationVideoOutcome::stale
                || fill.outcome == PresentationVideoOutcome::cancelled) {
                return failActive(
                    HeadlessAvPlaybackStage::fillVideo,
                    HeadlessAvPlaybackFailureCode::invariantFailure,
                    E_UNEXPECTED
                );
            }
            if (fill.admittedFrames == 0
                && fill.state != PresentationVideoDecodeState::endOfStream) {
                break;
            }
        }
    } catch (const MediaError& error) {
        return failActive(
            HeadlessAvPlaybackStage::fillVideo,
            error.code == MediaFailureCode::cancelled
                ? HeadlessAvPlaybackFailureCode::none
                : HeadlessAvPlaybackFailureCode::videoFailure,
            error.code == MediaFailureCode::cancelled
                ? cancelledResult
                : E_FAIL,
            static_cast<std::int32_t>(error.code)
        );
    } catch (...) {
        return failActive(
            HeadlessAvPlaybackStage::selectVideo,
            HeadlessAvPlaybackFailureCode::videoFailure,
            E_UNEXPECTED
        );
    }
    value.stage = HeadlessAvPlaybackStage::selectVideo;
    if (position.state == AudioPlaybackState::completed) {
        video_->cancel(generation_);
        state_ = HeadlessAvPlaybackState::completed;
        value.state = state_;
    }
    return value;
}

HeadlessAvPlaybackReceipt HeadlessAvPlaybackSession::pause(
    std::uint64_t expectedGeneration
) {
    std::lock_guard lifecycleLock(lifecycleMutex_);
    std::lock_guard lock(mutex_);
    bool closeRequested;
    {
        std::lock_guard operationLock(operationMutex_);
        closeRequested = closeRequested_;
    }
    if (closeRequested) {
        auto value = baseReceipt(
            HeadlessAvPlaybackOutcome::refused,
            HeadlessAvPlaybackStage::pauseAudio
        );
        value.failure = HeadlessAvPlaybackFailureCode::invalidRequest;
        value.hresult = E_ILLEGAL_METHOD_CALL;
        return value;
    }
    if (expectedGeneration == 0 || expectedGeneration != generation_) {
        auto value = baseReceipt(
            HeadlessAvPlaybackOutcome::stale,
            HeadlessAvPlaybackStage::pauseAudio
        );
        value.failure = HeadlessAvPlaybackFailureCode::invalidRequest;
        value.hresult = E_INVALIDARG;
        return value;
    }
    if (state_ == HeadlessAvPlaybackState::paused) {
        return baseReceipt(
            HeadlessAvPlaybackOutcome::noOp,
            HeadlessAvPlaybackStage::pauseAudio
        );
    }
    if (state_ != HeadlessAvPlaybackState::playing) {
        auto value = baseReceipt(
            HeadlessAvPlaybackOutcome::refused,
            HeadlessAvPlaybackStage::pauseAudio
        );
        value.failure = HeadlessAvPlaybackFailureCode::invalidRequest;
        value.hresult = E_ILLEGAL_METHOD_CALL;
        return value;
    }
    try {
        const auto audio = audio_->pause(generation_);
        if (audio.generation != generation_
            || (audio.outcome != AudioPlaybackOutcome::changed
                && audio.outcome != AudioPlaybackOutcome::noOp)
            || audio.state != AudioPlaybackState::paused) {
            return failActive(
                HeadlessAvPlaybackStage::pauseAudio,
                HeadlessAvPlaybackFailureCode::audioFailure,
                audio.generation == generation_ ? audio.hresult : E_UNEXPECTED
            );
        }
        state_ = HeadlessAvPlaybackState::paused;
        auto value = baseReceipt(
            outcomeForAudio(audio.outcome),
            HeadlessAvPlaybackStage::pauseAudio
        );
        value.audioState = audio.state;
        value.audioFailure = audio.failure;
        value.hresult = audio.hresult;
        return value;
    } catch (...) {
        return failActive(
            HeadlessAvPlaybackStage::pauseAudio,
            HeadlessAvPlaybackFailureCode::audioFailure,
            E_UNEXPECTED
        );
    }
}

HeadlessAvPlaybackReceipt HeadlessAvPlaybackSession::resume(
    std::uint64_t expectedGeneration
) {
    std::lock_guard lifecycleLock(lifecycleMutex_);
    std::lock_guard lock(mutex_);
    bool closeRequested;
    {
        std::lock_guard operationLock(operationMutex_);
        closeRequested = closeRequested_;
    }
    if (closeRequested) {
        auto value = baseReceipt(
            HeadlessAvPlaybackOutcome::refused,
            HeadlessAvPlaybackStage::resumeAudio
        );
        value.failure = HeadlessAvPlaybackFailureCode::invalidRequest;
        value.hresult = E_ILLEGAL_METHOD_CALL;
        return value;
    }
    if (expectedGeneration == 0 || expectedGeneration != generation_) {
        auto value = baseReceipt(
            HeadlessAvPlaybackOutcome::stale,
            HeadlessAvPlaybackStage::resumeAudio
        );
        value.failure = HeadlessAvPlaybackFailureCode::invalidRequest;
        value.hresult = E_INVALIDARG;
        return value;
    }
    if (state_ == HeadlessAvPlaybackState::playing) {
        return baseReceipt(
            HeadlessAvPlaybackOutcome::noOp,
            HeadlessAvPlaybackStage::resumeAudio
        );
    }
    if (state_ != HeadlessAvPlaybackState::paused) {
        auto value = baseReceipt(
            HeadlessAvPlaybackOutcome::refused,
            HeadlessAvPlaybackStage::resumeAudio
        );
        value.failure = HeadlessAvPlaybackFailureCode::invalidRequest;
        value.hresult = E_ILLEGAL_METHOD_CALL;
        return value;
    }
    try {
        const auto audio = audio_->resume(generation_);
        if (audio.generation != generation_
            || (audio.outcome != AudioPlaybackOutcome::changed
                && audio.outcome != AudioPlaybackOutcome::noOp)
            || audio.state != AudioPlaybackState::playing) {
            return failActive(
                HeadlessAvPlaybackStage::resumeAudio,
                HeadlessAvPlaybackFailureCode::audioFailure,
                audio.generation == generation_ ? audio.hresult : E_UNEXPECTED
            );
        }
        state_ = HeadlessAvPlaybackState::playing;
        auto value = baseReceipt(
            outcomeForAudio(audio.outcome),
            HeadlessAvPlaybackStage::resumeAudio
        );
        value.audioState = audio.state;
        value.audioFailure = audio.failure;
        value.hresult = audio.hresult;
        return value;
    } catch (...) {
        return failActive(
            HeadlessAvPlaybackStage::resumeAudio,
            HeadlessAvPlaybackFailureCode::audioFailure,
            E_UNEXPECTED
        );
    }
}

HeadlessAvPlaybackReceipt HeadlessAvPlaybackSession::cancel(
    std::uint64_t expectedGeneration
) {
    std::lock_guard lifecycleLock(lifecycleMutex_);
    bool closeRequested;
    bool validAtAdmission = false;
    {
        std::lock_guard operationLock(operationMutex_);
        closeRequested = closeRequested_;
        validAtAdmission = !closeRequested
            && expectedGeneration != 0
            && expectedGeneration == publishedGeneration_;
        if (validAtAdmission) {
            pendingCancellation_ = expectedGeneration;
            if (activeOperation_ != nullptr
                && activeOperation_->admittedGeneration
                    == expectedGeneration) {
                activeOperation_->cancellation.request_stop();
            }
        }
    }
    std::lock_guard lock(mutex_);
    {
        std::lock_guard operationLock(operationMutex_);
        if (pendingCancellation_ == expectedGeneration) {
            pendingCancellation_.reset();
        }
    }
    if (closeRequested) {
        auto value = baseReceipt(
            HeadlessAvPlaybackOutcome::refused,
            HeadlessAvPlaybackStage::cancel
        );
        value.failure = HeadlessAvPlaybackFailureCode::invalidRequest;
        value.hresult = E_ILLEGAL_METHOD_CALL;
        return value;
    }
    const bool interruptedReplacementCommitted = validAtAdmission
        && expectedGeneration != (std::numeric_limits<std::uint64_t>::max)()
        && generation_ == expectedGeneration + 1;
    if (!validAtAdmission
        || expectedGeneration == 0
        || (expectedGeneration != generation_
            && !interruptedReplacementCommitted)) {
        auto value = baseReceipt(
            HeadlessAvPlaybackOutcome::stale,
            HeadlessAvPlaybackStage::cancel
        );
        value.failure = HeadlessAvPlaybackFailureCode::invalidRequest;
        value.hresult = E_INVALIDARG;
        return value;
    }
    if (isTerminal(state_)) {
        return baseReceipt(terminalOutcome(state_));
    }
    return cancelCurrent();
}

HeadlessAvPlaybackReceipt HeadlessAvPlaybackSession::snapshot() const {
    std::lock_guard lock(mutex_);
    return baseReceipt(HeadlessAvPlaybackOutcome::noOp);
}

HeadlessAvPlaybackReceipt HeadlessAvPlaybackSession::close() {
    std::lock_guard lifecycleLock(lifecycleMutex_);
    {
        std::lock_guard operationLock(operationMutex_);
        closeRequested_ = true;
        if (activeOperation_ != nullptr) {
            activeOperation_->cancellation.request_stop();
        }
    }
    std::lock_guard lock(mutex_);
    if (closeReceipt_.has_value()) {
        return *closeReceipt_;
    }
    HeadlessAvPlaybackReceipt value;
    try {
        if (video_ != nullptr && generation_ != 0
            && !isTerminal(state_)) {
            video_->cancel(generation_);
        }
        const auto audio = audio_->close();
        state_ = HeadlessAvPlaybackState::closed;
        value = baseReceipt(
            audio.state == AudioPlaybackState::closed
                ? HeadlessAvPlaybackOutcome::changed
                : HeadlessAvPlaybackOutcome::failed,
            HeadlessAvPlaybackStage::close
        );
        value.audioState = audio.state;
        value.audioFailure = audio.failure;
        value.failure = audio.state == AudioPlaybackState::closed
            ? HeadlessAvPlaybackFailureCode::none
            : HeadlessAvPlaybackFailureCode::audioFailure;
        value.hresult = audio.hresult;
    } catch (...) {
        state_ = HeadlessAvPlaybackState::closed;
        value = baseReceipt(
            HeadlessAvPlaybackOutcome::failed,
            HeadlessAvPlaybackStage::close
        );
        value.failure = HeadlessAvPlaybackFailureCode::audioFailure;
        value.audioFailure = AudioPlaybackFailureCode::invariantFailure;
        value.hresult = E_UNEXPECTED;
    }
    video_.reset();
    closeReceipt_ = value;
    return value;
}

HeadlessAvPlaybackReceipt HeadlessAvPlaybackSession::baseReceipt(
    HeadlessAvPlaybackOutcome outcome,
    HeadlessAvPlaybackStage stage
) const {
    HeadlessAvPlaybackReceipt value;
    value.generation = generation_;
    value.state = state_;
    value.outcome = outcome;
    value.stage = stage;
    if (audio_ != nullptr) {
        const auto audio = audio_->snapshot();
        value.audioState = audio.state;
        value.audioFailure = audio.failure;
    }
    if (video_ != nullptr) {
        value.videoState = video_->state();
    }
    return value;
}

HeadlessAvPlaybackReceipt
HeadlessAvPlaybackSession::terminateFromAudioPosition(
    const AudioPlaybackPositionReceipt& position,
    HeadlessAvPlaybackStage stage
) {
    if (video_ != nullptr) {
        video_->cancel(generation_);
    }
    state_ = stateForAudio(position.state);
    if (state_ == HeadlessAvPlaybackState::playing
        || state_ == HeadlessAvPlaybackState::paused) {
        state_ = HeadlessAvPlaybackState::failed;
    }
    auto value = baseReceipt(outcomeForAudio(position.outcome), stage);
    value.audioState = position.state;
    value.audioFailure = position.failure;
    value.hresult = position.hresult;
    value.failure = state_ == HeadlessAvPlaybackState::failed
        || state_ == HeadlessAvPlaybackState::invalidated
        ? HeadlessAvPlaybackFailureCode::audioFailure
        : HeadlessAvPlaybackFailureCode::none;
    return value;
}

HeadlessAvPlaybackReceipt HeadlessAvPlaybackSession::failActive(
    HeadlessAvPlaybackStage stage,
    HeadlessAvPlaybackFailureCode failure,
    HRESULT hresult,
    std::int32_t mediaFailureCode
) {
    if (video_ != nullptr && generation_ != 0) {
        video_->cancel(generation_);
    }
    try {
        if (generation_ != 0) {
            audio_->cancel(generation_);
        }
    } catch (...) {
    }
    state_ = hresult == cancelledResult
        ? HeadlessAvPlaybackState::cancelled
        : HeadlessAvPlaybackState::failed;
    auto value = baseReceipt(
        hresult == cancelledResult
            ? HeadlessAvPlaybackOutcome::cancelled
            : HeadlessAvPlaybackOutcome::failed,
        stage
    );
    value.failure = failure;
    value.hresult = hresult;
    value.mediaFailureCode = mediaFailureCode;
    return value;
}

HeadlessAvPlaybackReceipt HeadlessAvPlaybackSession::cancelCurrent() {
    try {
        if (video_ != nullptr) {
            video_->cancel(generation_);
        }
        const auto audio = audio_->cancel(generation_);
        if (audio.outcome != AudioPlaybackOutcome::cancelled
            && audio.outcome != AudioPlaybackOutcome::changed
            && audio.outcome != AudioPlaybackOutcome::noOp) {
            state_ = HeadlessAvPlaybackState::failed;
            auto value = baseReceipt(
                HeadlessAvPlaybackOutcome::failed,
                HeadlessAvPlaybackStage::cancel
            );
            value.failure = HeadlessAvPlaybackFailureCode::audioFailure;
            value.audioFailure = audio.failure;
            value.hresult = audio.hresult;
            value.audioState = audio.state;
            return value;
        }
        state_ = HeadlessAvPlaybackState::cancelled;
        auto value = baseReceipt(
            HeadlessAvPlaybackOutcome::cancelled,
            HeadlessAvPlaybackStage::cancel
        );
        value.audioState = audio.state;
        value.audioFailure = audio.failure;
        value.hresult = audio.hresult;
        return value;
    } catch (...) {
        state_ = HeadlessAvPlaybackState::failed;
        auto value = baseReceipt(
            HeadlessAvPlaybackOutcome::failed,
            HeadlessAvPlaybackStage::cancel
        );
        value.failure = HeadlessAvPlaybackFailureCode::invariantFailure;
        value.audioFailure = AudioPlaybackFailureCode::invariantFailure;
        value.hresult = E_UNEXPECTED;
        return value;
    }
}

}
