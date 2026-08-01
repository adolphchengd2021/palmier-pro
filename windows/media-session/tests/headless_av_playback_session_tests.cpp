#include "palmier/media/headless_av_playback_session.hpp"

#include "media_test_fixtures.hpp"
#include "media_test_support.hpp"

#include <Windows.h>

#include <barrier>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

namespace palmier::media::detail {

class HeadlessAvPlaybackSessionTestAccess final {
public:
    static std::unique_ptr<HeadlessAvPlaybackSession> make(
        std::unique_ptr<HeadlessAvPlaybackAudioPort> audio,
        HeadlessAvPlaybackLimits limits = {}
    ) {
        return std::unique_ptr<HeadlessAvPlaybackSession>(
            new HeadlessAvPlaybackSession(std::move(audio), limits)
        );
    }
};

}

namespace {

using palmier::media::AudioPlaybackFailureCode;
using palmier::media::AudioPlaybackOutcome;
using palmier::media::AudioPlaybackPositionReceipt;
using palmier::media::AudioPlaybackReceipt;
using palmier::media::AudioPlaybackStage;
using palmier::media::AudioPlaybackState;
using palmier::media::HeadlessAvPlaybackFailureCode;
using palmier::media::HeadlessAvPlaybackLimits;
using palmier::media::HeadlessAvPlaybackOutcome;
using palmier::media::HeadlessAvPlaybackSession;
using palmier::media::HeadlessAvPlaybackState;
using palmier::media::MediaFailureCode;
using palmier::media::PresentationVideoDecodeState;
using palmier::media::detail::HeadlessAvPlaybackAudioPort;
using palmier::media::detail::HeadlessAvPlaybackSessionTestAccess;
using palmier::media::test_support::TemporaryDirectory;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct FakeAudioState final {
    std::uint64_t generation{};
    AudioPlaybackState state{AudioPlaybackState::idle};
    std::size_t playCalls{};
    std::size_t positionCalls{};
    std::size_t cancelCalls{};
    std::size_t closeCalls{};
    bool failNextPlay{};
    bool failAfterCommit{};
    bool hasClockSample{true};
    AudioPlaybackOutcome positionOutcome{AudioPlaybackOutcome::noOp};
    HRESULT positionResult{S_OK};
    std::uint64_t anchorPosition{1'000};
    std::uint64_t clockFrequency{40};
    std::uint64_t sampleDelta{};
    std::int32_t sourceTimeBaseNumerator{1};
    std::int32_t sourceTimeBaseDenominator{40};
    std::int64_t timelineFrame{};
    std::mutex gateMutex;
    std::condition_variable gateCondition;
    bool gateNextPlay{};
    bool playEntered{};
};

AudioPlaybackReceipt audioReceipt(
    const FakeAudioState& state,
    AudioPlaybackOutcome outcome,
    AudioPlaybackStage stage = AudioPlaybackStage::none
) {
    AudioPlaybackReceipt value;
    value.generation = state.generation;
    value.state = state.state;
    value.outcome = outcome;
    value.stage = stage;
    value.hresult = S_OK;
    if (state.generation != 0) {
        value.hasClockAnchor = true;
        value.clockAnchor = {
            {
                state.generation,
                state.anchorPosition,
                state.clockFrequency,
                state.timelineFrame,
            },
            0,
            state.sourceTimeBaseNumerator,
            state.sourceTimeBaseDenominator,
            0,
            false,
        };
    }
    return value;
}

class FakeAudioPort final : public HeadlessAvPlaybackAudioPort {
public:
    explicit FakeAudioPort(std::shared_ptr<FakeAudioState> state)
        : state_(std::move(state)) {}

    AudioPlaybackReceipt playExactGeneration(
        const std::filesystem::path&,
        std::int64_t timelineFrame,
        std::uint64_t generation,
        std::stop_token cancellation
    ) override {
        ++state_->playCalls;
        {
            std::unique_lock lock(state_->gateMutex);
            if (state_->gateNextPlay) {
                state_->playEntered = true;
                state_->gateCondition.notify_all();
                std::stop_callback cancellationCallback(
                    cancellation,
                    [this] { state_->gateCondition.notify_all(); }
                );
                state_->gateCondition.wait(
                    lock,
                    [&cancellation] { return cancellation.stop_requested(); }
                );
                state_->gateNextPlay = false;
                return audioReceipt(*state_, AudioPlaybackOutcome::cancelled);
            }
        }
        if (cancellation.stop_requested()) {
            return audioReceipt(*state_, AudioPlaybackOutcome::cancelled);
        }
        if (state_->failNextPlay) {
            state_->failNextPlay = false;
            auto value = audioReceipt(*state_, AudioPlaybackOutcome::refused);
            value.failure = AudioPlaybackFailureCode::mediaFailure;
            value.hresult = E_FAIL;
            return value;
        }
        if (generation != state_->generation + 1) {
            auto value = audioReceipt(*state_, AudioPlaybackOutcome::refused);
            value.failure = AudioPlaybackFailureCode::invalidRequest;
            value.hresult = E_INVALIDARG;
            return value;
        }
        state_->generation = generation;
        state_->timelineFrame = timelineFrame;
        if (state_->failAfterCommit) {
            state_->failAfterCommit = false;
            state_->state = AudioPlaybackState::failed;
            auto value = audioReceipt(*state_, AudioPlaybackOutcome::failed);
            value.failure = AudioPlaybackFailureCode::outputFailure;
            value.hresult = E_FAIL;
            return value;
        }
        state_->state = AudioPlaybackState::playing;
        return audioReceipt(
            *state_,
            AudioPlaybackOutcome::changed,
            AudioPlaybackStage::startDevice
        );
    }

    AudioPlaybackPositionReceipt position(
        std::uint64_t generation
    ) const override {
        ++state_->positionCalls;
        AudioPlaybackPositionReceipt value;
        value.generation = state_->generation;
        value.state = state_->state;
        value.outcome = state_->positionOutcome;
        value.hresult = state_->positionResult;
        if (generation != state_->generation) {
            value.outcome = AudioPlaybackOutcome::refused;
            value.failure = AudioPlaybackFailureCode::invalidRequest;
            value.hresult = E_INVALIDARG;
            return value;
        }
        value.hasClockAnchor = true;
        value.clockAnchor = audioReceipt(
            *state_,
            AudioPlaybackOutcome::noOp
        ).clockAnchor;
        value.hasClockSample = state_->hasClockSample;
        value.clockSample = {
            state_->generation,
            state_->anchorPosition + state_->sampleDelta,
            0,
            false,
        };
        return value;
    }

    AudioPlaybackReceipt cancel(std::uint64_t generation) override {
        ++state_->cancelCalls;
        if (generation != state_->generation) {
            auto value = audioReceipt(*state_, AudioPlaybackOutcome::refused);
            value.failure = AudioPlaybackFailureCode::invalidRequest;
            value.hresult = E_INVALIDARG;
            return value;
        }
        state_->state = AudioPlaybackState::cancelled;
        return audioReceipt(*state_, AudioPlaybackOutcome::cancelled);
    }

    AudioPlaybackReceipt snapshot() const override {
        return audioReceipt(*state_, AudioPlaybackOutcome::noOp);
    }

    AudioPlaybackReceipt close() override {
        ++state_->closeCalls;
        state_->state = AudioPlaybackState::closed;
        return audioReceipt(
            *state_,
            AudioPlaybackOutcome::changed,
            AudioPlaybackStage::close
        );
    }

private:
    std::shared_ptr<FakeAudioState> state_;
};

std::unique_ptr<HeadlessAvPlaybackSession> session(
    const std::shared_ptr<FakeAudioState>& state,
    HeadlessAvPlaybackLimits limits = {}
) {
    return HeadlessAvPlaybackSessionTestAccess::make(
        std::make_unique<FakeAudioPort>(state),
        limits
    );
}

void playsAndTicksOneRealVideoGeneration() {
    TemporaryDirectory media;
    const auto input = media.write(
        "av.mov",
        palmier::media::test_fixtures::qtrleOpaqueThreeFrames
    );
    auto state = std::make_shared<FakeAudioState>();
    auto playback = session(state);

    const auto started = playback->play(input, 0, {10, 1});
    require(started.outcome == HeadlessAvPlaybackOutcome::changed, "play failed");
    require(started.generation == 1, "first A/V generation changed");
    require(started.state == HeadlessAvPlaybackState::playing, "A/V did not play");
    require(state->playCalls == 1, "audio play was not called exactly once");
    const auto unchanged = playback->play(input, 0, {10, 1});
    require(unchanged.outcome == HeadlessAvPlaybackOutcome::noOp, "same play changed");
    require(state->playCalls == 1, "same play reached audio");

    const auto tick = playback->tick(1);
    require(tick.frame.has_value(), "clock tick returned no real video frame");
    require(tick.frame->presentationTimestamp == 0, "clock tick changed video PTS");
    require(tick.droppedFrames == 0, "exact video tick dropped a frame");
    require(tick.hasTargetTimelineFrame, "tick omitted timeline target");
    require(state->positionCalls == 1, "tick read the audio clock more than once");

    const auto positionsBeforeStale = state->positionCalls;
    const auto stale = playback->tick(2);
    require(stale.outcome == HeadlessAvPlaybackOutcome::stale, "stale tick was accepted");
    require(
        state->positionCalls == positionsBeforeStale,
        "stale tick read the audio clock"
    );
    const auto rerated = playback->play(input, 0, {20, 1});
    require(rerated.generation == 2, "frame-rate replacement did not advance");
    require(rerated.outcome == HeadlessAvPlaybackOutcome::changed, "frame-rate replacement no-op'd");
    require(state->playCalls == 2, "frame-rate replacement did not reach audio");
    require(playback->close().state == HeadlessAvPlaybackState::closed, "close failed");
    require(playback->close().state == HeadlessAvPlaybackState::closed, "repeat close changed");
    require(state->closeCalls == 1, "repeat close reached audio twice");
}

void failedReplacementPreservesTheActiveGeneration() {
    TemporaryDirectory media;
    const auto input = media.write(
        "av.mov",
        palmier::media::test_fixtures::qtrleOpaqueThreeFrames
    );
    auto state = std::make_shared<FakeAudioState>();
    auto playback = session(state);
    require(playback->play(input, 0, {10, 1}).generation == 1, "initial play failed");

    state->failNextPlay = true;
    const auto refusedAudio = playback->play(input, 1, {10, 1});
    require(
        refusedAudio.outcome == HeadlessAvPlaybackOutcome::refused,
        "failed audio replacement committed"
    );
    require(refusedAudio.generation == 1, "failed audio replacement changed generation");
    require(
        playback->snapshot().state == HeadlessAvPlaybackState::playing,
        "failed audio replacement stopped active playback"
    );

    const auto audioCalls = state->playCalls;
    std::stop_source stoppedReplacement;
    stoppedReplacement.request_stop();
    const auto cancelledVideo = playback->play(
        input,
        2,
        {10, 1},
        stoppedReplacement.get_token()
    );
    require(
        cancelledVideo.outcome == HeadlessAvPlaybackOutcome::cancelled,
        "cancelled video preparation was not reported"
    );
    require(
        cancelledVideo.generation == 1,
        "cancelled video preparation changed generation"
    );
    require(
        state->playCalls == audioCalls,
        "cancelled video preparation reached audio"
    );
    require(
        playback->snapshot().state == HeadlessAvPlaybackState::playing,
        "cancelled video preparation stopped old audio"
    );
    const auto refusedVideo = playback->play(
        media.path() / "missing.mp4",
        3,
        {10, 1}
    );
    require(
        refusedVideo.failure == HeadlessAvPlaybackFailureCode::videoFailure,
        "missing video returned the wrong failure"
    );
    require(refusedVideo.generation == 1, "failed video replacement changed generation");
    require(state->playCalls == audioCalls, "failed video preparation reached audio");

    const auto tick = playback->tick(1);
    require(tick.frame.has_value(), "failed replacement discarded the active video");
    state->failAfterCommit = true;
    const auto committedFailure = playback->play(input, 3, {10, 1});
    require(committedFailure.generation == 2, "post-commit failure kept old generation");
    require(committedFailure.state == HeadlessAvPlaybackState::failed, "post-commit failure stayed playing");
    const auto recovered = playback->play(input, 4, {10, 1});
    require(recovered.generation == 3, "post-commit failure blocked next generation");
    require(recovered.state == HeadlessAvPlaybackState::playing, "post-commit recovery failed");
    playback->close();
}

void boundsCatchUpAndDeliversOnlyTheLatestFrame() {
    TemporaryDirectory media;
    const auto input = media.write(
        "three.mov",
        palmier::media::test_fixtures::qtrleOpaqueThreeFrames
    );
    auto state = std::make_shared<FakeAudioState>();
    state->clockFrequency = 10'240;
    state->sourceTimeBaseDenominator = 10'240;
    state->sampleDelta = 2'048;
    HeadlessAvPlaybackLimits limits;
    limits.video.maximumFramesPerFill = 1;
    limits.maximumVideoFillCallsPerTick = 2;
    auto playback = session(state, limits);
    require(playback->play(input, 0, {10, 1}).generation == 1, "catch-up play failed");

    const auto tick = playback->tick(1);
    require(tick.frame.has_value(), "catch-up returned no frame");
    require(
        tick.frame->presentationTimestamp == 2'048,
        "catch-up did not return the latest due frame"
    );
    require(tick.fillCalls == 2, "catch-up exceeded its fill-call budget");
    require(tick.admittedFrames == 2, "catch-up admitted the wrong frame count");
    require(tick.droppedFrames == 2, "catch-up reported the wrong drop count");
    require(tick.fillBudgetExhausted, "catch-up hid budget exhaustion");

    const auto eof = playback->tick(1);
    require(!eof.frame.has_value(), "EOF replayed a frame");
    require(
        eof.videoState == PresentationVideoDecodeState::endOfStream,
        "EOF was not stable"
    );
    require(eof.state == HeadlessAvPlaybackState::playing, "video EOF ended audio");
    playback->close();
}

void noSampleAndAudioTerminalsDoNotGuessVideoTime() {
    TemporaryDirectory media;
    const auto input = media.write(
        "av.mov",
        palmier::media::test_fixtures::qtrleOpaqueThreeFrames
    );
    auto state = std::make_shared<FakeAudioState>();
    state->hasClockSample = false;
    auto playback = session(state);
    require(playback->play(input, 0, {10, 1}).generation == 1, "play failed");

    const auto pending = playback->tick(1);
    require(pending.outcome == HeadlessAvPlaybackOutcome::noOp, "no-sample tick changed");
    require(!pending.frame.has_value(), "no-sample tick guessed a frame");
    require(state->positionCalls == 1, "no-sample tick reread the clock");

    state->state = AudioPlaybackState::completed;
    const auto completed = playback->tick(1);
    require(completed.state == HeadlessAvPlaybackState::completed, "audio EOF did not end A/V");
    require(!completed.frame.has_value(), "audio EOF without a sample guessed a frame");
    require(playback->tick(1).outcome == HeadlessAvPlaybackOutcome::noOp, "late tick changed terminal");
    playback->close();

    auto failedState = std::make_shared<FakeAudioState>();
    auto failedPlayback = session(failedState);
    require(failedPlayback->play(input, 0, {10, 1}).generation == 1, "failure play failed");
    failedState->state = AudioPlaybackState::failed;
    failedState->positionOutcome = AudioPlaybackOutcome::failed;
    failedState->positionResult = E_FAIL;
    const auto failed = failedPlayback->tick(1);
    require(failed.state == HeadlessAvPlaybackState::failed, "audio failure was hidden");
    require(failedState->cancelCalls == 0, "terminal audio was cancelled again");
    failedPlayback->close();
}

void validatesRequestsCancellationAndConcurrentClose() {
    bool threw = false;
    try {
        HeadlessAvPlaybackLimits invalid;
        invalid.maximumVideoFillCallsPerTick = 0;
        auto state = std::make_shared<FakeAudioState>();
        static_cast<void>(session(state, invalid));
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "zero fill-call budget was accepted");

    TemporaryDirectory media;
    const auto input = media.write(
        "av.mov",
        palmier::media::test_fixtures::qtrleOpaqueThreeFrames
    );
    auto state = std::make_shared<FakeAudioState>();
    auto playback = session(state);
    const auto invalidRate = playback->play(input, 0, {0, 1});
    require(
        invalidRate.failure == HeadlessAvPlaybackFailureCode::invalidRequest,
        "invalid frame rate was accepted"
    );
    require(state->playCalls == 0, "invalid request reached audio");
    require(playback->play(input, 0, {10, 1}).generation == 1, "play failed");
    require(
        playback->cancel(2).outcome == HeadlessAvPlaybackOutcome::stale,
        "stale cancel was accepted"
    );
    const auto cancelled = playback->cancel(1);
    require(cancelled.state == HeadlessAvPlaybackState::cancelled, "cancel failed");
    require(state->cancelCalls == 1, "cancel reached audio more than once");
    require(
        playback->cancel(1).outcome == HeadlessAvPlaybackOutcome::cancelled,
        "repeat cancel changed terminal"
    );
    require(state->cancelCalls == 1, "repeat cancel reached audio");

    std::barrier start(3);
    std::optional<palmier::media::HeadlessAvPlaybackReceipt> first;
    std::optional<palmier::media::HeadlessAvPlaybackReceipt> second;
    std::jthread firstCloser([&] {
        start.arrive_and_wait();
        first = playback->close();
    });
    std::jthread secondCloser([&] {
        start.arrive_and_wait();
        second = playback->close();
    });
    start.arrive_and_wait();
    firstCloser.join();
    secondCloser.join();
    require(first.has_value() && second.has_value(), "concurrent close lost a receipt");
    require(first->state == HeadlessAvPlaybackState::closed, "first close failed");
    require(second->state == HeadlessAvPlaybackState::closed, "second close failed");
    require(state->closeCalls == 1, "concurrent close reached audio twice");

    auto gatedState = std::make_shared<FakeAudioState>();
    gatedState->gateNextPlay = true;
    auto gatedPlayback = session(gatedState);
    std::optional<palmier::media::HeadlessAvPlaybackReceipt> playResult;
    std::jthread player([&] {
        playResult = gatedPlayback->play(input, 0, {10, 1});
    });
    {
        std::unique_lock lock(gatedState->gateMutex);
        gatedState->gateCondition.wait(
            lock,
            [&] { return gatedState->playEntered; }
        );
    }
    const auto gatedClose = gatedPlayback->close();
    player.join();
    require(playResult.has_value(), "interrupted play lost its receipt");
    require(
        playResult->outcome == HeadlessAvPlaybackOutcome::cancelled,
        "close did not cancel active play"
    );
    require(gatedClose.state == HeadlessAvPlaybackState::closed, "gated close failed");
    require(gatedState->closeCalls == 1, "gated close reached audio twice");
    require(
        gatedPlayback->play(input, 0, {10, 1}).outcome
            == HeadlessAvPlaybackOutcome::refused,
        "play was admitted after close requested"
    );
}

}

int main() {
    try {
        playsAndTicksOneRealVideoGeneration();
        failedReplacementPreservesTheActiveGeneration();
        boundsCatchUpAndDeliversOnlyTheLatestFrame();
        noSampleAndAudioTerminalsDoNotGuessVideoTime();
        validatesRequestsCancellationAndConcurrentClose();
        std::cout << "headless A/V playback session tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
