# ADR 0024: Compile one static project video layer once

- Status: Accepted for the M1 project-render boundary
- Date: 2026-08-02
- Owner: Cross-platform project/render core
- Applies to: persisted project clip properties used by Windows preview and export

## Decision

`core/project-render` is the sole first-stage compiler from one persisted video
clip to an immutable render-layer template. It consumes the existing read-only
`ProjectDocument`, resolves timeline, track, and clip by stable persisted ID in
both the normalized model and retained full JSON DOM, and refuses missing,
duplicate, or synthesized identities. Qt and the preview coordinator do not
parse transform or effect JSON and do not construct a second RenderPlan rule.

The compiler accepts safe integer clip ranges at exact 1x speed. It maps static
center, size, clockwise rotation, opacity, normal source-over, and zero or one
enabled static `color.exposure` effect. Each requested timeline frame is
revalidated against the clip interval, then maps to `trimStartFrame +
timelineFrame - clipStartFrame` with checked arithmetic. Preview and export call
the same `makeRenderPlan` operation.

The compiler defines nonzero trim mapping, but the first application candidate
and preview session still refuse nonzero trim until FFmpeg seek and the audio
clock share that source anchor. The preview session completes at the compiled clip out-point and
preserves the last valid frame instead of treating the next source tick as a
render failure.

The retained full DOM is required because the read-only normalized project
projection intentionally does not yet expose every visual property. This is
not a second general project decoder: the compiler reads only render eligibility
and static render values, while project structure, defaults for existing fields,
and source retention remain owned by `core/project`.

Crop, edge masks, horizontal or vertical flip, fades, active visual or effect
keyframes, non-normal blend modes, multiple enabled effects, unsupported effect
types, and exposure outside -3 through 3 are explicit refusals. Disabled effects
do not affect rendering. A refusal prevents the candidate from starting and is
reported by its stable compiler error code; unsupported data remains untouched
in the source DOM.

Malformed visual values are also explicit refusals rather than Swift-compatible
defaulting because rendering an identity value would hide a visible project
error. Qt candidate selection cannot skip a refused earlier visual or timing
contract for a later clip, and it refuses another non-audio visible layer
overlapping the selected interval.

## Tests

The compiler test proves persisted-ID resolution, timeline-to-source frame
mapping with trim, transform, opacity, normal blend, exposure, preview/export
render-entry-point pixel identity, cancellation, inactive-frame refusal,
malformed-value refusal, and negative visual boundaries. The D3D11 test compiles
project JSON into the same plan, compares CPU and WARP pixels within the existing
tolerance, and requires bit-identical WARP output from the preview and export
render entry points. Qt projection tests prove the immutable template is
transported into the background preview owner without skipping refused or
overlapping visuals.

## Evidence boundary

A green MSVC `/W4 /WX` build and CTest prove one static single-video-layer
project mapping and shared CPU/WARP preview/export behavior. They do not prove
multi-layer composition, crop, flip, animated properties, fades, text, nesting,
arbitrary effects, encoding, Swift pixel parity, visible UI pixels, physical GPU
performance, physical-device A/V synchronization, or Windows 10 build 19045.
There is no production export planner or encoder in this batch; export parity is
limited to the shared renderer entry-point test until that application path is
implemented.
