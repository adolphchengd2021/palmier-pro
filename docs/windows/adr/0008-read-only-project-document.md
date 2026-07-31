# ADR 0008: Read-only project document and normalized projection

## Status

Accepted for the M0 Windows prototype.

## Decision

The first Windows project path parses `project.json` into one strict, full JSON
DOM and derives a typed, read-only projection from that DOM. The full DOM is the
source of truth for data not yet represented by the projection.

The reader accepts the current `ProjectFile` root and the legacy bare
`Timeline` root. A present `timelines` key always selects the current root; an
invalid current root never falls back to legacy decoding.

Missing timeline, track, and clip IDs use an injected generator. Each projected
ID records whether it was persisted or synthesized. Synthesized IDs are not
written back by this prototype.

Required top-level and timeline fields, required types, supported enums, signed
64-bit bounds, and positive timeline dimensions stop the read. Clip data errors
inside Track's optional `clips` collection follow Swift's `try?` behavior: the
collection becomes empty and emits a diagnostic. An injected ID generator
failure is infrastructure failure and always remains fatal. Invalid active and
open timeline references are normalized for the derived session view and
reported. Duplicate IDs and unsafe frame ranges are reported without mutating
the source DOM.

The machine-readable boundary is
`contracts/project/v1/reader-projection.json`. `projectionComplete` remains
false: clip effects, transforms, fades, text, keyframes, view state, speakers,
multicam data, media metadata, and other known or future fields remain available
only through the full source DOM in this milestone.

## Consequences

- The prototype can inspect real current and legacy fixtures without losing
  unknown JSON values in memory.
- Canonical source replay proves parser retention, not save compatibility.
- No Windows writer or package mutation is permitted yet.
- A future writer must merge changes into the full DOM and pass cross-platform
  load-edit-save-reopen canaries before the read-only disposition can change.
