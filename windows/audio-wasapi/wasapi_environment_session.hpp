#pragma once

#include "palmier/audio/wasapi_environment_probe.hpp"

#include <Windows.h>

#include <cstdint>
#include <string>

namespace palmier::audio {

struct WasapiSharedModePeriods final {
    std::uint32_t defaultPeriod{};
    std::uint32_t fundamentalPeriod{};
    std::uint32_t minimumPeriod{};
    std::uint32_t maximumPeriod{};
};

using WasapiMixFormat = PcmFormat;

class WasapiEnvironmentSession {
public:
    virtual ~WasapiEnvironmentSession() = default;

    virtual HRESULT initializeApartment() = 0;
    virtual HRESULT createEnumerator() = 0;
    virtual HRESULT selectDefaultRenderEndpoint(std::string& endpointId) = 0;
    virtual HRESULT activateAudioClient() = 0;
    virtual HRESULT loadMixFormat(WasapiMixFormat& format) = 0;
    virtual HRESULT setClientProperties() = 0;
    virtual HRESULT loadSharedModePeriods(WasapiSharedModePeriods& periods) = 0;
    virtual HRESULT initializeSharedAudioStream(std::uint32_t periodFrames) = 0;
    virtual HRESULT loadBufferFrames(std::uint32_t& bufferFrames) = 0;
    virtual HRESULT attachRenderEvent() = 0;
    virtual HRESULT loadRenderService() = 0;
    virtual HRESULT loadClockService() = 0;
    virtual HRESULT loadClockFrequency(std::uint64_t& frequency) = 0;
};

WasapiEnvironmentProbeResult runWasapiEnvironmentProbe(
    WasapiEnvironmentSession& session
);

}
