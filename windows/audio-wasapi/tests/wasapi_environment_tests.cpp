#include "palmier/audio/wasapi_environment_probe.hpp"

#include "wasapi_environment_session.hpp"
#include "wasapi_native_stream.hpp"

#include <audioclient.h>
#include <initguid.h>
#include <ksmedia.h>
#include <mmreg.h>
#include <mmdeviceapi.h>

#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using palmier::audio::WasapiEnvironmentProbeResult;
using palmier::audio::WasapiEnvironmentSession;
using palmier::audio::WasapiMixFormat;
using palmier::audio::WasapiProbeStatus;
using palmier::audio::WasapiProbeStage;
using palmier::audio::WasapiSharedModePeriods;
using palmier::audio::PcmSampleEncoding;
using palmier::audio::hasValidSharedModePeriods;
using palmier::audio::isUnavailableWasapiResult;
using palmier::audio::runWasapiEnvironmentProbe;
using palmier::audio::wasapiProbeJson;
using palmier::audio::parseWasapiMixFormat;

class ScriptedWasapiSession final : public WasapiEnvironmentSession {
public:
    HRESULT failResult{S_OK};
    std::string failStage;
    std::vector<std::string> calls;
    std::uint32_t initializedPeriod{};

    HRESULT initializeApartment() override { return record("initialize-com"); }
    HRESULT createEnumerator() override { return record("create-enumerator"); }

    HRESULT selectDefaultRenderEndpoint(std::string& endpointId) override {
        const HRESULT result = record("default-endpoint");
        if (SUCCEEDED(result)) {
            endpointId = "test-endpoint";
        }
        return result;
    }

    HRESULT activateAudioClient() override { return record("activate-client"); }

    HRESULT loadMixFormat(WasapiMixFormat& format) override {
        const HRESULT result = record("mix-format");
        if (SUCCEEDED(result)) {
            format = {
                48'000,
                2,
                32,
                32,
                8,
                0x3,
                PcmSampleEncoding::ieeeFloat,
                true,
            };
        }
        return result;
    }

    HRESULT setClientProperties() override { return record("client-properties"); }

    HRESULT loadSharedModePeriods(WasapiSharedModePeriods& periods) override {
        const HRESULT result = record("engine-period");
        if (SUCCEEDED(result)) {
            periods = {480, 48, 96, 960};
        }
        return result;
    }

    HRESULT initializeSharedAudioStream(std::uint32_t periodFrames) override {
        initializedPeriod = periodFrames;
        return record("initialize-stream");
    }

    HRESULT loadBufferFrames(std::uint32_t& bufferFrames) override {
        const HRESULT result = record("buffer-size");
        if (SUCCEEDED(result)) {
            bufferFrames = 960;
        }
        return result;
    }

    HRESULT attachRenderEvent() override { return record("set-event"); }
    HRESULT loadRenderService() override { return record("render-service"); }
    HRESULT loadClockService() override { return record("clock-service"); }

    HRESULT loadClockFrequency(std::uint64_t& frequency) override {
        const HRESULT result = record("clock-frequency");
        if (SUCCEEDED(result)) {
            frequency = 48'000;
        }
        return result;
    }

private:
    HRESULT record(std::string_view stage) {
        calls.emplace_back(stage);
        return stage == failStage ? failResult : S_OK;
    }
};

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void classifiesOnlyKnownEnvironmentFailures() {
    require(
        isUnavailableWasapiResult(WasapiProbeStage::defaultEndpoint, E_NOTFOUND),
        "missing default endpoint was not unavailable"
    );
    require(
        isUnavailableWasapiResult(
            WasapiProbeStage::defaultEndpoint,
            AUDCLNT_E_SERVICE_NOT_RUNNING
        ),
        "stopped audio service was not unavailable"
    );
    require(
        isUnavailableWasapiResult(
            WasapiProbeStage::clockFrequency,
            AUDCLNT_E_DEVICE_INVALIDATED
        ),
        "invalidated device was not unavailable"
    );
    require(
        isUnavailableWasapiResult(
            WasapiProbeStage::bufferSize,
            AUDCLNT_E_RESOURCES_INVALIDATED
        ),
        "invalidated resources were not unavailable"
    );
    require(
        isUnavailableWasapiResult(
            WasapiProbeStage::initializeStream,
            AUDCLNT_E_DEVICE_IN_USE
        ),
        "device competition was not unavailable"
    );
    require(
        isUnavailableWasapiResult(
            WasapiProbeStage::initializeStream,
            AUDCLNT_E_ENDPOINT_CREATE_FAILED
        ),
        "endpoint creation failure was not unavailable"
    );
    require(
        !isUnavailableWasapiResult(WasapiProbeStage::createEnumerator, E_NOTFOUND),
        "wrong-stage not-found was hidden"
    );
    require(
        !isUnavailableWasapiResult(
            WasapiProbeStage::activateClient,
            AUDCLNT_E_DEVICE_IN_USE
        ),
        "wrong-stage device competition was hidden"
    );
    require(
        !isUnavailableWasapiResult(
            WasapiProbeStage::defaultEndpoint,
            AUDCLNT_E_ENDPOINT_CREATE_FAILED
        ),
        "wrong-stage endpoint creation was hidden"
    );
    require(
        !isUnavailableWasapiResult(WasapiProbeStage::activateClient, E_ACCESSDENIED),
        "access denied was hidden"
    );
    require(
        !isUnavailableWasapiResult(WasapiProbeStage::initializeStream, E_INVALIDARG),
        "invalid argument was hidden"
    );
    require(
        !isUnavailableWasapiResult(WasapiProbeStage::activateClient, E_NOINTERFACE),
        "missing interface was hidden"
    );
    require(
        !isUnavailableWasapiResult(WasapiProbeStage::initializeCom, RPC_E_CHANGED_MODE),
        "COM mode error was hidden"
    );
}

void executesTheNoStartSetupInOrder() {
    ScriptedWasapiSession session;
    const auto result = runWasapiEnvironmentProbe(session);
    require(result.status == WasapiProbeStatus::available, "success was not available");
    require(result.stage == "ready-without-start", "wrong success stage");
    require(result.endpointId == "test-endpoint", "endpoint ID missing");
    require(result.pcmFormat.sampleRate == 48'000, "sample rate missing");
    require(result.pcmFormat.blockAlign == 8, "block align missing");
    require(
        result.pcmFormat.encoding == PcmSampleEncoding::ieeeFloat,
        "sample encoding missing"
    );
    require(result.bufferFrames == 960, "buffer size missing");
    require(result.clockFrequency == 48'000, "clock frequency missing");
    require(session.initializedPeriod == 480, "default period was not selected");
    require(
        session.calls == std::vector<std::string>{
            "initialize-com",
            "create-enumerator",
            "default-endpoint",
            "activate-client",
            "mix-format",
            "client-properties",
            "engine-period",
            "initialize-stream",
            "buffer-size",
            "set-event",
            "render-service",
            "clock-service",
            "clock-frequency",
        },
        "WASAPI setup order changed"
    );
}

void preservesExternalAndImplementationFailures() {
    ScriptedWasapiSession unavailable;
    unavailable.failStage = "initialize-stream";
    unavailable.failResult = AUDCLNT_E_ENDPOINT_CREATE_FAILED;
    const auto unavailableResult = runWasapiEnvironmentProbe(unavailable);
    require(
        unavailableResult.status == WasapiProbeStatus::unavailable,
        "endpoint loss was not unavailable"
    );
    require(unavailableResult.stage == "initialize-stream", "wrong unavailable stage");
    require(unavailable.calls.back() == "initialize-stream", "failure did not short circuit");

    ScriptedWasapiSession failed;
    failed.failStage = "activate-client";
    failed.failResult = E_ACCESSDENIED;
    const auto failedResult = runWasapiEnvironmentProbe(failed);
    require(failedResult.status == WasapiProbeStatus::failed, "access failure was hidden");
    require(failedResult.stage == "activate-client", "wrong failure stage");
    require(failed.calls.back() == "activate-client", "failure did not short circuit");
}

void validatesSharedModePeriodInvariants() {
    require(hasValidSharedModePeriods(480, 48, 96, 960), "valid periods rejected");
    require(!hasValidSharedModePeriods(480, 0, 96, 960), "zero fundamental accepted");
    require(!hasValidSharedModePeriods(480, 48, 47, 960), "minimum below fundamental accepted");
    require(!hasValidSharedModePeriods(480, 48, 528, 960), "default below minimum accepted");
    require(!hasValidSharedModePeriods(480, 48, 96, 479), "maximum below default accepted");
    require(!hasValidSharedModePeriods(480, 48, 97, 960), "nonmultiple minimum accepted");
    require(!hasValidSharedModePeriods(481, 48, 96, 960), "nonmultiple default accepted");
    require(!hasValidSharedModePeriods(480, 48, 96, 961), "nonmultiple maximum accepted");
}

void parsesExactPcmAndExtensibleMixFormats() {
    WAVEFORMATEX pcm{};
    pcm.wFormatTag = WAVE_FORMAT_PCM;
    pcm.nChannels = 1;
    pcm.nSamplesPerSec = 24'000;
    pcm.nAvgBytesPerSec = 48'000;
    pcm.nBlockAlign = 2;
    pcm.wBitsPerSample = 16;
    WasapiMixFormat parsed;
    require(SUCCEEDED(parseWasapiMixFormat(pcm, parsed)), "PCM16 was rejected");
    require(parsed.encoding == PcmSampleEncoding::integer, "PCM encoding changed");
    require(parsed.validBitsPerSample == 16, "PCM valid bits changed");
    require(parsed.channelMask == 0, "legacy PCM invented a channel mask");

    pcm.nChannels = 6;
    pcm.nAvgBytesPerSec = 288'000;
    pcm.nBlockAlign = 12;
    require(
        parseWasapiMixFormat(pcm, parsed) == E_UNEXPECTED,
        "unidentified legacy multichannel layout was accepted"
    );

    WAVEFORMATEXTENSIBLE extensible{};
    extensible.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    extensible.Format.nChannels = 2;
    extensible.Format.nSamplesPerSec = 48'000;
    extensible.Format.nAvgBytesPerSec = 384'000;
    extensible.Format.nBlockAlign = 8;
    extensible.Format.wBitsPerSample = 32;
    extensible.Format.cbSize = static_cast<WORD>(
        sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)
    );
    extensible.Samples.wValidBitsPerSample = 32;
    extensible.dwChannelMask = 0x3;
    extensible.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    require(
        SUCCEEDED(parseWasapiMixFormat(extensible.Format, parsed)),
        "extensible float32 was rejected"
    );
    require(parsed.encoding == PcmSampleEncoding::ieeeFloat, "float encoding changed");
    require(parsed.channelMask == 0x3, "channel mask changed");

    extensible.dwChannelMask = 0x4;
    require(
        parseWasapiMixFormat(extensible.Format, parsed) == E_UNEXPECTED,
        "inconsistent channel mask was accepted"
    );
    extensible.dwChannelMask = 0x3;
    extensible.SubFormat = GUID_NULL;
    require(
        parseWasapiMixFormat(extensible.Format, parsed)
            == AUDCLNT_E_UNSUPPORTED_FORMAT,
        "unknown extensible subformat was guessed"
    );
}

void emitsMachineReadableTerminalState() {
    WasapiEnvironmentProbeResult result;
    result.status = WasapiProbeStatus::unavailable;
    result.stage = "default-endpoint";
    result.hresult = E_NOTFOUND;
    require(
        wasapiProbeJson(result)
            == "{\"status\":\"unavailable\",\"stage\":\"default-endpoint\","
               "\"hresult\":\"0x80070490\",\"endpointId\":\"\","
               "\"sampleRate\":0,\"channelCount\":0,"
               "\"containerBitsPerSample\":0,\"validBitsPerSample\":0,"
               "\"blockAlign\":0,\"channelMask\":0,"
               "\"sampleEncoding\":\"unknown\",\"interleaved\":true,"
               "\"defaultPeriodFrames\":0,\"bufferFrames\":0,"
               "\"clockFrequency\":0}",
        "unavailable JSON changed"
    );

    result.status = WasapiProbeStatus::available;
    result.stage = "ready-without-start";
    result.hresult = S_OK;
    result.endpointId = "speaker\\\"id";
    result.pcmFormat = {
        48'000,
        2,
        32,
        32,
        8,
        0x3,
        PcmSampleEncoding::ieeeFloat,
        true,
    };
    result.defaultPeriodFrames = 480;
    result.bufferFrames = 960;
    result.clockFrequency = 48'000;
    require(
        wasapiProbeJson(result)
            == "{\"status\":\"available\",\"stage\":\"ready-without-start\","
               "\"hresult\":\"0x00000000\",\"endpointId\":\"speaker\\\\\\\"id\","
               "\"sampleRate\":48000,\"channelCount\":2,"
               "\"containerBitsPerSample\":32,\"validBitsPerSample\":32,"
               "\"blockAlign\":8,\"channelMask\":3,"
               "\"sampleEncoding\":\"ieee-float\",\"interleaved\":true,"
               "\"defaultPeriodFrames\":480,\"bufferFrames\":960,"
               "\"clockFrequency\":48000}",
        "available JSON changed"
    );
}

}

int main() {
    try {
        classifiesOnlyKnownEnvironmentFailures();
        validatesSharedModePeriodInvariants();
        parsesExactPcmAndExtensibleMixFormats();
        executesTheNoStartSetupInOrder();
        preservesExternalAndImplementationFailures();
        emitsMachineReadableTerminalState();
        std::cout << "WASAPI environment contract tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
