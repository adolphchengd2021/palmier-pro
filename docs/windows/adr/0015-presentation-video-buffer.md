# ADR 0015: Bound presentation-ordered video frames by generation

- Status: Accepted for M1 prototype
- Date: 2026-08-01
- Owner: Windows media session
- Applies to: decoded video admission before preview presentation

## Decision

`windows/media-session` owns the first mutable playback boundary. One serial
media worker owns a `PresentationVideoBuffer`; the type creates no threads and
performs no file I/O or decode. A decoded frame is normalized through the sole
`windows/media-render` adapter before it becomes visible to a renderer.

Every load or seek starts a positive, monotonically increasing generation and
clears the prior queue. Results for any non-current generation return a stale
receipt without validation, allocation, or mutation. Cancelling the current
generation clears its frames and permanently rejects later results for that
generation. Repeating `start` for that cancelled generation reports cancelled;
a new generation is required to resume admission.

Accepted frames require a present PTS, a positive and generation-stable time
base, and strictly increasing PTS values. The buffer does not guess missing
timestamps, reorder malformed output, or convert between time bases. It
dequeues only in the order accepted from the decoder.

Frame-count and normalized RGBA32F byte budgets are checked before pixel
adaptation. A full queue returns a structured refusal; it does not evict an
unpresented frame. Adapter validation or cancellation leaves the queue
unchanged. Receipts report operation, outcome, reason, generation, revision,
frame count, and retained bytes.

## Isolation and lifecycle

The buffer is deliberately single-owner and unsynchronized. The future media
session must call it only from its bounded serial executor, then deliver an
immutable frame to the presentation owner. QML, render callbacks, and the UI
thread must not enqueue, decode, adapt, or mutate this buffer. Copy and move
operations are disabled so the mutable owner cannot be duplicated.

Every mutation advances a checked revision. Injected adapter and checkpoint
callbacks are treated as untrusted synchronous boundaries: the enqueue path
revalidates generation, admission state, and revision before it commits. A
reentrant generation switch or cancel wins; any other reentrant mutation makes
the outer enqueue stale. Adapter output must also pass render-source validation
before admission.

Cancellation is checked before adaptation, throughout the adapter, and again
before commit. The adapter and post-adaptation checkpoint are injectable so
these interleavings remain deterministic in tests. A cancelled enqueue reports
`operationCancelled` without cancelling its generation; an explicit generation
cancel reports `generationCancelled`. The owning session must
still revalidate project, media, seek,
playback-rate, output, and lifecycle identity when crossing its own async
boundaries.

## Evidence boundary

Deterministic tests cover limits, monotonic generation, presentation ordering,
missing and non-increasing PTS, changed time bases, exact frame/byte capacity,
pre-cancellation, generation cancellation, repeated no-op cancellation, stale
enqueue/dequeue, reentrant generation and revision changes, invalid adapter
output, and queue preservation after refusal.

This slice does not yet decode multiple real frames, prove a successfully
adapted real-media fixture, seek, select a frame for a clock position, present
to a swap chain, synchronize audio, measure 1080p performance, or run on
Windows 10 build 19045. Those remain Gate G0 work.

## Primary references

- [FFmpeg send and receive API](https://ffmpeg.org/doxygen/trunk/group__lavc__encdec.html)
- [FFmpeg AVFrame timestamps](https://ffmpeg.org/doxygen/trunk/structAVFrame.html)
- [Improving app responsiveness](https://developer.apple.com/documentation/xcode/improving-app-responsiveness)
