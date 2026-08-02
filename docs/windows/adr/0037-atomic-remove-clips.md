# ADR 0037: Remove clips through the shared project session

- Status: Accepted for the Windows technical MVP
- Applies to: M2 safe-edit expansion

## Context

The macOS `remove_clips` tool removes stable clip IDs as one action, expands
linked audio/video groups, and prunes empty tracks. Windows must preserve that
behavior without adding a Qt-only or MCP-only mutation path.

## Decision

`remove_clips` is one atomic `ProjectSession` operation. The complete request
is resolved before mutation. Every named clip must exist and have stable
identity; duplicate names are deduplicated. The command expands linked audio/video groups
to every current partner before removal.

The operation removes exact source objects, then prunes empty tracks in the
same active timeline. It does not reinterpret unknown clip fields, so a clip
with fields outside the bounded Windows projection can still be deleted as a
whole. Source/model mismatches, unstable parent IDs, invalid links, overflow,
cancellation, and publication failure are refused before commit.

One changed batch advances revision and state identity once, publishes once,
and creates one undo entry. Undo restores the complete active timeline and its
source DOM, including removed linked clips, pruned tracks, stable IDs, and
unknown unrelated fields. The receipt lists every removed stable clip ID and
notes when pruning changed track indexes.

Qt and MCP call the same `ProjectRuntime::removeClips` method. Verification
requires independent MCP readback, shared undo, Save/restart proof, rejection
without mutation, cancellation, and exact source restoration.

## Consequences

- Remove is durable through the existing package writer, Save, and Save As.
- Preview and export invalidation consume the same runtime publication.
- Remove does not claim ripple close, track management, caption-group removal,
  multicam-group removal, or arbitrary property editing.
- Manual Windows UI verification remains required for selection, focus,
  keyboard deletion, disabled state, cancellation, and close during editing.
