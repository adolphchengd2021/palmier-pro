# ADR 0012: Normalize decoded frames before render resolution

- Status: Accepted for M0 prototype
- Date: 2026-08-01
- Owner: Windows media and rendering
- Applies to: FFmpeg decoded-frame ingestion into the synthetic RenderPlan

## Decision

`windows/media-render` is the only adapter from `DecodedVideoFrame` to the
platform-neutral `SourceFrame`. Decode and conversion finish before a
`FrameResolver` is published. A resolver never performs file I/O, FFmpeg work,
pixel conversion, or cache mutation.

The adapter validates the RenderPlan source dimensions before allocating the
RGBA32F buffer. It requires exact storage for `rowBytes * height`, accepts
padded rows, copies only visible RGBA bytes, and owns the resulting tightly
packed float pixels. The returned value has no lifetime dependency on the
decoded byte buffer.

FFmpeg display rotation is presentation metadata, so the adapter applies pure
cardinal counterclockwise rotation to the pixels and swaps dimensions for
90-degree rotations. RenderPlan layer rotation remains the user's independent
clockwise placement transform. No caller may both rotate the source pixels and
copy the display rotation into a layer transform.

The output contract is top-left, straight-alpha sRGB RGBA32F. The adapter
accepts only the media prototype's BT.709 primaries, sRGB transfer, RGB matrix,
and full or unspecified packed-RGB range. Explicit straight alpha is normalized
by `byte / 255`. Explicit opaque alpha must contain only `255` alpha bytes.
Unspecified and premultiplied alpha are hard refusals; the prototype does not
guess alpha or define an unpremultiplication color domain.

Conversion checks a `std::stop_token` before validation, on every source row,
and before publication. Cancellation and every validation failure return no
partial `SourceFrame`. Future project and cache owners must separately recheck
project identity, media revision, requested source frame, generation, and
lifecycle state before publishing or committing a result.

## Source-frame boundary

`decodeFirstVideoFrame` does not define a mapping for arbitrary RenderPlan
`sourceFrame` values. The M0 integration test resolves only source frame zero.
VFR, frame-rate conversion, seeking, presentation ordering, cache generations,
stale completion, and render-time cancellation remain outside this adapter.
They must be owned by a later media session and project compiler.

## Evidence boundary

The FFmpeg CI preset covers exact synthetic orientation and padded-stride
goldens, alpha/color/storage refusals, pre-cancellation, CPU/WARP parity, and
bitwise-identical preview/export calls per backend. Internal deterministic
checkpoints also prove cancellation after a converted row and immediately
before publication without sleeps. The real QTRLE decoded fixture carries
unspecified alpha and proves the adapter refuses it.

This does not yet prove a successfully adapted real media fixture, Swift BGRA8
pixel equivalence, cancellation while FFmpeg is blocked in external I/O, cache
lease behavior, Windows 10 build 19045, physical GPUs, D3D11VA, continuous
playback, audio sync, encoding, or packaging.

## Primary references

- [FFmpeg display matrix](https://ffmpeg.org/doxygen/trunk/display_8h_source.html)
- [FFmpeg alpha modes](https://www.ffmpeg.org/doxygen/trunk/pixfmt_8h.html)
- [FFmpeg AVFrame row storage](https://www.ffmpeg.org/doxygen/trunk/structAVFrame.html)
