# ADR 0016: Feed presentation-ordered FFmpeg frames into shared rendering

- Status: Accepted for M1 prototype
- Date: 2026-08-01
- Owner: Windows media session
- Applies to: sequential software decode through bounded preview handoff

## Decision

`FfmpegVideoFrameReader` is the stateful, synchronous owner of one input,
demuxer, software decoder, packet, frame, and reusable swscale context. It
receives decoder output before supplying more input, retains an unsent packet
when FFmpeg reports `EAGAIN`, sends one drain packet at input EOF, and exposes a
stable end-of-stream result. `decodeFirstVideoFrame` delegates to this cursor so
there is no second first-frame implementation.

The reader publishes `best_effort_timestamp` with the stream time base in
decoder output order. It does not invent missing timestamps, reorder output,
seek, or map timeline frame indexes to media timestamps. Any failure or
cancellation makes that reader terminal; resuming requires a new generation.
The synchronous API remains confined to a serial media executor owned by the
future application session.

`PresentationVideoDecodePump` is the single mutable orchestration owner. It
owns one reader, the existing `PresentationVideoBuffer`, and at most one decoded pending frame.
A fill call retries the pending frame before decoding more. When
the bounded buffer refuses admission, the pump reports `blocked` and retains
that exact frame until presentation frees capacity. It never evicts or skips an
unpresented frame. A separate per-fill budget admits four frames by default and
has a hard configurable ceiling of 32, so a large queue cannot make one call
decode an unbounded video.

A replacement input is opened before the new generation clears current queued
or pending state. After the generation commits, only its reader can admit
frames. The exact input path is immutable within a generation; a different path
requires a newer generation instead of being reported as an idempotent start.
Cancellation, decode failure, timestamp refusal, or adapter failure
clears and terminates that generation. A cancelled fill reports a cancelled
`MediaError` regardless of whether cancellation reaches the reader or a pending
frame admission. The pump creates no threads; the caller
owns scheduling, serialization, and cancellation propagation.

Decoded pixels pass through the sole `makeRenderSourceFrame` adapter and the
bounded presentation buffer. The presentation owner dequeues one immutable
`SourceFrame` and renders it synchronously through the existing
`renderPreviewFrame` or `renderExportFrame` entry point. No resolver performs
decode, conversion, file access, or cache mutation.

## Limits and lifecycle

The default decode ceiling is the renderer's shared 3,840 × 2,160 pixel budget,
not FFmpeg's wider standalone decode default. A pump rejects a decode limit
above that ceiling before opening media. Frame and normalized-byte admission
remain governed by `PresentationVideoBuffer`.

The cursor reuses `SwsContext` through `sws_getCachedContext`; a changed source
format replaces the owned context using FFmpeg's ownership contract. Demux I/O
uses a stable heap-owned interrupt state for the lifetime of `AVFormatContext`.
Packet budgets reset only after a decoded frame is published. Decoder send and
receive both reporting `EAGAIN` without progress is treated as corrupt input.
Cancellation is rechecked after decoder setup, immediately before generation
commit, and after each cursor result before EOS or frame admission is published.

## Test vector and evidence

The fixed base64 QTRLE MOV has SHA-256
`14290e9b2efb26f4ca1e2680b9a7589e141577cea53ceab4b5adf583a98a79e8`.
It contains three opaque 3 × 2 RGB24 frames with stream time base 1/10240 and
PTS values 0, 1024, and 2048. CI writes the embedded bytes to a unique temporary
directory and decodes them with the locked FFmpeg 8.1.2 dependency; CI does not
download or generate the fixture.

Direct reader tests verify all three exact RGBA8 frames, presentation order,
stable EOF, first-frame compatibility, and terminal cancellation. The pipeline
test verifies real decode → adapter → bounded buffer → dequeue → shared CPU and
D3D11 WARP preview/export rendering. A one-frame buffer proves repeated
backpressure retains all three frames. Separate cases prove cancellation and a
generation replacement discard stale queued and pending state.

A green Windows Server 2022 WARP job does not prove seek, VFR timeline mapping,
clock-driven selection, audio synchronization, a physical GPU, hardware decode,
real-time 1080p performance, interactive swap-chain presentation, packaging,
or Windows 10 build 19045 compatibility.

## Primary references

- [FFmpeg send and receive API](https://ffmpeg.org/doxygen/trunk/group__lavc__encdec.html)
- [FFmpeg AVFrame timestamps](https://ffmpeg.org/doxygen/trunk/structAVFrame.html)
- [FFmpeg libswscale](https://ffmpeg.org/doxygen/trunk/group__libsws.html)
