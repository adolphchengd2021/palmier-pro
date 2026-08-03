# ADR 0044: Export the bounded static video timeline to H.264

- Status: Accepted for the Windows technical MVP
- Applies to: Video-only exact-CFR H.264 export of the active timeline

## Context

The first Windows exporter accepts one selected static video clip. The shared
project compiler now owns a complete ordered `StaticVideoTimeline`, including
stable clip identity and black leading or inter-clip gaps. Export must consume
that schedule directly instead of rebuilding timing or placement rules.

## Decision

`windows/export-ffmpeg` accepts the complete compiled schedule plus exactly one
absolute local input mapping for every stable scheduled clip ID. Missing,
duplicate, extra, relative, or destination-aliasing mappings are refused before
staging. The package workflow compiles the active timeline first, resolves every
segment through the shared media manifest resolver, and publishes no partial
request.

One serial export owner walks `[0, timeline.durationFrames)`. It opens at most
one bounded FFmpeg source cursor at a time and replaces that cursor at each
segment boundary using the segment's exact source start and frame rate. Every
decoded timestamp must equal the source frame returned by the shared
`makeRenderPlan(StaticVideoTimeline, frame)` operation. Gap frames acquire no
media and render the schedule's zero-layer plan as black.

All output frames use zero-based consecutive PTS with duration one. The locked
`h264_mf` encoder, even canvas, exact CFR, BT.709 conversion, independent probe
and full decode verification, sibling staging, durable flush, exact-object
installation, cancellation, and cleanup contracts from ADR 0025 remain
unchanged. The selected-clip API is retained as a compatibility wrapper over a
one-segment schedule; it does not own a second encoding implementation.

## Consequences

- Preview and export share segment eligibility, ordering, source mapping, gap,
  and end-exclusive timing rules.
- A later unavailable source prevents all encoding and cannot produce a
  plausible partial movie.
- Source readers and decoded frames are bounded to the active segment instead
  of retaining every timeline source.
- Audio muxing, overlapping composition, H.265, speed, VFR, progress reporting,
  and export queueing remain outside this decision.

## Evidence boundary

Contract tests must refuse incomplete and ambiguous source maps before staging.
The native encoder test must export two clips separated by leading and
inter-clip gaps, independently decode every output frame, and distinguish black
gap pixels from visible source pixels. The package workflow must resolve both
sources and refuse a missing later source. These automated checks do not prove
physical Windows 10 encoder availability, visual parity with the shipping macOS
compositor, audio export, visible Qt interaction, or release approval.
