# ADR 0009: Render plan and D3D11 WARP reference

- Status: Accepted synthetic reference spike
- Date: 2026-08-01
- Owner: Media and rendering

## Decision

The first Windows rendering boundary is an immutable platform-neutral
`RenderPlan`. The CPU reference renderer and D3D11 renderer consume that same
plan. Preview-frame and export-frame entry points are thin adapters over the
same full-quality backend operation and do not own effect, transform, opacity,
or blending rules.

This is a synthetic float reference, not yet an oracle for the shipping Swift
BGRA8 compositor. Transform, opacity, and exposure values use float32. Rotation
is normalized with IEEE remainder modulo 360 before either backend sees it.
Plans are capped at 256 layers and 8,294,400 output pixels; source frames use
the same pixel budget. Pixel count multiplied by visible layers is capped at
67,108,864 composite samples. All sources are resolved and validated before
render work begins; repeated references are validated once, while per-layer
source pixels are capped at a cumulative 67,108,864 upload-work budget.

The v1 spike uses a top-left normalized canvas, bottom-to-top layers, nearest
point sampling, straight-alpha sRGB float sources, linear-light exposure,
premultiplied normal source-over, and an opaque black background. Positive
rotation is visually clockwise. Exposure resolves to `2^EV` with EV in
`[-3, 3]`.

The Windows CI backend creates a feature-level 11_0 WARP device, compiles the
production spike shader as Shader Model 5, renders to
`R32G32B32A32_FLOAT`, and reads a staging texture row by row. WARP, shader,
format, render, or readback failure fails the test; it is never skipped because
the runner has no physical GPU.

Each WARP backend instance serializes the complete immediate-context command
sequence. A frame resolver must keep each returned source immutable for the
duration of the render call.

## Scope boundary

This commit does not compile a Palmier project into a render plan. The current
C++ project projection is intentionally incomplete and lacks transform and
effect data. Until a dedicated compiler reads those values and rejects every
visible unsupported feature, no application path may silently construct an
identity or pass-through plan from the typed projection.

Project integration must reject same-track overlap, non-1 speed, variable frame
rate, source/timeline frame-rate mismatch, crop, flip, dynamic keyframes,
fades, non-normal blends, text, nesting, Lottie, unsupported effects, HDR,
mixed or unknown color metadata, and unknown alpha representation. These are
hard refusals, not fallback behavior.

The precise machine-readable boundary is
`contracts/render/v1/render-plan.json`. The wider effect contract remains
incomplete; this ADR freezes only the exposure subset used by the spike.

## Evidence boundary

A Windows Server 2022 WARP pass proves that the locked SDK can compile and run
the headless D3D11 reference path and that its float output agrees with the CPU
oracle within the recorded threshold. It does not prove Windows 10 build 19045,
physical NVIDIA/AMD/Intel devices, FFmpeg decoding or encoding, BGRA8 output,
color metadata propagation, HDR, WASAPI, playback timing, or user-visible UI.

Swift-generated BGRA8 goldens are still required to freeze sampling, edge,
quantization, color, alpha, and clipping semantics before this plan can claim
cross-platform preview/export pixel equivalence.

Windows 10 clean-VM and physical-GPU lanes remain separate Technical MVP gates.

## References

- [Create a WARP device](https://learn.microsoft.com/en-us/windows/win32/direct3d11/overviews-direct3d-11-devices-create-warp)
- [D3DCompile](https://learn.microsoft.com/en-us/windows/win32/api/d3dcompiler/nf-d3dcompiler-d3dcompile)
- [ID3D11DeviceContext::Map](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-map)
