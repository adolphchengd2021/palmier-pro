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
    std::atomic_uint32_t renderCredits{};
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
        auto credits = state_->renderCredits.load();
        while (credits > 0) {
            if (state_->renderCredits.compare_exchange_weak(
                    credits,
                    credits - 1
                )) {
                return S_OK;
            }
        }
        const HRESULT result = waitForWasapiRenderEvent(
            state_->renderEvent.get(),
            stopToken,
            timeoutMilliseconds
        );
        if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)
            && state_->automaticRender.load()) {
            state_->renderCredits.fetch_add(1);
        }
        return result;
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
        record();
        std::lock_guard lock(state_->mutex);
        state_->captured.clear();
        state_->clockPosition = 0;
        state_->renderCredits.store(0);
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
    require(session.play(first).generation == 1, "first generation changed");

    state->automaticRender.store(true);
    const auto replaced = session.play(second, 60);
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

}

int main() {
    try {
        playsRealPcmToOneTerminalAndAnchorsTheClock();
        failedReplacementPreservesTheRunningGeneration();
        successfulReplacementFlushesOldPcmAndUsesAnExactGeneration();
        rejectsInvalidRequestsAndPreservesNoOpState();
        concurrentCloseJoinsTheSessionAndDeviceExactlyOnce();
        std::cout << "Audio playback session tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
