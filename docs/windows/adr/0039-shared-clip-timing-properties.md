# ADR 0039: Keep Trim in shared clip properties

- Status: Accepted for the Windows P0-alpha editor
- Applies to: Clip timing, linked media, persistence, history, and Qt

## Context

The macOS Agent and editor treat duration, source-head trim, source-tail trim,
and speed as fields of `set_clip_properties`. A separate Windows Trim command
would create a second contract and eventually diverge across UI, MCP,
persistence, preview, and Undo/Redo.

## Decision

`ProjectSession::setClipProperties` is the sole Windows owner for the first clip
timing fields: `durationFrames`, `trimStartFrame`, `trimEndFrame`, and `speed`.
It validates the complete request before mutation, computes speed-driven
duration with checked frame arithmetic, and commits one exact project state.
Qt submits selected-clip timing through the same serial `ProjectRuntime` and
background editing executor.

Timing fields propagate to every clip in the same link group. Text partners
inherit duration but retain trim and speed, matching the existing macOS tool.
Nested timelines retain speed and return a skip note because they do not support
retiming. Multicam timing is refused before mutation.

Changing duration applies the existing model invariants to the preserved source
DOM: fade lengths clamp to the new duration, keyframes outside the clip are
removed, empty keyframe tracks clear, and text word timings rescale. Unknown
fields on clips, keyframes, word timings, tracks, timelines, and the project root
remain intact. Save persists the resulting DOM; Undo and Redo restore exact
before and after states. Failed, cancelled, and exact no-op requests do not add
history or invalidate Redo.

The Windows MCP inventory does not expose `set_clip_properties` yet. The current
preview cursor cannot seek nonzero source trims or general speed safely, so
advertising the full macOS tool would claim an edit/playback loop that is not
implemented. MCP exposure follows the complete property schema and seek-capable
preview/export path, not a Windows-only `trim_clips` tool.

## Consequences

- Qt and future MCP property editing reuse one mutation and history owner.
- Linked A/V timing, duration-dependent authored data, persistence, and exact
  history are covered before interactive trim handles are added.
- A timing edit can make the current prototype preview explicitly unavailable
  until source seeking and retiming reach the shared render path.
- Manual Windows UI verification remains required for focus, field editing,
  disabled states, selection changes, Undo/Redo, close, and lifecycle behavior.
