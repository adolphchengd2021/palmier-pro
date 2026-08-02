#include "media_test_fixtures.hpp"
#include "media_test_support.hpp"
#include "palmier/media/audio_playback_session.hpp"
#include "palmier/media/ffmpeg_media_reader.hpp"
#include "wasapi_native_stream.hpp"
#include "wasapi_output_worker_testing.hpp"

#include <Windows.h>
#include <audioclient.h>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using palmier::audio::PcmFormat;
using palmier::audio::PcmSampleEncoding;
using palmier::audio::WasapiClockReading;
using palmier::audio::WasapiMixFormat;
using palmier::audio::WasapiOutputWorker;
using palmier::audio::WasapiOutputWorkerStream;
using palmier::audio::WasapiSharedModePeriods;
using palmier::audio::waitForWasapiRenderEvent;
using palmier::media::AudioPlaybackOutcome;
using palmier::media::AudioPlaybackSession;
using palmier::media::AudioPlaybackStage;
using palmier::media::AudioPlaybackState;
using palmier::media::DecodeFrameStart;
using palmier::media::FfmpegAudioFrameReader;
using palmier::media::test_support::TemporaryDirectory;
using palmier::media::test_support::require;

class HandleOwner final {
public:
    explicit HandleOwner(HANDLE value) : value_(value) {}
    ~HandleOwner() {
        if (value_ != nullptr) {
            CloseHandle(value_);
        }
    }

    [[nodiscard]] HANDLE get() const { return value_; }

    HandleOwner(const HandleOwner&) = delete;
    HandleOwner& operator=(const HandleOwner&) = delete;

private:
    HANDLE value_{};
};

PcmFormat stereo48k() {
    return {48'000, 2, 16, 16, 4, 0x3, PcmSampleEncoding::integer, true};
}

struct PlaybackStreamState final {
    HandleOwner renderEvent{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    std::mutex mutex;
    std::vector<std::byte> captured;
    std::vector<DWORD> nativeThreads;
    std::atomic_bool automaticRender{true};
    DWORD constructedThread{};
    DWORD destroyedThread{};
    std::uint64_t clockPosition{};
    std::uint32_t closeCalls{};
};

class PlaybackStream final : public WasapiOutputWorkerStream {
public:
    explicit PlaybackStream(std::shared_ptr<PlaybackStreamState> state)
        : state_(std::move(state)), lease_(96 * stereo48k().blockAlign) {
        state_->constructedThread = GetCurrentThreadId();
    }

    ~PlaybackStream() override {
        state_->destroyedThread = GetCurrentThreadId();
    }

    HRESULT initializeApartment() override { return record(); }
    HRESULT createEnumerator() override { return record(); }
    HRESULT selectDefaultRenderEndpoint(std::string& endpointId) override {
        endpointId = "playback-session-endpoint";
        return record();
    }
    HRESULT activateAudioClient() override { return record(); }
    HRESULT loadMixFormat(WasapiMixFormat& format) override {
        format = stereo48k();
        return record();
    }
    HRESULT setClientProperties() override { return record(); }
    HRESULT loadSharedModePeriods(WasapiSharedModePeriods& periods) override {
        periods = {96, 1, 1, 192};
        return record();
    }
    HRESULT initializeSharedAudioStream(std::uint32_t) override {
        return record();
    }
    HRESULT loadBufferFrames(std::uint32_t& bufferFrames) override {
        bufferFrames = 96;
        return record();
    }
    HRESULT attachRenderEvent() override { return record(); }
    HRESULT loadRenderService() override { return record(); }
    HRESULT loadClockService() override { return record(); }
    HRESULT loadClockFrequency(std::uint64_t& frequency) override {
        frequency = 48'000;
        return record();
    }

    HRESULT waitForRenderEvent(
        std::stop_token stopToken,
        std::uint32_t timeoutMilliseconds
    ) noexcept override {
        record();
        if (state_->automaticRender.load()) {
            std::this_thread::yield();
            return stopToken.stop_requested()
                ? HRESULT_FROM_WIN32(ERROR_CANCELLED)
                : S_OK;
        }
        return waitForWasapiRenderEvent(
            state_->renderEvent.get(),
            stopToken,
            timeoutMilliseconds
        );
    }
    HRESULT loadCurrentPadding(std::uint32_t& paddingFrames) noexcept override {
        paddingFrames = 0;
        return record();
    }
    HRESULT acquireBuffer(
        std::uint32_t frameCount,
        std::byte*& data
    ) noexcept override {
        record();
        const auto byteCount = static_cast<std::size_t>(frameCount)
            * stereo48k().blockAlign;
        if (byteCount > lease_.size()) {
            data = nullptr;
            return E_UNEXPECTED;
        }
        std::fill(
            lease_.begin(),
            lease_.begin() + static_cast<std::ptrdiff_t>(byteCount),
            std::byte{0}
        );
        activeFrames_ = frameCount;
        data = lease_.data();
        return S_OK;
    }
    HRESULT releaseBuffer(std::uint32_t frameCount, DWORD flags) noexcept override {
        record();
        if (frameCount == 0) {
            activeFrames_ = 0;
            return S_OK;
        }
        if (frameCount != activeFrames_) {
            return AUDCLNT_E_INVALID_SIZE;
        }
        if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) == 0) {
            const auto byteCount = static_cast<std::size_t>(frameCount)
                * stereo48k().blockAlign;
            std::lock_guard lock(state_->mutex);
            state_->captured.insert(
                state_->captured.end(),
                lease_.begin(),
                lease_.begin() + static_cast<std::ptrdiff_t>(byteCount)
            );
        }
        state_->clockPosition += frameCount;
        activeFrames_ = 0;
        return S_OK;
    }
    HRESULT start() noexcept override { return record(); }
    HRESULT loadClockPosition(WasapiClockReading& reading) noexcept override {
        record();
        reading = {
            state_->clockPosition,
            state_->clockPosition * 100,
            false,
        };
        return S_OK;
    }
    HRESULT stop() noexcept override { return record(); }
    HRESULT reset() noexcept override {
        const auto result = record();
        if (FAILED(result)) {
            return result;
        }
        {
            std::lock_guard lock(state_->mutex);
            state_->captured.clear();
            state_->clockPosition = 0;
        }
        if (ResetEvent(state_->renderEvent.get()) == 0) {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        return S_OK;
    }
    HRESULT close() noexcept override {
        record();
        ++state_->closeCalls;
        return S_OK;
    }

private:
    HRESULT record() noexcept {
        std::lock_guard lock(state_->mutex);
        state_->nativeThreads.push_back(GetCurrentThreadId());
        return S_OK;
    }

    std::shared_ptr<PlaybackStreamState> state_;
    std::vector<std::byte> lease_;
    std::uint32_t activeFrames_{};
};

std::unique_ptr<WasapiOutputWorker> outputWorker(
    const std::shared_ptr<PlaybackStreamState>& state
) {
    return std::make_unique<WasapiOutputWorker>([state] {
        return std::make_unique<PlaybackStream>(state);
    });
}

std::vector<std::byte> expectedPcm(const std::filesystem::path& input) {
    FfmpegAudioFrameReader reader(input, stereo48k());
    std::vector<std::byte> bytes;
    for (;;) {
        auto block = reader.nextBlock();
        if (!block.has_value()) {
            return bytes;
        }
        bytes.insert(
            bytes.end(),
            block->interleavedBytes.begin(),
            block->interleavedBytes.end()
        );
    }
}

std::vector<std::byte> expectedPcm(
    const std::filesystem::path& input,
    DecodeFrameStart start
) {
    FfmpegAudioFrameReader reader(input, stereo48k(), start);
    std::vector<std::byte> bytes;
    for (;;) {
        auto block = reader.nextBlock();
        if (!block.has_value()) return bytes;
        bytes.insert(
            bytes.end(),
            block->interleavedBytes.begin(),
            block->interleavedBytes.end()
        );
    }
}

void automaticRenderClockCannotLoseProgressToCancellation() {
    auto state = std::make_shared<PlaybackStreamState>();
    PlaybackStream stream(state);
    std::stop_source cancelled;
    cancelled.request_stop();
    require(
        stream.waitForRenderEvent(cancelled.get_token(), 0)
            == HRESULT_FROM_WIN32(ERROR_CANCELLED),
        "automatic fake ignored cancellation"
    );
    require(
        stream.waitForRenderEvent({}, 0) == S_OK,
        "automatic fake cancellation blocked later progress"
    );
    state->automaticRender.store(false);
    require(SetEvent(state->renderEvent.get()) != 0, "manual fake render signal failed");
    require(stream.waitForRenderEvent({}, 0) == S_OK, "manual fake render wait failed");
    require(SetEvent(state->renderEvent.get()) != 0, "stale fake render signal failed");
    require(SUCCEEDED(stream.reset()), "automatic fake reset failed");
    require(
        stream.waitForRenderEvent({}, 0)
            == HRESULT_FROM_WIN32(ERROR_TIMEOUT),
        "automatic fake reset retained a stale render event"
    );
}

void playsRealPcmToOneTerminalAndAnchorsTheClock() {
    TemporaryDirectory media;
    const auto input = media.write(
        "patterned.wav",
        palmier::media::test_fixtures::patternedPcmWav
    );
    const auto expected = expectedPcm(input);
    auto state = std::make_shared<PlaybackStreamState>();
    AudioPlaybackSession session(outputWorker(state));

    const auto started = session.play(input, 30);
    require(started.outcome == AudioPlaybackOutcome::changed, "play failed");
    require(started.state == AudioPlaybackState::playing, "session did not play");
    require(started.generation == 1, "first generation changed");
    require(started.hasClockAnchor, "playback clock was not anchored");
    require(started.clockAnchor.value.timelineFrame == 30, "timeline anchor changed");
    require(started.clockAnchor.value.frequency == 48'000, "clock frequency changed");
    require(started.clockAnchor.value.devicePosition == 96, "device anchor changed");
    require(
        started.clockAnchor.sourcePresentationTimestamp == 0,
        "source timestamp anchor changed"
    );
    require(
        started.clockAnchor.sourceTimeBaseNumerator == 1
            && started.clockAnchor.sourceTimeBaseDenominator == 24'000,
        "source time base anchor changed"
    );

    const auto position = session.position(1);
    require(position.hasClockAnchor, "position lost the playback anchor");
    require(position.hasClockSample, "position did not expose a device sample");
    require(position.clockSample.generation == 1, "position generation changed");
    require(
        position.clockSample.devicePosition
            >= position.clockAnchor.value.devicePosition,
        "position regressed before its anchor"
    );
    require(
        session.position(2).outcome == AudioPlaybackOutcome::refused,
        "stale position read was accepted"
    );

    const auto terminal = session.waitForTerminal(1);
    require(terminal.state == AudioPlaybackState::completed, "playback did not complete");
    require(terminal.acceptedFrames == 1'536, "accepted frame count changed");
    {
        std::lock_guard lock(state->mutex);
        require(state->captured == expected, "session changed canonical PCM bytes");
    }
    require(session.close().state == AudioPlaybackState::closed, "close failed");
    require(state->destroyedThread == state->constructedThread, "stream teardown crossed threads");
    std::lock_guard lock(state->mutex);
    for (const DWORD thread : state->nativeThreads) {
        require(thread == state->constructedThread, "native call crossed device thread");
    }
}

void trimmedPlaybackAnchorsAndHandsOffTheSameSourceRange() {
    TemporaryDirectory media;
    const auto input = media.write(
        "trimmed-patterned.wav",
        palmier::media::test_fixtures::patternedPcmWav
    );
    const DecodeFrameStart start{1, {100, 1}};
    const auto expected = expectedPcm(input, start);
    auto state = std::make_shared<PlaybackStreamState>();
    AudioPlaybackSession session(outputWorker(state));

    const auto started = session.play(input, 12, start);
    require(started.outcome == AudioPlaybackOutcome::changed, "trimmed audio play failed");
    require(started.clockAnchor.value.timelineFrame == 12, "trimmed timeline anchor changed");
    require(
        started.clockAnchor.sourcePresentationTimestamp == 240,
        "trimmed source anchor did not move to the requested frame"
    );
    const auto terminal = session.waitForTerminal(started.generation);
    require(terminal.state == AudioPlaybackState::completed, "trimmed audio did not complete");
    require(terminal.acceptedFrames == 1'056, "trimmed audio frame count changed");
    std::lock_guard lock(state->mutex);
    require(state->captured == expected, "trimmed playback changed the decoded source range");
}

void pauseResumePreservesGenerationClockAndQueuedPcm() {
    TemporaryDirectory media;
    const auto input = media.write(
        "pause-patterned.wav",
        palmier::media::test_fixtures::patternedPcmWav
    );
    auto state = std::make_shared<PlaybackStreamState>();
    state->automaticRender.store(false);
    AudioPlaybackSession session(outputWorker(state));

    const auto started = session.play(input, 24);
    require(started.state == AudioPlaybackState::playing, "pause test play failed");
    std::vector<std::byte> capturedBeforePause;
    {
        std::lock_guard lock(state->mutex);
        capturedBeforePause = state->captured;
    }
    const auto paused = session.pause(1);
    require(paused.outcome == AudioPlaybackOutcome::changed, "audio pause failed");
    require(paused.state == AudioPlaybackState::paused, "audio pause lost state");
    require(paused.generation == 1, "audio pause changed generation");
    require(paused.clockAnchor.value.timelineFrame == 24, "audio pause changed clock anchor");
    require(session.pause(1).outcome == AudioPlaybackOutcome::noOp, "repeat audio pause changed");
    require(session.pause(2).outcome == AudioPlaybackOutcome::refused, "stale audio pause was accepted");

    const auto pausedPosition = session.position(1);
    require(pausedPosition.state == AudioPlaybackState::paused, "paused position lost state");
    require(pausedPosition.hasClockAnchor, "paused position lost clock anchor");
    const auto resumed = session.resume(1);
    require(resumed.outcome == AudioPlaybackOutcome::changed, "audio resume failed");
    require(resumed.state == AudioPlaybackState::playing, "audio resume lost state");
    require(resumed.generation == 1, "audio resume changed generation");
    require(resumed.clockAnchor.value.timelineFrame == 24, "audio resume changed clock anchor");
    require(session.resume(1).outcome == AudioPlaybackOutcome::noOp, "repeat audio resume changed");
    require(session.resume(2).outcome == AudioPlaybackOutcome::refused, "stale audio resume was accepted");

    const auto pausedAgain = session.pause(1);
    require(pausedAgain.state == AudioPlaybackState::paused, "second audio pause failed");
    {
        std::lock_guard lock(state->mutex);
        require(state->captured == capturedBeforePause, "pause or resume flushed queued PCM");
    }
    const auto cancelled = session.cancel(1);
    require(cancelled.state == AudioPlaybackState::cancelled, "paused audio cancel failed");
    require(session.close().state == AudioPlaybackState::closed, "paused audio close failed");
}

void failedReplacementPreservesTheRunningGeneration() {
    TemporaryDirectory media;
    const auto input = media.write(
        "active.wav",
        palmier::media::test_fixtures::patternedPcmWav
    );
    auto state = std::make_shared<PlaybackStreamState>();
    state->automaticRender.store(false);
    AudioPlaybackSession session(outputWorker(state));
    const auto started = session.play(input);
    require(started.state == AudioPlaybackState::playing, "initial play failed");

    const auto refused = session.play(media.path() / "missing.wav");
    require(refused.outcome == AudioPlaybackOutcome::refused, "missing replacement committed");
    require(refused.stage == AudioPlaybackStage::openInput, "replacement stage changed");
    require(refused.generation == 1, "failed replacement changed generation");
    require(refused.state == AudioPlaybackState::playing, "failed replacement stopped playback");
    require(session.snapshot().generation == 1, "snapshot changed generation");

    const auto cancelled = session.cancel(1);
    require(cancelled.state == AudioPlaybackState::cancelled, "cancel failed");
    const auto cancelledPosition = session.position(1);
    require(
        cancelledPosition.state == AudioPlaybackState::cancelled
            && cancelledPosition.outcome == AudioPlaybackOutcome::cancelled,
        "position hid the cancelled terminal"
    );
    require(
        session.waitForTerminal(1).state == AudioPlaybackState::cancelled,
        "cancel terminal was not queryable"
    );
    require(session.close().state == AudioPlaybackState::closed, "close failed");
}

void successfulReplacementFlushesOldPcmAndUsesAnExactGeneration() {
    TemporaryDirectory media;
    const auto first = media.write(
        "first.wav",
        palmier::media::test_fixtures::patternedPcmWav
    );
    const auto second = media.write(
        "second.wav",
        palmier::media::test_fixtures::patternedPcmWav
    );
    const auto expected = expectedPcm(second);
    auto state = std::make_shared<PlaybackStreamState>();
    state->automaticRender.store(false);
    AudioPlaybackSession session(outputWorker(state));
    require(
        session.playExactGeneration(first, 0, 2).outcome
            == AudioPlaybackOutcome::refused,
        "incorrect first exact generation was accepted"
    );
    require(session.snapshot().generation == 0, "refused exact generation committed");
    require(
        session.playExactGeneration(first, 0, 1).generation == 1,
        "first exact generation changed"
    );

    require(
        session.playExactGeneration(second, 60, 3).outcome
            == AudioPlaybackOutcome::refused,
        "skipped exact generation was accepted"
    );
    require(session.snapshot().generation == 1, "skipped generation changed state");

    state->automaticRender.store(true);
    const auto replaced = session.playExactGeneration(second, 60, 2);
    require(replaced.outcome == AudioPlaybackOutcome::changed, "replacement failed");
    require(replaced.state == AudioPlaybackState::playing, "replacement did not play");
    require(replaced.generation == 2, "replacement generation was not exact");
    require(replaced.clockAnchor.value.generation == 2, "clock anchor stayed stale");
    require(replaced.clockAnchor.value.timelineFrame == 60, "replacement timeline changed");
    const auto oldTerminal = session.waitForTerminal(1, std::stop_token{});
    require(oldTerminal.outcome == AudioPlaybackOutcome::refused, "old waiter did not terminate");

    const auto terminal = session.waitForTerminal(2);
    require(terminal.state == AudioPlaybackState::completed, "replacement did not complete");
    require(terminal.acceptedFrames == 1'536, "replacement frame count changed");
    {
        std::lock_guard lock(state->mutex);
        require(state->captured == expected, "replacement retained old PCM");
    }
    require(session.close().state == AudioPlaybackState::closed, "close failed");
}

void exactGenerationRestartsAnOtherwiseIdenticalRequest() {
    TemporaryDirectory media;
    const auto input = media.write(
        "active.wav",
        palmier::media::test_fixtures::patternedPcmWav
    );
    auto state = std::make_shared<PlaybackStreamState>();
    state->automaticRender.store(false);
    AudioPlaybackSession session(outputWorker(state));
    std::stop_source cancelled;
    cancelled.request_stop();
    require(
        session.playExactGeneration(input, 0, 1, cancelled.get_token()).outcome
            == AudioPlaybackOutcome::cancelled,
        "pre-cancelled exact generation was accepted"
    );
    require(
        session.snapshot().generation == 0,
        "pre-cancelled exact generation committed"
    );
    require(
        session.playExactGeneration(input, 0, 1).generation == 1,
        "cancelled exact generation poisoned the next request"
    );
    const auto restarted = session.playExactGeneration(input, 0, 2);
    require(restarted.outcome == AudioPlaybackOutcome::changed, "exact restart no-op'd");
    require(restarted.generation == 2, "exact restart changed generation");
    require(session.cancel(2).state == AudioPlaybackState::cancelled, "cancel failed");
    require(session.close().state == AudioPlaybackState::closed, "close failed");
}

void cancelledExactReplacementResumesTheActiveGeneration() {
    TemporaryDirectory media;
    const auto input = media.write(
        "active.wav",
        palmier::media::test_fixtures::patternedPcmWav
    );
    auto state = std::make_shared<PlaybackStreamState>();
    state->automaticRender.store(false);
    AudioPlaybackSession session(outputWorker(state));
    require(session.playExactGeneration(input, 0, 1).generation == 1, "play failed");

    std::stop_source cancelled;
    cancelled.request_stop();
    const auto replacement = session.playExactGeneration(
        input,
        1,
        2,
        cancelled.get_token()
    );
    require(replacement.outcome == AudioPlaybackOutcome::cancelled, "replacement was not cancelled");
    require(replacement.generation == 1, "cancelled replacement changed generation");
    require(replacement.state == AudioPlaybackState::playing, "cancelled replacement stopped playback");

    state->automaticRender.store(true);
    SetEvent(state->renderEvent.get());
    require(
        session.waitForTerminal(1).state == AudioPlaybackState::completed,
        "cancelled replacement poisoned active handoff"
    );
    require(session.close().state == AudioPlaybackState::closed, "close failed");
}

void rejectsInvalidRequestsAndPreservesNoOpState() {
    TemporaryDirectory media;
    const auto input = media.write(
        "active.wav",
        palmier::media::test_fixtures::patternedPcmWav
    );
    auto state = std::make_shared<PlaybackStreamState>();
    state->automaticRender.store(false);
    AudioPlaybackSession session(outputWorker(state));

    const auto empty = session.play({});
    require(empty.outcome == AudioPlaybackOutcome::refused, "empty input was accepted");
    require(empty.generation == 0, "empty input advanced the generation");
    const auto negative = session.play(input, -1);
    require(negative.outcome == AudioPlaybackOutcome::refused, "negative timeline was accepted");
    require(negative.generation == 0, "negative timeline advanced the generation");
    require(
        session.waitForTerminal(0).outcome == AudioPlaybackOutcome::refused,
        "zero generation waiter did not terminate"
    );

    const auto started = session.play(input, 12);
    require(started.generation == 1, "valid play did not start generation one");
    const auto unchanged = session.play(input, 12);
    require(unchanged.outcome == AudioPlaybackOutcome::noOp, "identical play changed state");
    require(unchanged.generation == 1, "identical play advanced the generation");
    const auto reanchored = session.play(input, 13);
    require(reanchored.generation == 2, "new timeline anchor did not replace playback");
    require(reanchored.clockAnchor.value.timelineFrame == 13, "timeline reanchor changed");
    const auto staleCancel = session.cancel(1);
    require(staleCancel.outcome == AudioPlaybackOutcome::refused, "stale cancel changed state");
    require(session.snapshot().state == AudioPlaybackState::playing, "stale cancel stopped playback");
    require(session.close().state == AudioPlaybackState::closed, "close failed");
}

void concurrentCloseJoinsTheSessionAndDeviceExactlyOnce() {
    TemporaryDirectory media;
    const auto input = media.write(
        "active.wav",
        palmier::media::test_fixtures::patternedPcmWav
    );
    auto state = std::make_shared<PlaybackStreamState>();
    state->automaticRender.store(false);
    AudioPlaybackSession session(outputWorker(state));
    require(session.play(input).state == AudioPlaybackState::playing, "play failed");

    std::barrier start(3);
    std::optional<palmier::media::AudioPlaybackReceipt> first;
    std::optional<palmier::media::AudioPlaybackReceipt> second;
    std::jthread firstCloser([&] {
        start.arrive_and_wait();
        first = session.close();
    });
    std::jthread secondCloser([&] {
        start.arrive_and_wait();
        second = session.close();
    });
    start.arrive_and_wait();
    firstCloser.join();
    secondCloser.join();

    require(first.has_value() && second.has_value(), "concurrent close lost a receipt");
    require(first->state == AudioPlaybackState::closed, "first close did not finish");
    require(second->state == AudioPlaybackState::closed, "second close did not finish");
    require(first->outcome == second->outcome, "concurrent close outcomes diverged");
    const auto terminal = session.waitForTerminal(1);
    require(terminal.state == AudioPlaybackState::cancelled, "close lost active terminal");
    require(terminal.outcome == AudioPlaybackOutcome::cancelled, "close terminal changed outcome");
    {
        std::lock_guard lock(state->mutex);
        require(state->closeCalls == 1, "concurrent close repeated native teardown");
    }
    require(state->destroyedThread == state->constructedThread, "stream teardown crossed threads");
}

template<typename Test>
void runCase(const char* name, Test&& test) {
    std::cout << "RUN " << name << std::endl;
    std::forward<Test>(test)();
    std::cout << "PASS " << name << std::endl;
}

}

int main() {
    try {
        runCase(
            "automaticRenderClockCannotLoseProgressToCancellation",
            automaticRenderClockCannotLoseProgressToCancellation
        );
        runCase(
            "playsRealPcmToOneTerminalAndAnchorsTheClock",
            playsRealPcmToOneTerminalAndAnchorsTheClock
        );
        runCase(
            "trimmedPlaybackAnchorsAndHandsOffTheSameSourceRange",
            trimmedPlaybackAnchorsAndHandsOffTheSameSourceRange
        );
        runCase(
            "pauseResumePreservesGenerationClockAndQueuedPcm",
            pauseResumePreservesGenerationClockAndQueuedPcm
        );
        runCase(
            "failedReplacementPreservesTheRunningGeneration",
            failedReplacementPreservesTheRunningGeneration
        );
        runCase(
            "successfulReplacementFlushesOldPcmAndUsesAnExactGeneration",
            successfulReplacementFlushesOldPcmAndUsesAnExactGeneration
        );
        runCase(
            "exactGenerationRestartsAnOtherwiseIdenticalRequest",
            exactGenerationRestartsAnOtherwiseIdenticalRequest
        );
        runCase(
            "cancelledExactReplacementResumesTheActiveGeneration",
            cancelledExactReplacementResumesTheActiveGeneration
        );
        runCase(
            "rejectsInvalidRequestsAndPreservesNoOpState",
            rejectsInvalidRequestsAndPreservesNoOpState
        );
        runCase(
            "concurrentCloseJoinsTheSessionAndDeviceExactlyOnce",
            concurrentCloseJoinsTheSessionAndDeviceExactlyOnce
        );
        std::cout << "Audio playback session tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
