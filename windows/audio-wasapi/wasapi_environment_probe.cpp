#include "palmier/audio/wasapi_environment_probe.hpp"

#include "wasapi_environment_session.hpp"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <exception>
#include <iomanip>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <sstream>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

namespace palmier::audio {
namespace {

using Microsoft::WRL::ComPtr;

class ComApartment final {
public:
    ComApartment() : result_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ComApartment() {
        if (result_ == S_OK || result_ == S_FALSE) {
            CoUninitialize();
        }
    }

    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;

    HRESULT result() const { return result_; }

private:
    HRESULT result_{};
};

struct CoTaskMemory final {
    void operator()(void* value) const { CoTaskMemFree(value); }
};

struct HandleCloser final {
    void operator()(void* value) const {
        if (value != nullptr) {
            CloseHandle(value);
        }
    }
};

using WaveFormatOwner = std::unique_ptr<WAVEFORMATEX, CoTaskMemory>;
using WideStringOwner = std::unique_ptr<wchar_t, CoTaskMemory>;
using EventOwner = std::unique_ptr<void, HandleCloser>;

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

std::optional<std::string> utf8(std::wstring_view value) {
    if (value.empty()) {
        return std::string{};
    }
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        SetLastError(ERROR_ARITHMETIC_OVERFLOW);
        return std::nullopt;
    }
    const auto valueSize = static_cast<int>(value.size());
    const int required = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        valueSize,
        nullptr,
        0,
        nullptr,
        nullptr
    );
    if (required <= 0) {
        return std::nullopt;
    }
    std::string output(static_cast<std::size_t>(required), '\0');
    const int converted = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        valueSize,
        output.data(),
        required,
        nullptr,
        nullptr
    );
    if (converted != required) {
        return std::nullopt;
    }
    return output;
}

class NativeWasapiEnvironmentSession final : public WasapiEnvironmentSession {
public:
    HRESULT initializeApartment() override {
        return apartment_.result();
    }

    HRESULT createEnumerator() override {
        return CoCreateInstance(
            __uuidof(MMDeviceEnumerator),
            nullptr,
            CLSCTX_ALL,
            IID_PPV_ARGS(&enumerator_)
        );
    }

    HRESULT selectDefaultRenderEndpoint(std::string& endpointId) override {
        HRESULT result = enumerator_->GetDefaultAudioEndpoint(
            eRender,
            eMultimedia,
            &device_
        );
        if (FAILED(result)) {
            return result;
        }
        wchar_t* rawEndpointId = nullptr;
        result = device_->GetId(&rawEndpointId);
        if (FAILED(result)) {
            return result;
        }
        WideStringOwner endpointIdOwner(rawEndpointId);
        if (rawEndpointId == nullptr || rawEndpointId[0] == L'\0') {
            return E_UNEXPECTED;
        }
        SetLastError(ERROR_SUCCESS);
        const auto convertedEndpointId = utf8(rawEndpointId);
        if (!convertedEndpointId.has_value()) {
            const DWORD conversionError = GetLastError();
            return conversionError == ERROR_SUCCESS
                ? E_UNEXPECTED
                : HRESULT_FROM_WIN32(conversionError);
        }
        endpointId = *convertedEndpointId;
        return S_OK;
    }

    HRESULT activateAudioClient() override {
        return device_->Activate(
            __uuidof(IAudioClient3),
            CLSCTX_ALL,
            nullptr,
            reinterpret_cast<void**>(client_.GetAddressOf())
        );
    }

    HRESULT loadMixFormat(WasapiMixFormat& format) override {
        WAVEFORMATEX* rawFormat = nullptr;
        const HRESULT result = client_->GetMixFormat(&rawFormat);
        if (FAILED(result)) {
            return result;
        }
        format_.reset(rawFormat);
        if (!format_) {
            return E_UNEXPECTED;
        }
        format.sampleRate = format_->nSamplesPerSec;
        format.channelCount = format_->nChannels;
        format.bitsPerSample = format_->wBitsPerSample;
        return S_OK;
    }

    HRESULT setClientProperties() override {
        AudioClientProperties properties{};
        properties.cbSize = sizeof(properties);
        properties.bIsOffload = FALSE;
        properties.eCategory = AudioCategory_Media;
        properties.Options = AUDCLNT_STREAMOPTIONS_NONE;
        return client_->SetClientProperties(&properties);
    }

    HRESULT loadSharedModePeriods(WasapiSharedModePeriods& periods) override {
        return client_->GetSharedModeEnginePeriod(
            format_.get(),
            &periods.defaultPeriod,
            &periods.fundamentalPeriod,
            &periods.minimumPeriod,
            &periods.maximumPeriod
        );
    }

    HRESULT initializeSharedAudioStream(std::uint32_t periodFrames) override {
        return client_->InitializeSharedAudioStream(
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
            periodFrames,
            format_.get(),
            nullptr
        );
    }

    HRESULT loadBufferFrames(std::uint32_t& bufferFrames) override {
        return client_->GetBufferSize(&bufferFrames);
    }

    HRESULT attachRenderEvent() override {
        renderEvent_.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        if (!renderEvent_) {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        return client_->SetEventHandle(renderEvent_.get());
    }

    HRESULT loadRenderService() override {
        return client_->GetService(IID_PPV_ARGS(&renderClient_));
    }

    HRESULT loadClockService() override {
        return client_->GetService(IID_PPV_ARGS(&clock_));
    }

    HRESULT loadClockFrequency(std::uint64_t& frequency) override {
        return clock_->GetFrequency(&frequency);
    }

private:
    ComApartment apartment_;
    ComPtr<IMMDeviceEnumerator> enumerator_;
    ComPtr<IMMDevice> device_;
    EventOwner renderEvent_;
    ComPtr<IAudioClient3> client_;
    WaveFormatOwner format_;
    ComPtr<IAudioRenderClient> renderClient_;
    ComPtr<IAudioClock> clock_;
};

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
    if (format.sampleRate == 0 || format.channelCount == 0 || format.bitsPerSample == 0) {
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
    probe.sampleRate = format.sampleRate;
    probe.channelCount = format.channelCount;
    probe.bitsPerSample = format.bitsPerSample;
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
                    NativeWasapiEnvironmentSession session;
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
           << "\",\"sampleRate\":" << result.sampleRate
           << ",\"channelCount\":" << result.channelCount
           << ",\"bitsPerSample\":" << result.bitsPerSample
           << ",\"defaultPeriodFrames\":" << result.defaultPeriodFrames
           << ",\"bufferFrames\":" << result.bufferFrames
           << ",\"clockFrequency\":" << result.clockFrequency << '}';
    return output.str();
}

}
