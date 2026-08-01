#pragma once

#include "palmier/audio/pcm_format.hpp"

#include <Windows.h>

#include <cstdint>
#include <string>

namespace palmier::audio {

enum class WasapiProbeStatus {
    available,
    unavailable,
    failed,
};

enum class WasapiProbeStage {
    initializeCom,
    createEnumerator,
    defaultEndpoint,
    endpointIdInvariant,
    activateClient,
    mixFormat,
    mixFormatInvariant,
    clientProperties,
    enginePeriod,
    enginePeriodInvariant,
    initializeStream,
    bufferSize,
    setEvent,
    renderService,
    clockService,
    clockFrequency,
    probeThread,
};

struct WasapiEnvironmentProbeResult final {
    WasapiProbeStatus status{WasapiProbeStatus::failed};
    std::string stage;
    HRESULT hresult{S_OK};
    std::string endpointId;
    PcmFormat pcmFormat;
    std::uint32_t defaultPeriodFrames{};
    std::uint32_t bufferFrames{};
    std::uint64_t clockFrequency{};
};

bool isUnavailableWasapiResult(WasapiProbeStage stage, HRESULT result);

bool hasValidSharedModePeriods(
    std::uint32_t defaultPeriod,
    std::uint32_t fundamentalPeriod,
    std::uint32_t minimumPeriod,
    std::uint32_t maximumPeriod
);

WasapiEnvironmentProbeResult probeDefaultWasapiRenderEndpoint();

std::string wasapiProbeJson(const WasapiEnvironmentProbeResult& result);

}
