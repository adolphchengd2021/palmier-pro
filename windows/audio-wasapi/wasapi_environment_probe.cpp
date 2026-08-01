#include "palmier/audio/wasapi_environment_probe.hpp"

#include "wasapi_environment_session.hpp"
#include "wasapi_native_stream.hpp"

#include <audioclient.h>
#include <mmdeviceapi.h>

#include <exception>
#include <iomanip>
#include <new>
#include <optional>
#include <sstream>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

namespace palmier::audio {
namespace {

WasapiEnvironmentProbeResult terminalResult(
    WasapiProbeStage stage,
    HRESULT result
) {
    const auto stageName = [stage] {
        switch (stage) {
        case WasapiProbeStage::initializeCom: return "initialize-com";
        case WasapiProbeStage::createEnumerator: return "create-enumerator";
        case WasapiProbeStage::defaultEndpoint: return "default-endpoint";
        case WasapiProbeStage::endpointIdInvariant: return "endpoint-id-invariant";
        case WasapiProbeStage::activateClient: return "activate-client";
        case WasapiProbeStage::mixFormat: return "mix-format";
        case WasapiProbeStage::mixFormatInvariant: return "mix-format-invariant";
        case WasapiProbeStage::clientProperties: return "client-properties";
        case WasapiProbeStage::enginePeriod: return "engine-period";
        case WasapiProbeStage::enginePeriodInvariant: return "engine-period-invariant";
        case WasapiProbeStage::initializeStream: return "initialize-stream";
        case WasapiProbeStage::bufferSize: return "buffer-size";
        case WasapiProbeStage::setEvent: return "set-event";
        case WasapiProbeStage::renderService: return "render-service";
        case WasapiProbeStage::clockService: return "clock-service";
        case WasapiProbeStage::clockFrequency: return "clock-frequency";
        case WasapiProbeStage::probeThread: return "probe-thread";
        }
        return "probe-thread";
    }();
    return {
        isUnavailableWasapiResult(stage, result)
            ? WasapiProbeStatus::unavailable
            : WasapiProbeStatus::failed,
        stageName,
        result,
    };
}

std::string statusName(WasapiProbeStatus status) {
    switch (status) {
    case WasapiProbeStatus::available:
        return "available";
    case WasapiProbeStatus::unavailable:
        return "unavailable";
    case WasapiProbeStatus::failed:
        return "failed";
    }
    return "failed";
}

std::string_view encodingName(PcmSampleEncoding encoding) {
    switch (encoding) {
    case PcmSampleEncoding::unknown: return "unknown";
    case PcmSampleEncoding::integer: return "integer";
    case PcmSampleEncoding::ieeeFloat: return "ieee-float";
    }
    return "unknown";
}

std::string jsonEscape(std::string_view value) {
    std::ostringstream output;
    for (const unsigned char character : value) {
        switch (character) {
        case '\\': output << "\\\\"; break;
        case '"': output << "\\\""; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (character < 0x20) {
                output << "\\u"
                       << std::hex << std::setw(4) << std::setfill('0')
                       << static_cast<unsigned int>(character)
                       << std::dec;
            } else {
                output << character;
            }
        }
    }
    return output.str();
}

}

bool isUnavailableWasapiResult(WasapiProbeStage stage, HRESULT result) {
    if (result == E_NOTFOUND) {
        return stage == WasapiProbeStage::defaultEndpoint;
    }
    if (result == AUDCLNT_E_DEVICE_IN_USE
        || result == AUDCLNT_E_ENDPOINT_CREATE_FAILED) {
        return stage == WasapiProbeStage::initializeStream;
    }
    if (result != AUDCLNT_E_SERVICE_NOT_RUNNING
        && result != AUDCLNT_E_DEVICE_INVALIDATED
        && result != AUDCLNT_E_RESOURCES_INVALIDATED) {
        return false;
    }
    switch (stage) {
    case WasapiProbeStage::defaultEndpoint:
    case WasapiProbeStage::activateClient:
    case WasapiProbeStage::mixFormat:
    case WasapiProbeStage::clientProperties:
    case WasapiProbeStage::enginePeriod:
    case WasapiProbeStage::initializeStream:
    case WasapiProbeStage::bufferSize:
    case WasapiProbeStage::setEvent:
    case WasapiProbeStage::renderService:
    case WasapiProbeStage::clockService:
    case WasapiProbeStage::clockFrequency:
        return true;
    case WasapiProbeStage::initializeCom:
    case WasapiProbeStage::createEnumerator:
    case WasapiProbeStage::endpointIdInvariant:
    case WasapiProbeStage::mixFormatInvariant:
    case WasapiProbeStage::enginePeriodInvariant:
    case WasapiProbeStage::probeThread:
        return false;
    }
    return false;
}

bool hasValidSharedModePeriods(
    std::uint32_t defaultPeriod,
    std::uint32_t fundamentalPeriod,
    std::uint32_t minimumPeriod,
    std::uint32_t maximumPeriod
) {
    return fundamentalPeriod > 0
        && fundamentalPeriod <= minimumPeriod
        && minimumPeriod <= defaultPeriod
        && defaultPeriod <= maximumPeriod
        && minimumPeriod % fundamentalPeriod == 0
        && defaultPeriod % fundamentalPeriod == 0
        && maximumPeriod % fundamentalPeriod == 0;
}

WasapiEnvironmentProbeResult runWasapiEnvironmentProbe(
    WasapiEnvironmentSession& session
) {
    HRESULT result = session.initializeApartment();
    if (FAILED(result)) {
        return terminalResult(WasapiProbeStage::initializeCom, result);
    }
    result = session.createEnumerator();
    if (FAILED(result)) {
        return terminalResult(WasapiProbeStage::createEnumerator, result);
    }

    WasapiEnvironmentProbeResult probe;
    result = session.selectDefaultRenderEndpoint(probe.endpointId);
    if (FAILED(result)) {
        return terminalResult(WasapiProbeStage::defaultEndpoint, result);
    }
    if (probe.endpointId.empty()) {
        return terminalResult(WasapiProbeStage::endpointIdInvariant, E_UNEXPECTED);
    }
    result = session.activateAudioClient();
    if (FAILED(result)) {
        return terminalResult(WasapiProbeStage::activateClient, result);
    }

    WasapiMixFormat format;
    result = session.loadMixFormat(format);
    if (FAILED(result)) {
        return terminalResult(WasapiProbeStage::mixFormat, result);
    }
    if (!isValidPcmFormat(format)) {
        return terminalResult(WasapiProbeStage::mixFormatInvariant, E_UNEXPECTED);
    }
    result = session.setClientProperties();
    if (FAILED(result)) {
        return terminalResult(WasapiProbeStage::clientProperties, result);
    }

    WasapiSharedModePeriods periods;
    result = session.loadSharedModePeriods(periods);
    if (FAILED(result)) {
        return terminalResult(WasapiProbeStage::enginePeriod, result);
    }
    if (!hasValidSharedModePeriods(
            periods.defaultPeriod,
            periods.fundamentalPeriod,
            periods.minimumPeriod,
            periods.maximumPeriod
        )) {
        return terminalResult(WasapiProbeStage::enginePeriodInvariant, E_UNEXPECTED);
    }
    result = session.initializeSharedAudioStream(periods.defaultPeriod);
    if (FAILED(result)) {
        return terminalResult(WasapiProbeStage::initializeStream, result);
    }

    result = session.loadBufferFrames(probe.bufferFrames);
    if (FAILED(result) || probe.bufferFrames == 0) {
        return terminalResult(
            WasapiProbeStage::bufferSize,
            FAILED(result) ? result : E_UNEXPECTED
        );
    }
    result = session.attachRenderEvent();
    if (FAILED(result)) {
        return terminalResult(WasapiProbeStage::setEvent, result);
    }
    result = session.loadRenderService();
    if (FAILED(result)) {
        return terminalResult(WasapiProbeStage::renderService, result);
    }
    result = session.loadClockService();
    if (FAILED(result)) {
        return terminalResult(WasapiProbeStage::clockService, result);
    }
    result = session.loadClockFrequency(probe.clockFrequency);
    if (FAILED(result) || probe.clockFrequency == 0) {
        return terminalResult(
            WasapiProbeStage::clockFrequency,
            FAILED(result) ? result : E_UNEXPECTED
        );
    }

    probe.status = WasapiProbeStatus::available;
    probe.stage = "ready-without-start";
    probe.hresult = S_OK;
    probe.pcmFormat = format;
    probe.defaultPeriodFrames = periods.defaultPeriod;
    return probe;
}

WasapiEnvironmentProbeResult probeDefaultWasapiRenderEndpoint() {
    std::optional<WasapiEnvironmentProbeResult> probe;
    std::exception_ptr exception;
    try {
        {
            std::jthread worker([&probe, &exception] {
                try {
                    WasapiNativeStream session;
                    probe = runWasapiEnvironmentProbe(session);
                } catch (...) {
                    exception = std::current_exception();
                }
            });
        }
        if (exception) {
            std::rethrow_exception(exception);
        }
    } catch (const std::bad_alloc&) {
        return terminalResult(WasapiProbeStage::probeThread, E_OUTOFMEMORY);
    } catch (const std::system_error&) {
        return terminalResult(WasapiProbeStage::probeThread, E_FAIL);
    } catch (...) {
        return terminalResult(WasapiProbeStage::probeThread, E_UNEXPECTED);
    }
    if (!probe.has_value()) {
        return terminalResult(WasapiProbeStage::probeThread, E_UNEXPECTED);
    }
    return std::move(*probe);
}

std::string wasapiProbeJson(const WasapiEnvironmentProbeResult& result) {
    std::ostringstream output;
    output << "{\"status\":\"" << statusName(result.status)
           << "\",\"stage\":\"" << jsonEscape(result.stage)
           << "\",\"hresult\":\"0x"
           << std::hex << std::uppercase << std::setw(8) << std::setfill('0')
           << static_cast<std::uint32_t>(result.hresult)
           << std::dec << "\",\"endpointId\":\""
           << jsonEscape(result.endpointId)
           << "\",\"sampleRate\":" << result.pcmFormat.sampleRate
           << ",\"channelCount\":" << result.pcmFormat.channelCount
           << ",\"containerBitsPerSample\":"
           << result.pcmFormat.containerBitsPerSample
           << ",\"validBitsPerSample\":"
           << result.pcmFormat.validBitsPerSample
           << ",\"blockAlign\":" << result.pcmFormat.blockAlign
           << ",\"channelMask\":" << result.pcmFormat.channelMask
           << ",\"sampleEncoding\":\""
           << encodingName(result.pcmFormat.encoding) << "\""
           << ",\"interleaved\":"
           << (result.pcmFormat.interleaved ? "true" : "false")
           << ",\"defaultPeriodFrames\":" << result.defaultPeriodFrames
           << ",\"bufferFrames\":" << result.bufferFrames
           << ",\"clockFrequency\":" << result.clockFrequency << '}';
    return output.str();
}

}
