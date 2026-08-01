# ADR 0019: Select presentation frames from the audio clock

- Status: Accepted for M1 injected clock selection
- Date: 2026-08-01
- Owner: Windows media session
- Applies to: presentation-ordered video consumption before preview output

## Decision

The existing single-owner `PresentationVideoBuffer` performs the first atomic audio-clock selection.
The caller supplies the current generation, the exact
buffer revision it observed, a generation-matched device clock anchor and
sample, the editor frame rate, and the first admitted audio source PTS and time
base. The selector creates no threads, performs no I/O, and never calls a native
audio interface.

The device sample maps to an integer timeline frame through the shared checked
audio-clock operation. Independently, the same device delta advances the
preserved audio source origin and is floored into the queued video's stable time
base. This common media origin prevents the first audio and video PTS from being
independently rebased to zero. All products, sums, signed floors, and conversions
use exact 128-bit numerator, denominator, quotient, and remainder arithmetic,
with cross-cancellation before division and exact 256-bit fractional comparison.
Invalid frequency, time base, position regression, or a checked media-time
result outside the signed timestamp domain fails before queue mutation.

A clock before the first queued PTS holds that early frame and does not advance
revision. When one or more frames are due, one operation returns the newest due frame,
drops only the older due frames it supersedes, releases their complete
byte accounting, and advances revision exactly once. The next early frame stays
queued. Empty, held, stale-generation, stale-revision, stale-clock, and cancelled
results never mutate state.

`PresentationVideoDecodePump` forwards this operation and exposes the buffer
revision, so the serial media owner does not reproduce selection math or queue
mutation. FIFO dequeue remains for diagnostic and compatibility tests, but the
clock-driven preview path uses selection.

## Tests

Pure synchronous tests cover exact-anchor selection, early hold, latest-due
selection, multi-frame drop receipts, cross-time-base source origins, negative
fractional floor behavior, a seven-hour 90 kHz clock regression, a full-width
clock-frequency denominator, stale
generation/revision/clock refusal, cancelled
generations, invalid clocks, discontinuity, arithmetic overflow, and revision
overflow without partial consumption. No test uses a sleep, device, or native
clock.

The real three-frame FFmpeg pipeline supplies PTS values at 0, 1,024, and 2,048
in a 1/10,240 time base. Injected matching device samples select each exact
frame through the decode pump before the shared CPU and D3D11 WARP render entry
points.

## Evidence boundary

A green MSVC build and deterministic CTest prove checked clock mapping, atomic
hold/drop/select behavior, generation and revision isolation, real decoded PTS
selection, and continued CPU/WARP rendering. They do not prove a swap chain,
physical endpoint latency, long-run drift correction, playback rate changes,
seek orchestration, interactive synchronized preview, or Windows 10 build 19045.

## Primary references

- [IAudioClock::GetPosition](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclock-getposition)
- [FFmpeg AVFrame timestamps](https://ffmpeg.org/doxygen/trunk/structAVFrame.html)
- [Integer conversion rank](https://en.cppreference.com/w/cpp/language/usual_arithmetic_conversions)
