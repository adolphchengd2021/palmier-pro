#include "wasapi_native_stream.hpp"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <limits>
#include <optional>
#include <string_view>

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

}

class WasapiNativeStream::Impl final {
public:
    ComApartment apartment;
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    EventOwner renderEvent;
    ComPtr<IAudioClient3> client;
    WaveFormatOwner format;
    ComPtr<IAudioRenderClient> renderClient;
    ComPtr<IAudioClock> clock;
};

WasapiNativeStream::WasapiNativeStream() : impl_(std::make_unique<Impl>()) {}

WasapiNativeStream::~WasapiNativeStream() {
    close();
}

HRESULT waitForWasapiRenderEvent(
    HANDLE renderEvent,
    std::stop_token stopToken,
    std::uint32_t timeoutMilliseconds,
    HANDLE registeredEvent
) noexcept {
    if (renderEvent == nullptr) {
        return E_INVALIDARG;
    }
    EventOwner cancelEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!cancelEvent) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    std::stop_callback cancellation(stopToken, [&cancelEvent]() noexcept {
        SetEvent(cancelEvent.get());
    });
    if (registeredEvent != nullptr && !SetEvent(registeredEvent)) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    const HANDLE handles[] = {renderEvent, cancelEvent.get()};
    const DWORD result = WaitForMultipleObjects(2, handles, FALSE, timeoutMilliseconds);
    if (result == WAIT_OBJECT_0) {
        return S_OK;
    }
    if (result == WAIT_OBJECT_0 + 1) {
        return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    }
    if (result == WAIT_TIMEOUT) {
        return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }
    return HRESULT_FROM_WIN32(GetLastError());
}

HRESULT WasapiNativeStream::initializeApartment() {
    return impl_->apartment.result();
}

HRESULT WasapiNativeStream::createEnumerator() {
    return CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        IID_PPV_ARGS(&impl_->enumerator)
    );
}

HRESULT WasapiNativeStream::selectDefaultRenderEndpoint(std::string& endpointId) {
    HRESULT result = impl_->enumerator->GetDefaultAudioEndpoint(
        eRender,
        eMultimedia,
        &impl_->device
    );
    if (FAILED(result)) {
        return result;
    }
    wchar_t* rawEndpointId = nullptr;
    result = impl_->device->GetId(&rawEndpointId);
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

HRESULT WasapiNativeStream::activateAudioClient() {
    return impl_->device->Activate(
        __uuidof(IAudioClient3),
        CLSCTX_ALL,
        nullptr,
        reinterpret_cast<void**>(impl_->client.GetAddressOf())
    );
}

HRESULT WasapiNativeStream::loadMixFormat(WasapiMixFormat& format) {
    WAVEFORMATEX* rawFormat = nullptr;
    const HRESULT result = impl_->client->GetMixFormat(&rawFormat);
    if (FAILED(result)) {
        return result;
    }
    impl_->format.reset(rawFormat);
    if (!impl_->format) {
        return E_UNEXPECTED;
    }
    format.sampleRate = impl_->format->nSamplesPerSec;
    format.channelCount = impl_->format->nChannels;
    format.bitsPerSample = impl_->format->wBitsPerSample;
    format.blockAlign = impl_->format->nBlockAlign;
    return S_OK;
}

HRESULT WasapiNativeStream::setClientProperties() {
    AudioClientProperties properties{};
    properties.cbSize = sizeof(properties);
    properties.bIsOffload = FALSE;
    properties.eCategory = AudioCategory_Media;
    properties.Options = AUDCLNT_STREAMOPTIONS_NONE;
    return impl_->client->SetClientProperties(&properties);
}

HRESULT WasapiNativeStream::loadSharedModePeriods(WasapiSharedModePeriods& periods) {
    return impl_->client->GetSharedModeEnginePeriod(
        impl_->format.get(),
        &periods.defaultPeriod,
        &periods.fundamentalPeriod,
        &periods.minimumPeriod,
        &periods.maximumPeriod
    );
}

HRESULT WasapiNativeStream::initializeSharedAudioStream(std::uint32_t periodFrames) {
    return impl_->client->InitializeSharedAudioStream(
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        periodFrames,
        impl_->format.get(),
        nullptr
    );
}

HRESULT WasapiNativeStream::loadBufferFrames(std::uint32_t& bufferFrames) {
    return impl_->client->GetBufferSize(&bufferFrames);
}

HRESULT WasapiNativeStream::attachRenderEvent() {
    impl_->renderEvent.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!impl_->renderEvent) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    return impl_->client->SetEventHandle(impl_->renderEvent.get());
}

HRESULT WasapiNativeStream::loadRenderService() {
    return impl_->client->GetService(IID_PPV_ARGS(&impl_->renderClient));
}

HRESULT WasapiNativeStream::loadClockService() {
    return impl_->client->GetService(IID_PPV_ARGS(&impl_->clock));
}

HRESULT WasapiNativeStream::loadClockFrequency(std::uint64_t& frequency) {
    return impl_->clock->GetFrequency(&frequency);
}

HRESULT WasapiNativeStream::waitForRenderEvent(
    std::stop_token stopToken,
    std::uint32_t timeoutMilliseconds
) noexcept {
    if (!impl_->renderEvent) {
        return E_UNEXPECTED;
    }
    return waitForWasapiRenderEvent(
        impl_->renderEvent.get(),
        stopToken,
        timeoutMilliseconds
    );
}

HRESULT WasapiNativeStream::loadCurrentPadding(std::uint32_t& paddingFrames) noexcept {
    return impl_->client->GetCurrentPadding(&paddingFrames);
}

HRESULT WasapiNativeStream::acquireBuffer(
    std::uint32_t frameCount,
    std::byte*& data
) noexcept {
    BYTE* rawData = nullptr;
    const HRESULT result = impl_->renderClient->GetBuffer(frameCount, &rawData);
    data = reinterpret_cast<std::byte*>(rawData);
    return result;
}

HRESULT WasapiNativeStream::releaseBuffer(
    std::uint32_t frameCount,
    DWORD flags
) noexcept {
    return impl_->renderClient->ReleaseBuffer(frameCount, flags);
}

HRESULT WasapiNativeStream::start() noexcept {
    return impl_->client->Start();
}

HRESULT WasapiNativeStream::loadClockPosition(WasapiClockReading& reading) noexcept {
    const HRESULT result = impl_->clock->GetPosition(
        &reading.devicePosition,
        &reading.qpc100Nanoseconds
    );
    reading.precisionDegraded = result == S_FALSE;
    return result;
}

HRESULT WasapiNativeStream::stop() noexcept {
    return impl_->client->Stop();
}

HRESULT WasapiNativeStream::reset() noexcept {
    return impl_->client->Reset();
}

HRESULT WasapiNativeStream::close() noexcept {
    impl_->renderClient.Reset();
    impl_->clock.Reset();
    impl_->client.Reset();
    impl_->format.reset();
    impl_->renderEvent.reset();
    impl_->device.Reset();
    impl_->enumerator.Reset();
    return S_OK;
}

}
