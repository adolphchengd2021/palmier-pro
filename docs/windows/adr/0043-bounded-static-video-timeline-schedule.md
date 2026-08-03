# ADR 0043: Compile one bounded static video timeline schedule

- Status: Accepted for the Windows technical MVP
- Applies to: Exact-CFR project preview and export scheduling

## Context

Project preview and H.264 export currently compile one selected static video
clip. The Qt projection orders candidate clips, but returns only the first
available source. Preview and export therefore have no shared authority for
clip transitions, leading or inter-clip gaps, or end-exclusive timeline
completion.

## Decision

`core/project-render` is the sole owner of a bounded `StaticVideoTimeline`.
Compilation accepts one persisted timeline ID and returns immutable canvas and
frame-rate settings, an end-exclusive duration, and at most 256 visible static
video segments. Segments are stable-sorted by start frame, persisted track
order, then persisted clip order. Clip IDs must be persisted and unique across
the complete schedule so media resolution and transition receipts are unambiguous.

The timeline domain is `[0, lastSegmentEnd)`. Adjacent segments are valid and
switch ownership at the exact shared boundary. Any overlap between visible
non-audio clips is refused before media work. Hidden tracks and audio-only
clips do not enter the video schedule. A visible non-audio clip that cannot be
compiled by the existing static video compiler is an explicit refusal; it is
never silently omitted.

`staticVideoLayerAt` resolves the active stable clip identity for a frame.
`makeRenderPlan(StaticVideoTimeline, frame)` delegates active frames to the
existing single-layer mapping. A leading or inter-clip gap produces the same
validated canvas and timeline frame with zero layers, which renders as opaque
black. Frames before zero or at and after the schedule duration are refused.

The 256-segment bound makes compilation and lookup resource use explicit while
the compiler still resolves each segment against the retained full project DOM.
It is a technical-MVP capacity limit, not a product project limit.

## Consequences

- Preview and export can consume one ordered identity and frame-mapping source.
- Gaps do not acquire media, invent source frames, or extend the final frame.
- Exact clip transitions can replace the A/V generation without positional IDs.
- Overlapping composition, speed changes, VFR/source-rate conversion, text,
  images, nested timelines, and dynamic effects remain explicit refusals.
- Projection, preview-session, and export-session owners must resolve media for
  every scheduled segment before claiming full timeline playback or export.

The Qt project projection resolves every scheduled segment by stable clip ID
and publishes no partial candidate. Its presentation adapter starts the first
segment, atomically replaces the A/V generation at an adjacent clip boundary,
and routes direct seek to the active scheduled source. The controller refuses
gap seek and step targets before background admission. Leading and inter-clip
gap cadence remain a later step.
The underlying presentation session can atomically seek to a different input
and compiled layer, including paused preparation without starting audio. This
is the generation-safe transition primitive for the timeline owner.

## Evidence and limits

Deterministic compiler tests cover stable ordering, leading and inter-clip
gaps, adjacent end-exclusive transitions, source mapping, black gap rendering,
overlap, unsupported visible content, cancellation, and the segment bound.
Qt projection tests cover all-source publication and refusal when a later
scheduled source is unavailable. Presentation tests cover atomic cross-source
mapping, while Qt controller tests cover acceptance of a newer transition
generation and continued bounded cadence. The H.264 exporter consumes the same
schedule, resolves every source by stable clip ID, and renders gaps through its
zero-layer plan; see ADR 0044.
These tests do not prove A/V generation replacement on physical devices,
continuous gap timing, visible UI
pixels, physical audio/GPU synchronization, VFR, or Windows 10 build 19045.
