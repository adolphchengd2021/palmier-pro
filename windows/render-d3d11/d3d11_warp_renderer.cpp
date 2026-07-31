#include "palmier/render/d3d11_warp_renderer.hpp"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iomanip>
#include <mutex>
#include <new>
#include <numbers>
#include <sstream>
#include <string>
#include <utility>

namespace palmier::render {
namespace {

using Microsoft::WRL::ComPtr;

constexpr auto shaderSource = R"(
cbuffer LayerConstants : register(b0) {
    float2 canvasSize;
    float2 center;
    float2 layerSize;
    float2 rotation;
    float opacity;
    float exposureEv;
    float2 padding;
};

Texture2D<float4> sourceTexture : register(t0);

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

float srgbToLinear(float value) {
    return value <= 0.04045
        ? value / 12.92
        : pow(max((value + 0.055) / 1.055, 0.0), 2.4);
}

float linearToSrgb(float value) {
    return value <= 0.0031308
        ? value * 12.92
        : 1.055 * pow(max(value, 0.0), 1.0 / 2.4) - 0.055;
}

float4 psMain(VertexOutput input) : SV_Target {
    const float2 target = input.position.xy / canvasSize;
    const float2 delta = target - center;
    const float2 local = float2(
        rotation.x * delta.x + rotation.y * delta.y,
        -rotation.y * delta.x + rotation.x * delta.y
    );
    const float2 uv = local / layerSize + 0.5;
    if (uv.x < 0.0 || uv.x >= 1.0 || uv.y < 0.0 || uv.y >= 1.0) {
        discard;
    }

    uint sourceWidth;
    uint sourceHeight;
    sourceTexture.GetDimensions(sourceWidth, sourceHeight);
    const uint2 sourcePixel = min(
        uint2(uv * float2(sourceWidth, sourceHeight)),
        uint2(sourceWidth - 1, sourceHeight - 1)
    );
    const float4 source = sourceTexture.Load(int3(sourcePixel, 0));
    const float multiplier = exp2(exposureEv);
    const float3 linearColor = float3(
        srgbToLinear(source.r),
        srgbToLinear(source.g),
        srgbToLinear(source.b)
    ) * multiplier;
    const float3 processed = float3(
        linearToSrgb(linearColor.r),
        linearToSrgb(linearColor.g),
        linearToSrgb(linearColor.b)
    );
    const float alpha = source.a * opacity;
    return float4(processed * alpha, alpha);
}
)";

struct alignas(16) LayerConstants final {
    float canvasWidth;
    float canvasHeight;
    float centerX;
    float centerY;
    float layerWidth;
    float layerHeight;
    float cosine;
    float sine;
    float opacity;
    float exposureEv;
    float padding0;
    float padding1;
};

static_assert(sizeof(Rgba32Float) == sizeof(float) * 4);
static_assert(sizeof(LayerConstants) % 16 == 0);

std::string hresultString(HRESULT result) {
    std::ostringstream stream;
    stream << "HRESULT 0x" << std::hex << std::uppercase
           << static_cast<unsigned long>(result);
    return stream.str();
}

[[noreturn]] void fail(std::string code, std::string detail) {
    throw RenderError(std::move(code), "/d3d11", std::move(detail));
}

void requireSuccess(HRESULT result, const std::string& operation) {
    if (FAILED(result)) {
        if (result == E_OUTOFMEMORY) {
            fail("resourceExhausted", operation + " exhausted graphics memory");
        }
        fail("d3d11Failure", operation + " failed with " + hresultString(result));
    }
}

class ScopedTextureMap final {
public:
    ScopedTextureMap(ID3D11DeviceContext* context, ID3D11Texture2D* texture)
        : context_(context), texture_(texture) {
        requireSuccess(
            context_->Map(texture_, 0, D3D11_MAP_READ, 0, &mapped_),
            "Map(staging)"
        );
    }

    ~ScopedTextureMap() { context_->Unmap(texture_, 0); }

    ScopedTextureMap(const ScopedTextureMap&) = delete;
    ScopedTextureMap& operator=(const ScopedTextureMap&) = delete;

    const D3D11_MAPPED_SUBRESOURCE& value() const noexcept { return mapped_; }

private:
    ID3D11DeviceContext* context_;
    ID3D11Texture2D* texture_;
    D3D11_MAPPED_SUBRESOURCE mapped_{};
};

ComPtr<ID3DBlob> compileShader(const char* entryPoint, const char* target) {
    constexpr UINT flags = D3DCOMPILE_ENABLE_STRICTNESS
        | D3DCOMPILE_IEEE_STRICTNESS
        | D3DCOMPILE_WARNINGS_ARE_ERRORS
        | D3DCOMPILE_OPTIMIZATION_LEVEL3;
    ComPtr<ID3DBlob> bytecode;
    ComPtr<ID3DBlob> diagnostics;
    const auto result = D3DCompile(
        shaderSource,
        std::strlen(shaderSource),
        "palmier_render_plan.hlsl",
        nullptr,
        nullptr,
        entryPoint,
        target,
        flags,
        0,
        &bytecode,
        &diagnostics
    );
    if (FAILED(result)) {
        const auto detail = diagnostics
            ? std::string(
                static_cast<const char*>(diagnostics->GetBufferPointer()),
                diagnostics->GetBufferSize()
            )
            : hresultString(result);
        fail("shaderCompilationFailed", detail);
    }
    return bytecode;
}

D3D11_TEXTURE2D_DESC textureDescription(
    std::uint32_t width,
    std::uint32_t height,
    D3D11_USAGE usage,
    UINT bindFlags,
    UINT cpuAccessFlags
) {
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    description.SampleDesc.Count = 1;
    description.Usage = usage;
    description.BindFlags = bindFlags;
    description.CPUAccessFlags = cpuAccessFlags;
    return description;
}

}

class D3d11WarpRenderer::Impl final {
public:
    Impl() {
        constexpr std::array featureLevels{D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL selectedLevel{};
        requireSuccess(
            D3D11CreateDevice(
                nullptr,
                D3D_DRIVER_TYPE_WARP,
                nullptr,
                0,
                featureLevels.data(),
                static_cast<UINT>(featureLevels.size()),
                D3D11_SDK_VERSION,
                &device_,
                &selectedLevel,
                &context_
            ),
            "D3D11CreateDevice(WARP)"
        );
        if (selectedLevel != D3D_FEATURE_LEVEL_11_0) {
            fail("unsupportedFeatureLevel", "WARP did not provide feature level 11_0");
        }

        UINT formatSupport = 0;
        requireSuccess(
            device_->CheckFormatSupport(DXGI_FORMAT_R32G32B32A32_FLOAT, &formatSupport),
            "CheckFormatSupport(R32G32B32A32_FLOAT)"
        );
        constexpr UINT requiredSupport = D3D11_FORMAT_SUPPORT_TEXTURE2D
            | D3D11_FORMAT_SUPPORT_SHADER_LOAD
            | D3D11_FORMAT_SUPPORT_RENDER_TARGET
            | D3D11_FORMAT_SUPPORT_BLENDABLE;
        if ((formatSupport & requiredSupport) != requiredSupport) {
            fail("unsupportedRenderFormat", "WARP float render format lacks required support");
        }

        const auto vertexBytecode = compileShader("vsMain", "vs_5_0");
        requireSuccess(
            device_->CreateVertexShader(
                vertexBytecode->GetBufferPointer(),
                vertexBytecode->GetBufferSize(),
                nullptr,
                &vertexShader_
            ),
            "CreateVertexShader"
        );
        const auto pixelBytecode = compileShader("psMain", "ps_5_0");
        requireSuccess(
            device_->CreatePixelShader(
                pixelBytecode->GetBufferPointer(),
                pixelBytecode->GetBufferSize(),
                nullptr,
                &pixelShader_
            ),
            "CreatePixelShader"
        );

        D3D11_BUFFER_DESC constantDescription{};
        constantDescription.ByteWidth = static_cast<UINT>(sizeof(LayerConstants));
        constantDescription.Usage = D3D11_USAGE_DEFAULT;
        constantDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        requireSuccess(
            device_->CreateBuffer(&constantDescription, nullptr, &layerConstants_),
            "CreateBuffer(layer constants)"
        );

        D3D11_BLEND_DESC blendDescription{};
        auto& target = blendDescription.RenderTarget[0];
        target.BlendEnable = TRUE;
        target.SrcBlend = D3D11_BLEND_ONE;
        target.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        target.BlendOp = D3D11_BLEND_OP_ADD;
        target.SrcBlendAlpha = D3D11_BLEND_ONE;
        target.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        requireSuccess(
            device_->CreateBlendState(&blendDescription, &blendState_),
            "CreateBlendState"
        );

        D3D11_RASTERIZER_DESC rasterizerDescription{};
        rasterizerDescription.FillMode = D3D11_FILL_SOLID;
        rasterizerDescription.CullMode = D3D11_CULL_NONE;
        rasterizerDescription.DepthClipEnable = TRUE;
        requireSuccess(
            device_->CreateRasterizerState(&rasterizerDescription, &rasterizerState_),
            "CreateRasterizerState"
        );
    }

    RenderedFrame render(const RenderPlan& plan, const FrameResolver& resolveFrame) {
        const std::scoped_lock lock(renderMutex_);
        const auto sources = resolveAndValidateSourceFrames(plan, resolveFrame);
        const auto targetDescription = textureDescription(
            plan.canvasWidth(),
            plan.canvasHeight(),
            D3D11_USAGE_DEFAULT,
            D3D11_BIND_RENDER_TARGET,
            0
        );
        ComPtr<ID3D11Texture2D> targetTexture;
        requireSuccess(
            device_->CreateTexture2D(&targetDescription, nullptr, &targetTexture),
            "CreateTexture2D(render target)"
        );
        ComPtr<ID3D11RenderTargetView> renderTarget;
        requireSuccess(
            device_->CreateRenderTargetView(targetTexture.Get(), nullptr, &renderTarget),
            "CreateRenderTargetView"
        );

        constexpr float black[]{0, 0, 0, 1};
        context_->ClearRenderTargetView(renderTarget.Get(), black);
        ID3D11RenderTargetView* targets[]{renderTarget.Get()};
        context_->OMSetRenderTargets(1, targets, nullptr);
        context_->OMSetBlendState(blendState_.Get(), nullptr, 0xFFFFFFFF);
        context_->RSSetState(rasterizerState_.Get());
        const D3D11_VIEWPORT viewport{
            0,
            0,
            static_cast<float>(plan.canvasWidth()),
            static_cast<float>(plan.canvasHeight()),
            0,
            1,
        };
        context_->RSSetViewports(1, &viewport);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
        context_->PSSetShader(pixelShader_.Get(), nullptr, 0);
        ID3D11Buffer* constantBuffers[]{layerConstants_.Get()};
        context_->PSSetConstantBuffers(0, 1, constantBuffers);

        for (std::size_t index = 0; index < plan.layers().size(); ++index) {
            const auto& layer = plan.layers()[index];
            const auto& source = *sources[index];
            const auto sourceDescription = textureDescription(
                source.width,
                source.height,
                D3D11_USAGE_IMMUTABLE,
                D3D11_BIND_SHADER_RESOURCE,
                0
            );
            const D3D11_SUBRESOURCE_DATA sourceData{
                source.pixels.data(),
                source.width * static_cast<UINT>(sizeof(Rgba32Float)),
                0,
            };
            ComPtr<ID3D11Texture2D> sourceTexture;
            requireSuccess(
                device_->CreateTexture2D(&sourceDescription, &sourceData, &sourceTexture),
                "CreateTexture2D(source)"
            );
            ComPtr<ID3D11ShaderResourceView> sourceView;
            requireSuccess(
                device_->CreateShaderResourceView(sourceTexture.Get(), nullptr, &sourceView),
                "CreateShaderResourceView"
            );

            const auto angle = layer.transform.rotationDegrees
                * std::numbers::pi_v<float> / 180.0F;
            const LayerConstants constants{
                static_cast<float>(plan.canvasWidth()),
                static_cast<float>(plan.canvasHeight()),
                static_cast<float>(layer.transform.centerX),
                static_cast<float>(layer.transform.centerY),
                static_cast<float>(layer.transform.width),
                static_cast<float>(layer.transform.height),
                std::cos(angle),
                std::sin(angle),
                layer.opacity,
                layer.exposureEv.value_or(0),
                0,
                0,
            };
            context_->UpdateSubresource(layerConstants_.Get(), 0, nullptr, &constants, 0, 0);
            ID3D11ShaderResourceView* sourceViews[]{sourceView.Get()};
            context_->PSSetShaderResources(0, 1, sourceViews);
            context_->Draw(3, 0);
            ID3D11ShaderResourceView* emptyViews[]{nullptr};
            context_->PSSetShaderResources(0, 1, emptyViews);
        }

        const auto stagingDescription = textureDescription(
            plan.canvasWidth(),
            plan.canvasHeight(),
            D3D11_USAGE_STAGING,
            0,
            D3D11_CPU_ACCESS_READ
        );
        ComPtr<ID3D11Texture2D> staging;
        requireSuccess(
            device_->CreateTexture2D(&stagingDescription, nullptr, &staging),
            "CreateTexture2D(staging)"
        );
        std::vector<Rgba32Float> outputPixels;
        try {
            outputPixels.resize(
                static_cast<std::size_t>(plan.canvasWidth()) * plan.canvasHeight()
            );
        } catch (const std::bad_alloc&) {
            fail("resourceExhausted", "D3D11 readback allocation failed");
        }
        RenderedFrame output{
            plan.canvasWidth(),
            plan.canvasHeight(),
            std::move(outputPixels),
        };
        context_->CopyResource(staging.Get(), targetTexture.Get());
        const ScopedTextureMap mapped(context_.Get(), staging.Get());
        for (std::uint32_t row = 0; row < plan.canvasHeight(); ++row) {
            const auto* sourceRow = static_cast<const std::byte*>(mapped.value().pData)
                + static_cast<std::size_t>(row) * mapped.value().RowPitch;
            auto* destinationRow = output.pixels.data()
                + static_cast<std::size_t>(row) * plan.canvasWidth();
            std::memcpy(
                destinationRow,
                sourceRow,
                static_cast<std::size_t>(plan.canvasWidth()) * sizeof(Rgba32Float)
            );
        }
        context_->OMSetRenderTargets(0, nullptr, nullptr);
        return output;
    }

private:
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11VertexShader> vertexShader_;
    ComPtr<ID3D11PixelShader> pixelShader_;
    ComPtr<ID3D11Buffer> layerConstants_;
    ComPtr<ID3D11BlendState> blendState_;
    ComPtr<ID3D11RasterizerState> rasterizerState_;
    std::mutex renderMutex_;
};

D3d11WarpRenderer::D3d11WarpRenderer() : impl_(std::make_unique<Impl>()) {}
D3d11WarpRenderer::~D3d11WarpRenderer() = default;

RenderedFrame D3d11WarpRenderer::render(
    const RenderPlan& plan,
    const FrameResolver& resolveFrame
) {
    return impl_->render(plan, resolveFrame);
}

}
