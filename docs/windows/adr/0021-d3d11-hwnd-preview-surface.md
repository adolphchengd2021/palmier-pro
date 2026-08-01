# ADR 0021: Present bounded preview frames to one HWND

- Status: Accepted for M1 native presentation boundary
- Date: 2026-08-01
- Owner: Windows D3D11 renderer
- Applies to: preview pixels after shared render-plan composition

## Decision

`D3d11PreviewSurface` is the single serialized owner of one HWND swap chain,
D3D11 device and immediate context, back-buffer view, presentation serial, and
terminal state. Its synchronous methods perform GPU setup and control work, so
the future Qt coordinator must call them from one background presentation
executor and never from the UI thread.

The surface lazily creates either a hardware or explicitly requested WARP
device. It creates one two-buffer `B8G8R8A8_UNORM` flip-discard swap chain with
Alt+Enter disabled. A resize first unbinds every context resource, clears state,
flushes the immediate context, releases the render-target view, and only then
calls `ResizeBuffers`. Zero, over-budget, unchanged, cancelled, and terminal
requests do not partially replace the live back buffer.

Each admitted frame is a complete `RenderedFrame` produced by the existing
shared preview renderer. The boundary validates dimensions and exact storage,
reuses one dynamic `R32G32B32A32_FLOAT` texture and SRV until source dimensions
change, uploads with `D3D11_MAP_WRITE_DISCARD`, draws one full-screen triangle,
letterboxes the source, and clears uncovered pixels to opaque black.
There is no frame queue and no retry loop. `Present(0,
DXGI_PRESENT_DO_NOT_WAIT)` executes at most once per call.

`DXGI_STATUS_OCCLUDED` suspends useful presentation until a later
`DXGI_PRESENT_TEST` reports visibility. `DXGI_ERROR_WAS_STILL_DRAWING` is an
explicit no-op, not a spin. Device removal, reset, or internal-driver failure
permanently invalidates the instance. Unsupported environments are separately
classified as unavailable. Other failing HRESULT values permanently fail the
instance. A higher owner must create a new surface after terminal invalidation;
this boundary does not silently recreate a device or retarget a window.

Cancellation is checked before initialization, again before swap-chain resize,
before upload, before draw, and before Present. Cancellation after GPU draw but
before Present leaves the unpresented back buffer private and does not advance `presentSerial`.
Repeated close is stable. Close releases all back-buffer references before the
swap chain and releases the context before the device and factory.

## Tests

Deterministic result-classification tests inject success, occlusion, busy,
unsupported, device-removed, and generic failure HRESULT values. A native smoke
creates a hidden HWND and WARP device, exercises initial resize, same-size
no-op, same-size upload-resource reuse, dimension-change replacement, invalid
frame refusal, one classified present, second resize, clear, pre-cancellation,
invalid limits, and repeated close. It uses a hard timeout and contains no
polling or retry loop.

## Evidence boundary

A green MSVC `/W4 /WX` build and CTest prove API compilation, deterministic
HRESULT classification, swap-chain lifecycle ordering, and that the GitHub
runner can classify a hidden-HWND WARP path. They do not prove Qt integration,
visible pixels, display cadence, physical-GPU performance, zero-copy decode,
device recovery, HDR, A/V synchronization, or Windows 10 build 19045 behavior.

## Primary references

- [IDXGIFactory2::CreateSwapChainForHwnd](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgifactory2-createswapchainforhwnd)
- [IDXGISwapChain::Present](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-present)
- [IDXGISwapChain::ResizeBuffers](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-resizebuffers)
