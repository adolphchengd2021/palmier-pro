#pragma once

#include "palmier/audio/audio_clock.hpp"
#include "palmier/audio/wasapi_output_worker.hpp"

#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <stop_token>

namespace palmier::media {

enum class AudioPlaybackState {
    idle,
    preparing,
    playing,
    completed,
    cancelled,
    invalidated,
    failed,
    closed,
};

enum class AudioPlaybackOutcome {
    changed,
    noOp,
    cancelled,
    refused,
    failed,
    invalidated,
};

enum class AudioPlaybackStage {
    none,
    deviceConfiguration,
    openInput,
    prebuffer,
    installGeneration,
    pcmHandoff,
    startDevice,
    drain,
    close,
};

enum class AudioPlaybackFailureCode {
    none,
    invalidRequest,
    deviceUnavailable,
    deviceFailure,
    mediaFailure,
    generationExhausted,
    handoffFailure,
    outputFailure,
    invariantFailure,
};

struct AudioPlaybackClockAnchor final {
    audio::AudioClockAnchor value;
    std::int64_t sourcePresentationTimestamp{};
    std::int32_t sourceTimeBaseNumerator{};
    std::int32_t sourceTimeBaseDenominator{1};
    std::uint64_t qpc100Nanoseconds{};
    bool precisionDegraded{};
};

struct AudioPlaybackPositionReceipt final {
    std::uint64_t generation{};
    AudioPlaybackState state{AudioPlaybackState::idle};
    AudioPlaybackOutcome outcome{AudioPlaybackOutcome::noOp};
    AudioPlaybackFailureCode failure{AudioPlaybackFailureCode::none};
    HRESULT hresult{S_OK};
    bool hasClockAnchor{};
    AudioPlaybackClockAnchor clockAnchor;
    bool hasClockSample{};
    audio::AudioClockSample clockSample;
};

struct AudioPlaybackReceipt final {
    std::uint64_t generation{};
    AudioPlaybackState state{AudioPlaybackState::idle};
    AudioPlaybackOutcome outcome{AudioPlaybackOutcome::noOp};
    AudioPlaybackStage stage{AudioPlaybackStage::none};
    AudioPlaybackFailureCode failure{AudioPlaybackFailureCode::none};
    HRESULT hresult{S_OK};
    std::int32_t mediaFailureCode{-1};
    std::uint64_t acceptedFrames{};
    bool hasClockAnchor{};
    AudioPlaybackClockAnchor clockAnchor;
};

class AudioPlaybackSession final {
public:
    AudioPlaybackSession();
    explicit AudioPlaybackSession(
        std::unique_ptr<audio::WasapiOutputWorker> outputWorker
    );
    ~AudioPlaybackSession();

    AudioPlaybackSession(const AudioPlaybackSession&) = delete;
    AudioPlaybackSession& operator=(const AudioPlaybackSession&) = delete;

    AudioPlaybackReceipt play(
        const std::filesystem::path& input,
        std::int64_t timelineFrame = 0
    );
    AudioPlaybackReceipt cancel(std::uint64_t expectedGeneration);
    AudioPlaybackReceipt waitForTerminal(
        std::uint64_t generation,
        std::stop_token stopToken = {}
    );
    AudioPlaybackPositionReceipt position(
        std::uint64_t expectedGeneration
    ) const;
    AudioPlaybackReceipt snapshot() const;
    AudioPlaybackReceipt close();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}
