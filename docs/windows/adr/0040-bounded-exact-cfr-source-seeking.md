# ADR 0040: Seek source trims at the FFmpeg ownership boundary

- Status: Accepted for the Windows P0-alpha editor
- Applies to: Exact-CFR preview, audio playback, and selected-clip H.264 export

## Context

`StaticVideoLayer` already maps a project timeline frame to
`sourceStartFrame + timelineOffset`. Preview and export nevertheless opened
independent sequential readers at source frame zero, so a persisted
`trimStartFrame` could not affect rendered media safely.

## Decision

`DecodeFrameStart` is the single FFmpeg input contract for a source frame index
and exact frame rate. The reader validates the rate before seeking, asks the
demuxer for a keyframe at or before the target, flushes decoder state, and drops
decoded frames only within a fixed budget. The first returned video frame must
have a presentation timestamp exactly equal to the requested integer source
frame. A gap, missing timestamp, VFR rate, exhaustion, cancellation, or seek
failure is terminal and observable.

Audio uses the same source-frame request. It seeks its own stream, resamples on
the audio executor, discards complete pre-target blocks, trims one crossing
block at the exact output sample, and publishes the target source timestamp as
the audio-clock anchor. If the project frame cannot map exactly into both the
output sample rate and source time base, playback is refused.

Preview passes one `DecodeFrameStart` through the headless A/V coordinator to
both decode pumps. H.264 export opens the same seek-capable video reader and
uses `makeRenderPlan` to obtain every expected source frame instead of assuming
that the export loop index is the source frame.

## Consequences

- Nonzero source-head trim preserves the project timeline origin while video
  and audio begin at the same source time.
- Preview and selected-clip H.264 export share the project compiler and source
  mapping without UI or exporter-specific arithmetic.
- Exact-CFR speed 1 is supported; VFR and speed changes remain explicit
  refusals until the shared retiming contract is implemented.
- Tests cover real video seek pixels and PTS, exact audio sample trimming and
  clock anchoring, A/V source alignment, cancellation, Qt candidate selection,
  and trimmed export frame count.
