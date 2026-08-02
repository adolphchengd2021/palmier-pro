# ADR 0036: Move clips through the shared project session

- Status: Accepted for the Windows technical MVP
- Applies to: M2 safe-edit expansion

## Context

The Windows editor already routes Split, Undo, Save, preview projection, and
MCP through one serial `ProjectRuntime`. Moving clips must use the same owner;
separate Qt or MCP mutations would diverge in validation, persistence, and
undo behavior.

## Decision

`move_clips` is one atomic `ProjectSession` operation. Every request is fully
resolved before mutation using stable clip IDs, the active timeline generation,
integer project frames, and compatible destination track types.

A requested frame change propagates the same delta to linked partners while
each partner remains on its own track unless it was named with a compatible
destination track. Conflicting linked deltas, negative results, duplicate
requests, overlapping moved destinations, unstable IDs, and unsafe arithmetic
are refused before commit.

Destination overwrite follows the shared half-open frame model. A supported
unlinked blocker is removed, trimmed, or split as needed; retained source
objects and unrelated fields remain exact. A move that would overwrite a
linked blocker is refused with `unsupportedLinkedOverwrite` until the shared
linked-overwrite operation can update every partner without desynchronizing
the group. Caption and multicam moves remain explicit refusals at this bounded
projection boundary.

An exact no-op returns `changed: false`, keeps the revision and state identity,
does not publish runtime state, and adds no undo entry. A changed batch commits
one revision, one state identity, one publication, and one undo entry. Undo
restores the complete active timeline and its source DOM so track pruning,
clip placement, overwrite pieces, and unknown unrelated fields return exactly.

Qt and MCP call the same runtime method. MCP readback and shared undo require
independent verification, including rejection and exact no-op cases.

## Consequences

- Save and Save As persist the exact moved runtime snapshot through the
  existing project package service.
- Preview and export invalidation observe the same runtime publication as Qt
  and MCP.
- The current slice expands safe editing without claiming multicam, caption,
  linked-overwrite, drag interaction, or full timeline parity.
- Manual UI acceptance remains required for selection, focus, keyboard,
  disabled, cancellation, and close-during-edit behavior.
