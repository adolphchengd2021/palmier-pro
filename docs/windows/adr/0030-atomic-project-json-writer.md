# ADR 0030: Commit project.json from an exact runtime snapshot

- Status: Accepted for the Windows technical MVP
- Date: 2026-08-02
- Owner: Project runtime and Windows project package
- Applies to: P0

## Decision

The first Windows writer requests an immutable save snapshot through the serial
`ProjectRuntime`. The snapshot carries the active project generation, revision,
save snapshot state ID, and the full JSON DOM. Canonical serialization therefore
starts from the same source that UI and MCP mutations update and retains fields
outside the current typed projection.

The synchronous writer is a background-service seam. It validates one existing
`.palmier` directory and its existing `project.json`, serializes at most the
reader's 64 MiB limit, and uses same-volume sibling staging. It writes the
complete file through one uniquely created handle, holds the existing
destination against concurrent writes and replacement, flushes the staging
file, and rechecks its identity, size, and last-write time. The writer then uses
Windows 10 POSIX rename semantics for its single handle-based atomic replacement
while the guard remains open. Cancellation is honored while waiting,
before and during serialization, between write chunks, after flush, and before
the replacement. Once the replacement begins, the result is committed and
cancellation cannot convert it into a failure. Precommit failure removes the
exact staging file and preserves the previous destination.

After the disk commit, the writer acknowledges the saved state ID through the
same runtime without a cancellation token. If newer edits exist, the runtime
records the saved state but remains dirty. If the project was replaced or the
runtime closed, the receipt reports that the disk commit succeeded but runtime
acknowledgement did not; it never returns a failure-shaped result for a completed
replacement.

Writer admission is process-wide and serial in this slice. That is stricter than
the eventual per-package coordinator but prevents two current writes from racing
the same destination.

## Tests

The Windows CTest copies the declared unknown-field package, performs a normal
stable-ID split on a supported sibling clip, saves, closes the runtime, reopens
the package, and independently checks every declared project and media canary,
both split pieces, exact frame timing, persisted dirty state, and the empty
post-restart undo boundary. Deterministic checkpoints cover every precommit
cancellation boundary, cancellation after commit, a newer edit after snapshot,
an excluded concurrent atomic destination replacement, an exclusively locked
file, and staging cleanup.

## Evidence boundary

This proves a bounded `project.json` edit-save-restart slice under MSVC `/W4
/WX`. It does not provide autosave, Save As, package-wide media transactions,
per-package multi-operation coordination, close or quit UI, crash recovery,
external-process locking, `media.json` mutation, or Windows 10 19045 runtime
certification.
