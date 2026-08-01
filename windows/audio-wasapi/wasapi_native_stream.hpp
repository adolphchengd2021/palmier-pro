#pragma once

#include "wasapi_environment_session.hpp"
#include "wasapi_output_backend.hpp"

#include <mmreg.h>

#include <memory>

namespace palmier::audio {

HRESULT parseWasapiMixFormat(
    const WAVEFORMATEX& source,
    PcmFormat& destination
) noexcept;

HRESULT waitForWasapiRenderEvent(
    HANDLE renderEvent,
    std::stop_token stopToken,
    std::uint32_t timeoutMilliseconds,
    HANDLE registeredEvent = nullptr
) noexcept;

class WasapiNativeStream final :
    public WasapiEnvironmentSession,
    public WasapiOutputBackend {
public:
    WasapiNativeStream();
    ~WasapiNativeStream() override;

    WasapiNativeStream(const WasapiNativeStream&) = delete;
    WasapiNativeStream& operator=(const WasapiNativeStream&) = delete;

    HRESULT initializeApartment() override;
    HRESULT createEnumerator() override;
    HRESULT selectDefaultRenderEndpoint(std::string& endpointId) override;
    HRESULT activateAudioClient() override;
    HRESULT loadMixFormat(WasapiMixFormat& format) override;
    HRESULT setClientProperties() override;
    HRESULT loadSharedModePeriods(WasapiSharedModePeriods& periods) override;
    HRESULT initializeSharedAudioStream(std::uint32_t periodFrames) override;
    HRESULT loadBufferFrames(std::uint32_t& bufferFrames) override;
    HRESULT attachRenderEvent() override;
    HRESULT loadRenderService() override;
    HRESULT loadClockService() override;
    HRESULT loadClockFrequency(std::uint64_t& frequency) override;

    HRESULT waitForRenderEvent(
        std::stop_token stopToken,
        std::uint32_t timeoutMilliseconds
    ) noexcept override;
    HRESULT loadCurrentPadding(std::uint32_t& paddingFrames) noexcept override;
    HRESULT acquireBuffer(
        std::uint32_t frameCount,
        std::byte*& data
    ) noexcept override;
    HRESULT releaseBuffer(std::uint32_t frameCount, DWORD flags) noexcept override;
    HRESULT start() noexcept override;
    HRESULT loadClockPosition(WasapiClockReading& reading) noexcept override;
    HRESULT stop() noexcept override;
    HRESULT reset() noexcept override;
    HRESULT close() noexcept override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}
