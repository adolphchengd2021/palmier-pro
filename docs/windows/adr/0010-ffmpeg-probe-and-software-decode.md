# ADR 0010: Isolate FFmpeg probe and stateful software decode

- Status: Accepted for M0 prototype
- Date: 2026-08-01
- Owner: Windows media engineering
- Applies to: FFmpeg 8.1.2 probe and sequential software decode only

## Decision

Activate vcpkg manifest mode at the locked `2026.06.24` baseline and build
FFmpeg 8.1.2 as dynamic LGPL libraries. Disable default features and request
only `avcodec`, `avformat`, and `swscale`. GPL and nonfree feature groups remain
outside the prototype contract.

`windows/media-ffmpeg` owns local-file container probe and a stateful cursor for
frames emitted by the software decoder. It preserves integer stream time bases,
rotation metadata, color metadata, alpha, audio stream shape, and explicit
failure codes. It owns no project state, timeline decisions, UI state, audio
output, playback clock, hardware decode, export, or cache.

The API is synchronous by design and must be called from a dedicated media
executor. Its `AVIOInterruptCB` observes a `std::stop_token`; cancellation is
also checked between probe, packet, decoder, conversion, and return boundaries.
Each cursor owns its FFmpeg contexts and reusable swscale context.
`decodeFirstVideoFrame` is a compatibility wrapper over that cursor. No
process-global log callback or shared codec context is installed.

The first conversion gate accepts only explicit BT.709 primaries, sRGB
transfer, RGB matrix input. Packed RGB with unspecified range is treated as
full-range because no YUV range expansion occurs. Other color combinations and
non-cardinal or non-pure display matrices are hard refusals, not guessed
fallbacks. Output is RGBA8 plus an explicit opaque, straight, premultiplied, or
unspecified alpha mode. Unknown alpha is preserved as unknown, so project
integration must refuse or normalize it explicitly. The output is not yet wired
into the float RenderPlan.

## Test vectors

Tests embed fixed base64 vectors and write them into a unique temporary
directory at runtime. They do not download or generate media in CI.

- A 4×4 single-frame QTRLE MOV carries asymmetric alpha/color channel values,
  BT.709/sRGB/RGB metadata, and a 90-degree display matrix. Its alpha mode is
  intentionally unspecified. It verifies exact pixels, time base, alpha-mode
  reporting, and rotation. SHA-256:
  `34b49ec505ec14f3373e3397a4c658d462d329a21c982b6feac5274a2ee02dcb`.
- A 16×16 H.264/AAC MP4 carries a 10 fps video stream and 48 kHz stereo audio.
  It verifies real container and stream discovery. SHA-256:
  `d7eb5181b706b60b3f3f4e572a72e275f69ad42815c77d76b909f2b17ff77c82`.
- A mono 8 kHz PCM WAV verifies that video decode reports `noVideoStream`.
  SHA-256: `207503465701ec21fedf076c87748252e66de71d2c8fc8ab5a4c5dffffc05457`.
- A three-frame 3×2 opaque RGB24 QTRLE MOV verifies exact sequential pixels,
  time base, PTS order, stable EOF, and cursor cancellation. SHA-256:
  `14290e9b2efb26f4ca1e2680b9a7589e141577cea53ceab4b5adf583a98a79e8`.

The vectors were generated locally with fixed FFmpeg filter and codec settings.
They are test data only; the linked prototype dependency remains the locked
vcpkg FFmpeg build.

## Evidence boundary

A green Windows Server 2022 job proves the exact dependency can compile, load its
DLLs, probe H.264/AAC, and software-decode the lossless reference frames. It does
not prove Windows 10 build 19045 compatibility, D3D11VA, real-time 1080p
playback, audio output, long-run synchronization, physical GPU behavior,
packaging, or distribution approval.

Before distribution, archive matching FFmpeg source, build flags, DLL hashes,
license notices, and SBOM data; complete codec patent review. WASAPI transport,
hardware decode, project integration, and performance measurements remain
separate gates.

## Primary references

- [vcpkg CMake integration](https://learn.microsoft.com/en-us/vcpkg/users/buildsystems/cmake-integration)
- [Locked FFmpeg port manifest](https://raw.githubusercontent.com/microsoft/vcpkg/cd61e1e26a038e82d6550a3ebbe0fbbfe7da78e3/ports/ffmpeg/vcpkg.json)
- [FFmpeg demuxing API](https://ffmpeg.org/doxygen/8.0/group__lavf__decoding.html)
- [FFmpeg send and receive API](https://ffmpeg.org/doxygen/trunk/group__lavc__encdec.html)
- [FFmpeg legal considerations](https://ffmpeg.org/legal.html)
