#pragma once

#include "palmier/audio/audio_clock.hpp"
#include "palmier/audio/pcm_format.hpp"

#include <Windows.h>

#include <cstdint>
#include <string>

namespace palmier::audio {

enum class WasapiOutputState {
    ready,
    primed,
    running,
    stopped,
    invalidated,
    failed,
    completed,
    closed,
};

enum class WasapiOutputOperation {
    start,
    render,
    pause,
    reset,
    close,
};

enum class WasapiOutputOutcome {
    changed,
    noOp,
    cancelled,
    refused,
    failed,
    invalidated,
};

enum class WasapiOutputStage {
    none,
    waitForEvent,
    currentPadding,
    paddingInvariant,
    acquireBuffer,
    copyBuffer,
    releaseBuffer,
    startClient,
    clockPosition,
    stopClient,
    resetClient,
    closeBackend,
};

struct WasapiOutputConfig final {
    std::uint32_t bufferFrames{};
    PcmFormat pcmFormat;
    std::uint64_t clockFrequency{};
    std::uint64_t generation{1};
};

struct WasapiOutputReceipt final {
    WasapiOutputOperation operation{WasapiOutputOperation::render};
    WasapiOutputOutcome outcome{WasapiOutputOutcome::failed};
    WasapiOutputState previousState{WasapiOutputState::failed};
    WasapiOutputState currentState{WasapiOutputState::failed};
    WasapiOutputStage stage{WasapiOutputStage::none};
    HRESULT hresult{S_OK};
    std::uint64_t generation{};
    std::uint32_t bufferFrames{};
    std::uint32_t paddingFrames{};
    std::uint32_t availableFrames{};
    std::uint32_t requestedFrames{};
    std::uint32_t mediaFrames{};
    std::uint32_t silenceFrames{};
    std::uint32_t releasedFrames{};
    std::uint64_t underrunEventsDelta{};
    std::uint64_t underrunEventsTotal{};
    bool lateCancellation{};
    bool hasClockSample{};
    AudioClockSample clockSample{};
};

enum class WasapiSilentOutputStatus {
    available,
    unavailable,
    failed,
};

struct WasapiSilentOutputResult final {
    WasapiSilentOutputStatus status{WasapiSilentOutputStatus::failed};
    std::string stage;
    HRESULT hresult{S_OK};
    std::uint64_t generation{};
    std::uint32_t primedFrames{};
    std::uint32_t eventFrames{};
    bool clockSampled{};
};

WasapiSilentOutputResult probeDefaultWasapiSilentOutput();
std::string wasapiSilentOutputJson(const WasapiSilentOutputResult& result);

}
