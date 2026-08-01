#include "palmier/render/d3d11_preview_surface.hpp"
#include "internal/d3d11_preview_surface_testing.hpp"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace palmier::render {
namespace {

using Microsoft::WRL::ComPtr;

constexpr auto previewShaderSource = R"(
cbuffer PreviewConstants : register(b0) {
    float2 contentOrigin;
    float2 contentSize;
};

Texture2D<float4> previewTexture : register(t0);
SamplerState previewSampler : register(s0);

struct VertexOutput {
    float4 position : SV_Position;
};

VertexOutput vsMain(uint vertexId : SV_VertexID) {
    const float2 positions[3] = {
        float2(-1.0, -1.0),
        float2(-1.0, 3.0),
        float2(3.0, -1.0)
    };
    VertexOutput output;
    output.position = float4(positions[vertexId], 0.0, 1.0);
    return output;
}

float4 psMain(VertexOutput input) : SV_Target {
    const float2 pixel = input.position.xy;
    if (pixel.x < contentOrigin.x || pixel.y < contentOrigin.y
        || pixel.x >= contentOrigin.x + contentSize.x
        || pixel.y >= contentOrigin.y + contentSize.y) {
        return float4(0.0, 0.0, 0.0, 1.0);
    }
    const float2 uv = (pixel - contentOrigin) / contentSize;
    return previewTexture.SampleLevel(previewSampler, uv, 0.0);
}
)";

struct alignas(16) PreviewConstants final {
    float contentX;
    float contentY;
    float contentWidth;
    float contentHeight;
};

static_assert(sizeof(Rgba32Float) == sizeof(float) * 4);
static_assert(sizeof(PreviewConstants) % 16 == 0);

bool isDeviceLost(HRESULT result) noexcept {
    return result == DXGI_ERROR_DEVICE_REMOVED
        || result == DXGI_ERROR_DEVICE_RESET
        || result == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

bool isUnavailable(HRESULT result) noexcept {
    return result == DXGI_ERROR_UNSUPPORTED
        || result == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE
        || result == E_NOINTERFACE;
}

HRESULT compileShader(
    const char* entryPoint,
    const char* target,
    ComPtr<ID3DBlob>& bytecode
) noexcept {
    constexpr UINT flags = D3DCOMPILE_ENABLE_STRICTNESS
        | D3DCOMPILE_IEEE_STRICTNESS
        | D3DCOMPILE_WARNINGS_ARE_ERRORS
        | D3DCOMPILE_OPTIMIZATION_LEVEL3;
    ComPtr<ID3DBlob> diagnostics;
    return D3DCompile(
        previewShaderSource,
        std::char_traits<char>::length(previewShaderSource),
        "palmier_preview_surface.hlsl",
        nullptr,
        nullptr,
        entryPoint,
        target,
        flags,
        0,
        &bytecode,
        &diagnostics
    );
}

detail::D3d11PreviewResultClassification classifyResult(
    HRESULT result,
    D3d11PreviewSurfaceOutcome successOutcome
) noexcept {
    if (result == DXGI_STATUS_OCCLUDED) {
        return {
            D3d11PreviewSurfaceState::occluded,
            D3d11PreviewSurfaceOutcome::occluded,
        };
    }
    if (result == DXGI_ERROR_WAS_STILL_DRAWING) {
        return {
            D3d11PreviewSurfaceState::ready,
            D3d11PreviewSurfaceOutcome::noOp,
        };
    }
    if (isDeviceLost(result)) {
        return {
            D3d11PreviewSurfaceState::invalidated,
            D3d11PreviewSurfaceOutcome::invalidated,
        };
    }
    if (isUnavailable(result)) {
        return {
            D3d11PreviewSurfaceState::invalidated,
            D3d11PreviewSurfaceOutcome::unavailable,
        };
    }
    if (FAILED(result)) {
        return {
            D3d11PreviewSurfaceState::failed,
            D3d11PreviewSurfaceOutcome::failed,
        };
    }
    return {D3d11PreviewSurfaceState::ready, successOutcome};
}

}

detail::D3d11PreviewResultClassification detail::classifyD3d11PreviewResult(
    HRESULT result,
    D3d11PreviewSurfaceOutcome successOutcome
) noexcept {
    return classifyResult(result, successOutcome);
}

class D3d11PreviewSurface::Impl final {
public:
    Impl(
        HWND window,
        D3d11PreviewDriver driver,
        D3d11PreviewSurfaceLimits limits
    ) : window_(window), driver_(driver), limits_(limits) {
        if (window_ == nullptr || limits_.maximumSurfacePixels == 0) {
            throw std::invalid_argument("invalid D3D11 preview surface configuration");
        }
    }

    ~Impl() { close(); }

    D3d11PreviewSurfaceReceipt resize(
        std::uint32_t width,
        std::uint32_t height,
        std::stop_token cancellation
    ) {
        std::lock_guard lock(mutex_);
        if (state_ == D3d11PreviewSurfaceState::closed) {
            return refused(D3d11PreviewSurfaceStage::resize, E_ILLEGAL_METHOD_CALL);
        }
        if (state_ == D3d11PreviewSurfaceState::invalidated
            || state_ == D3d11PreviewSurfaceState::failed) {
            return snapshot_;
        }
        if (!validDimensions(width, height)) {
            return refused(D3d11PreviewSurfaceStage::resize, E_INVALIDARG);
        }
        if (cancellation.stop_requested()) {
            return cancelled(D3d11PreviewSurfaceStage::resize);
        }
        if (!ensureInitialized()) {
            return snapshot_;
        }
        if (cancellation.stop_requested()) {
            return cancelled(D3d11PreviewSurfaceStage::resize);
        }
        if (swapChain_ != nullptr && width == width_ && height == height_) {
            auto value = receipt(
                D3d11PreviewSurfaceOutcome::noOp,
                D3d11PreviewSurfaceStage::resize,
                S_OK
            );
            publish(value);
            return value;
        }

        HRESULT result = S_OK;
        if (swapChain_ == nullptr) {
            DXGI_SWAP_CHAIN_DESC1 description{};
            description.Width = width;
            description.Height = height;
            description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            description.SampleDesc.Count = 1;
            description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            description.BufferCount = 2;
            description.Scaling = DXGI_SCALING_STRETCH;
            description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
            result = factory_->CreateSwapChainForHwnd(
                device_.Get(),
                window_,
                &description,
                nullptr,
                nullptr,
                &swapChain_
            );
            if (SUCCEEDED(result)) {
                result = factory_->MakeWindowAssociation(
                    window_,
                    DXGI_MWA_NO_ALT_ENTER
                );
            }
        } else {
            releaseBackBuffer();
            result = swapChain_->ResizeBuffers(
                0,
                width,
                height,
                DXGI_FORMAT_UNKNOWN,
                0
            );
        }
        if (FAILED(result)) {
            return failResult(result, D3d11PreviewSurfaceStage::resize);
        }
        width_ = width;
        height_ = height;
        result = createBackBuffer();
        if (FAILED(result)) {
            return failResult(result, D3d11PreviewSurfaceStage::resize);
        }
        state_ = D3d11PreviewSurfaceState::ready;
        auto value = receipt(
            D3d11PreviewSurfaceOutcome::noOp,
            D3d11PreviewSurfaceStage::resize,
            S_OK
        );
        publish(value);
        return value;
    }

    D3d11PreviewSurfaceReceipt present(
        const RenderedFrame& frame,
        std::stop_token cancellation
    ) {
        std::lock_guard lock(mutex_);
        if (state_ == D3d11PreviewSurfaceState::closed) {
            return refused(D3d11PreviewSurfaceStage::present, E_ILLEGAL_METHOD_CALL);
        }
        if (state_ == D3d11PreviewSurfaceState::invalidated
            || state_ == D3d11PreviewSurfaceState::failed) {
            return snapshot_;
        }
        if (swapChain_ == nullptr || renderTarget_ == nullptr) {
            return refused(D3d11PreviewSurfaceStage::present, E_PENDING);
        }
        if (!validFrame(frame)) {
            return refused(D3d11PreviewSurfaceStage::upload, E_INVALIDARG);
        }
        if (cancellation.stop_requested()) {
            return cancelled(D3d11PreviewSurfaceStage::upload);
        }
        if (state_ == D3d11PreviewSurfaceState::occluded) {
            const auto visible = swapChain_->Present(0, DXGI_PRESENT_TEST);
            if (visible == DXGI_STATUS_OCCLUDED) {
                auto value = receipt(
                    D3d11PreviewSurfaceOutcome::occluded,
                    D3d11PreviewSurfaceStage::present,
                    visible
                );
                publish(value);
                return value;
            }
            if (FAILED(visible)) {
                return failResult(
                    visible,
                    D3d11PreviewSurfaceStage::present
                );
            }
            state_ = D3d11PreviewSurfaceState::ready;
        }

        auto result = ensureUploadResources(frame.width, frame.height);
        if (FAILED(result)) {
            return failResult(result, D3d11PreviewSurfaceStage::upload);
        }
        if (cancellation.stop_requested()) {
            return cancelled(D3d11PreviewSurfaceStage::upload);
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        result = context_->Map(
            sourceTexture_.Get(),
            0,
            D3D11_MAP_WRITE_DISCARD,
            0,
            &mapped
        );
        if (FAILED(result)) {
            return failResult(result, D3d11PreviewSurfaceStage::upload);
        }
        const auto sourceRowBytes = static_cast<std::size_t>(frame.width)
            * sizeof(Rgba32Float);
        if (mapped.RowPitch < sourceRowBytes) {
            context_->Unmap(sourceTexture_.Get(), 0);
            return failResult(E_UNEXPECTED, D3d11PreviewSurfaceStage::upload);
        }
        auto* destination = static_cast<std::byte*>(mapped.pData);
        const auto* source = reinterpret_cast<const std::byte*>(frame.pixels.data());
        bool uploadCancelled = false;
        for (std::uint32_t row = 0; row < frame.height; ++row) {
            if (cancellation.stop_requested()) {
                uploadCancelled = true;
                break;
            }
            std::memcpy(
                destination + static_cast<std::size_t>(row) * mapped.RowPitch,
                source + static_cast<std::size_t>(row) * sourceRowBytes,
                sourceRowBytes
            );
        }
        context_->Unmap(sourceTexture_.Get(), 0);
        if (uploadCancelled) {
            return cancelled(D3d11PreviewSurfaceStage::upload);
        }
        if (cancellation.stop_requested()) {
            return cancelled(D3d11PreviewSurfaceStage::draw);
        }

        const auto scaleX = static_cast<float>(width_)
            / static_cast<float>(frame.width);
        const auto scaleY = static_cast<float>(height_)
            / static_cast<float>(frame.height);
        const auto scale = scaleX < scaleY ? scaleX : scaleY;
        const auto contentWidth = static_cast<float>(frame.width) * scale;
        const auto contentHeight = static_cast<float>(frame.height) * scale;
        const PreviewConstants constants{
            (static_cast<float>(width_) - contentWidth) * 0.5F,
            (static_cast<float>(height_) - contentHeight) * 0.5F,
            contentWidth,
            contentHeight,
        };
        context_->UpdateSubresource(constants_.Get(), 0, nullptr, &constants, 0, 0);
        constexpr float black[]{0, 0, 0, 1};
        context_->ClearRenderTargetView(renderTarget_.Get(), black);
        ID3D11RenderTargetView* targets[]{renderTarget_.Get()};
        context_->OMSetRenderTargets(1, targets, nullptr);
        const D3D11_VIEWPORT viewport{
            0,
            0,
            static_cast<float>(width_),
            static_cast<float>(height_),
            0,
            1,
        };
        context_->RSSetViewports(1, &viewport);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
        context_->PSSetShader(pixelShader_.Get(), nullptr, 0);
        ID3D11Buffer* constantBuffers[]{constants_.Get()};
        context_->PSSetConstantBuffers(0, 1, constantBuffers);
        ID3D11ShaderResourceView* sourceViews[]{sourceView_.Get()};
        context_->PSSetShaderResources(0, 1, sourceViews);
        ID3D11SamplerState* samplers[]{sampler_.Get()};
        context_->PSSetSamplers(0, 1, samplers);
        context_->Draw(3, 0);
        ID3D11ShaderResourceView* emptyViews[]{nullptr};
        context_->PSSetShaderResources(0, 1, emptyViews);
        context_->OMSetRenderTargets(0, nullptr, nullptr);

        if (cancellation.stop_requested()) {
            return cancelled(D3d11PreviewSurfaceStage::present);
        }
        return completePresent(
            D3d11PreviewSurfaceOutcome::presented,
            D3d11PreviewSurfaceStage::present
        );
    }

    D3d11PreviewSurfaceReceipt clear(std::stop_token cancellation) {
        std::lock_guard lock(mutex_);
        if (state_ == D3d11PreviewSurfaceState::closed) {
            return refused(D3d11PreviewSurfaceStage::clear, E_ILLEGAL_METHOD_CALL);
        }
        if (state_ == D3d11PreviewSurfaceState::invalidated
            || state_ == D3d11PreviewSurfaceState::failed) {
            return snapshot_;
        }
        if (swapChain_ == nullptr || renderTarget_ == nullptr) {
            auto value = receipt(
                D3d11PreviewSurfaceOutcome::noOp,
                D3d11PreviewSurfaceStage::clear,
                S_OK
            );
            publish(value);
            return value;
        }
        if (cancellation.stop_requested()) {
            return cancelled(D3d11PreviewSurfaceStage::clear);
        }
        constexpr float black[]{0, 0, 0, 1};
        context_->ClearRenderTargetView(renderTarget_.Get(), black);
        context_->OMSetRenderTargets(0, nullptr, nullptr);
        return completePresent(
            D3d11PreviewSurfaceOutcome::cleared,
            D3d11PreviewSurfaceStage::clear
        );
    }

    D3d11PreviewSurfaceReceipt snapshot() const {
        std::lock_guard lock(mutex_);
        return snapshot_;
    }

    D3d11PreviewSurfaceReceipt close() {
        std::lock_guard lock(mutex_);
        if (state_ == D3d11PreviewSurfaceState::closed) {
            return snapshot_;
        }
        releaseBackBuffer();
        swapChain_.Reset();
        sourceView_.Reset();
        sourceTexture_.Reset();
        sourceWidth_ = 0;
        sourceHeight_ = 0;
        sampler_.Reset();
        constants_.Reset();
        pixelShader_.Reset();
        vertexShader_.Reset();
        if (context_ != nullptr) {
            context_->ClearState();
            context_->Flush();
        }
        context_.Reset();
        device_.Reset();
        factory_.Reset();
        state_ = D3d11PreviewSurfaceState::closed;
        auto value = receipt(
            D3d11PreviewSurfaceOutcome::noOp,
            D3d11PreviewSurfaceStage::close,
            S_OK
        );
        publish(value);
        return value;
    }

    HRESULT prepareUploadResourcesForTesting(
        std::uint32_t width,
        std::uint32_t height
    ) {
        std::lock_guard lock(mutex_);
        if (device_ == nullptr || !validDimensions(width, height)) {
            return E_INVALIDARG;
        }
        return ensureUploadResources(width, height);
    }

    std::uint64_t uploadResourceSerial() const {
        std::lock_guard lock(mutex_);
        return uploadResourceSerial_;
    }

private:
    bool validDimensions(std::uint32_t width, std::uint32_t height) const noexcept {
        if (width == 0 || height == 0) {
            return false;
        }
        const auto pixels = static_cast<std::uint64_t>(width) * height;
        return pixels <= limits_.maximumSurfacePixels;
    }

    bool validFrame(const RenderedFrame& frame) const noexcept {
        if (!validDimensions(frame.width, frame.height)
            || frame.width > (std::numeric_limits<UINT>::max)()
                / sizeof(Rgba32Float)) {
            return false;
        }
        const auto pixels = static_cast<std::uint64_t>(frame.width) * frame.height;
        return pixels == frame.pixels.size();
    }

    bool ensureInitialized() {
        if (device_ != nullptr) {
            return true;
        }
        if (!IsWindow(window_)) {
            failResult(E_HANDLE, D3d11PreviewSurfaceStage::initialize);
            return false;
        }
        constexpr std::array featureLevels{
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
        };
        D3D_FEATURE_LEVEL selectedLevel{};
        const auto driverType = driver_ == D3d11PreviewDriver::warp
            ? D3D_DRIVER_TYPE_WARP
            : D3D_DRIVER_TYPE_HARDWARE;
        auto result = D3D11CreateDevice(
            nullptr,
            driverType,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            featureLevels.data(),
            static_cast<UINT>(featureLevels.size()),
            D3D11_SDK_VERSION,
            &device_,
            &selectedLevel,
            &context_
        );
        if (FAILED(result)) {
            failResult(result, D3d11PreviewSurfaceStage::initialize);
            return false;
        }
        if (selectedLevel != D3D_FEATURE_LEVEL_11_1
            && selectedLevel != D3D_FEATURE_LEVEL_11_0) {
            failResult(E_NOINTERFACE, D3d11PreviewSurfaceStage::initialize);
            return false;
        }

        ComPtr<IDXGIDevice> dxgiDevice;
        result = device_.As(&dxgiDevice);
        if (SUCCEEDED(result)) {
            ComPtr<IDXGIAdapter> adapter;
            result = dxgiDevice->GetAdapter(&adapter);
            if (SUCCEEDED(result)) {
                result = adapter->GetParent(IID_PPV_ARGS(&factory_));
            }
        }
        if (FAILED(result)) {
            failResult(result, D3d11PreviewSurfaceStage::initialize);
            return false;
        }

        ComPtr<ID3DBlob> vertexBytecode;
        result = compileShader("vsMain", "vs_5_0", vertexBytecode);
        if (SUCCEEDED(result)) {
            result = device_->CreateVertexShader(
                vertexBytecode->GetBufferPointer(),
                vertexBytecode->GetBufferSize(),
                nullptr,
                &vertexShader_
            );
        }
        if (FAILED(result)) {
            failResult(result, D3d11PreviewSurfaceStage::initialize);
            return false;
        }
        ComPtr<ID3DBlob> pixelBytecode;
        result = compileShader("psMain", "ps_5_0", pixelBytecode);
        if (SUCCEEDED(result)) {
            result = device_->CreatePixelShader(
                pixelBytecode->GetBufferPointer(),
                pixelBytecode->GetBufferSize(),
                nullptr,
                &pixelShader_
            );
        }
        if (FAILED(result)) {
            failResult(result, D3d11PreviewSurfaceStage::initialize);
            return false;
        }

        D3D11_BUFFER_DESC constantDescription{};
        constantDescription.ByteWidth = static_cast<UINT>(sizeof(PreviewConstants));
        constantDescription.Usage = D3D11_USAGE_DEFAULT;
        constantDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        result = device_->CreateBuffer(&constantDescription, nullptr, &constants_);
        if (FAILED(result)) {
            failResult(result, D3d11PreviewSurfaceStage::initialize);
            return false;
        }
        D3D11_SAMPLER_DESC samplerDescription{};
        samplerDescription.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        samplerDescription.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDescription.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDescription.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDescription.MaxLOD = D3D11_FLOAT32_MAX;
        result = device_->CreateSamplerState(&samplerDescription, &sampler_);
        if (FAILED(result)) {
            failResult(result, D3d11PreviewSurfaceStage::initialize);
            return false;
        }
        return true;
    }

    HRESULT createBackBuffer() {
        ComPtr<ID3D11Texture2D> backBuffer;
        auto result = swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
        if (SUCCEEDED(result)) {
            result = device_->CreateRenderTargetView(
                backBuffer.Get(),
                nullptr,
                &renderTarget_
            );
        }
        return result;
    }

    HRESULT ensureUploadResources(
        std::uint32_t width,
        std::uint32_t height
    ) {
        if (sourceTexture_ != nullptr && sourceView_ != nullptr
            && sourceWidth_ == width && sourceHeight_ == height) {
            return S_OK;
        }
        if (uploadResourceSerial_
            == (std::numeric_limits<std::uint64_t>::max)()) {
            return HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
        }
        const D3D11_TEXTURE2D_DESC description{
            width,
            height,
            1,
            1,
            DXGI_FORMAT_R32G32B32A32_FLOAT,
            {1, 0},
            D3D11_USAGE_DYNAMIC,
            D3D11_BIND_SHADER_RESOURCE,
            D3D11_CPU_ACCESS_WRITE,
            0,
        };
        ComPtr<ID3D11Texture2D> candidateTexture;
        auto result = device_->CreateTexture2D(
            &description,
            nullptr,
            &candidateTexture
        );
        if (FAILED(result)) {
            return result;
        }
        ComPtr<ID3D11ShaderResourceView> candidateView;
        result = device_->CreateShaderResourceView(
            candidateTexture.Get(),
            nullptr,
            &candidateView
        );
        if (FAILED(result)) {
            return result;
        }
        sourceTexture_ = std::move(candidateTexture);
        sourceView_ = std::move(candidateView);
        sourceWidth_ = width;
        sourceHeight_ = height;
        ++uploadResourceSerial_;
        return S_OK;
    }

    void releaseBackBuffer() noexcept {
        if (context_ != nullptr) {
            context_->OMSetRenderTargets(0, nullptr, nullptr);
            context_->ClearState();
            context_->Flush();
        }
        renderTarget_.Reset();
    }

    D3d11PreviewSurfaceReceipt completePresent(
        D3d11PreviewSurfaceOutcome successOutcome,
        D3d11PreviewSurfaceStage stage
    ) {
        const auto result = swapChain_->Present(0, DXGI_PRESENT_DO_NOT_WAIT);
        const auto classification = classifyResult(result, successOutcome);
        state_ = classification.state;
        if (classification.outcome == D3d11PreviewSurfaceOutcome::invalidated
            || classification.outcome == D3d11PreviewSurfaceOutcome::unavailable
            || classification.outcome == D3d11PreviewSurfaceOutcome::failed) {
            auto value = receipt(classification.outcome, stage, result);
            publish(value);
            return value;
        }
        if (classification.outcome == successOutcome) {
            ++presentSerial_;
        }
        auto value = receipt(classification.outcome, stage, result);
        publish(value);
        return value;
    }

    D3d11PreviewSurfaceReceipt failResult(
        HRESULT result,
        D3d11PreviewSurfaceStage stage
    ) {
        const auto classification = classifyResult(
            result,
            D3d11PreviewSurfaceOutcome::noOp
        );
        state_ = classification.state;
        auto value = receipt(classification.outcome, stage, result);
        publish(value);
        return value;
    }

    D3d11PreviewSurfaceReceipt refused(
        D3d11PreviewSurfaceStage stage,
        HRESULT result
    ) const {
        return receipt(D3d11PreviewSurfaceOutcome::refused, stage, result);
    }

    D3d11PreviewSurfaceReceipt cancelled(
        D3d11PreviewSurfaceStage stage
    ) const {
        return receipt(
            D3d11PreviewSurfaceOutcome::cancelled,
            stage,
            HRESULT_FROM_WIN32(ERROR_CANCELLED)
        );
    }

    D3d11PreviewSurfaceReceipt receipt(
        D3d11PreviewSurfaceOutcome outcome,
        D3d11PreviewSurfaceStage stage,
        HRESULT result
    ) const {
        return {
            state_,
            outcome,
            stage,
            result,
            width_,
            height_,
            presentSerial_,
        };
    }

    void publish(const D3d11PreviewSurfaceReceipt& value) {
        snapshot_ = value;
    }

    HWND window_{};
    D3d11PreviewDriver driver_{D3d11PreviewDriver::hardware};
    D3d11PreviewSurfaceLimits limits_;
    mutable std::mutex mutex_;
    D3d11PreviewSurfaceState state_{D3d11PreviewSurfaceState::idle};
    D3d11PreviewSurfaceReceipt snapshot_;
    std::uint32_t width_{};
    std::uint32_t height_{};
    std::uint64_t presentSerial_{};
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGIFactory2> factory_;
    ComPtr<IDXGISwapChain1> swapChain_;
    ComPtr<ID3D11RenderTargetView> renderTarget_;
    ComPtr<ID3D11Texture2D> sourceTexture_;
    ComPtr<ID3D11ShaderResourceView> sourceView_;
    std::uint32_t sourceWidth_{};
    std::uint32_t sourceHeight_{};
    std::uint64_t uploadResourceSerial_{};
    ComPtr<ID3D11VertexShader> vertexShader_;
    ComPtr<ID3D11PixelShader> pixelShader_;
    ComPtr<ID3D11Buffer> constants_;
    ComPtr<ID3D11SamplerState> sampler_;
};

D3d11PreviewSurface::D3d11PreviewSurface(
    HWND window,
    D3d11PreviewDriver driver,
    D3d11PreviewSurfaceLimits limits
) : impl_(std::make_unique<Impl>(window, driver, limits)) {}

D3d11PreviewSurface::~D3d11PreviewSurface() = default;

D3d11PreviewSurfaceReceipt D3d11PreviewSurface::resize(
    std::uint32_t width,
    std::uint32_t height,
    std::stop_token cancellation
) {
    return impl_->resize(width, height, cancellation);
}

D3d11PreviewSurfaceReceipt D3d11PreviewSurface::present(
    const RenderedFrame& frame,
    std::stop_token cancellation
) {
    return impl_->present(frame, cancellation);
}

D3d11PreviewSurfaceReceipt D3d11PreviewSurface::clear(
    std::stop_token cancellation
) {
    return impl_->clear(cancellation);
}

D3d11PreviewSurfaceReceipt D3d11PreviewSurface::snapshot() const {
    return impl_->snapshot();
}

D3d11PreviewSurfaceReceipt D3d11PreviewSurface::close() {
    return impl_->close();
}

HRESULT detail::D3d11PreviewSurfaceTestAccess::prepareUploadResources(
    D3d11PreviewSurface& surface,
    std::uint32_t width,
    std::uint32_t height
) {
    return surface.impl_->prepareUploadResourcesForTesting(width, height);
}

std::uint64_t detail::D3d11PreviewSurfaceTestAccess::uploadResourceSerial(
    const D3d11PreviewSurface& surface
) {
    return surface.impl_->uploadResourceSerial();
}

}
