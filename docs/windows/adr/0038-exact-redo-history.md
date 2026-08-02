# ADR 0038: Restore exact project states for Redo

- Status: Accepted for the Windows P0-alpha editor
- Applies to: Shared project history and Qt editing

## Context

Split, Move, and Remove already submit one atomic command to the serial
`ProjectSession` and store exact source-DOM undo snapshots. P0-alpha also needs
Redo without replaying old arguments against a project that may have changed.

## Decision

Undo captures the exact post-action state for the same tracks or timeline before
restoring the pre-action snapshot. That captured state becomes one Redo entry.
Redo restores it directly and preserves the original action ID. It also keeps
generated clip IDs stable and returns the original post-action state identity
while advancing only the monotonic revision.

A successful changed Split, Move, or Remove clears the Redo branch at commit.
Validation failure, cancellation, publication failure, stale project generation,
and exact Move no-op do not clear it. Save and persistence acknowledgement do not
clear history; dirty state remains the comparison between the restored state ID
and the latest persisted state ID.

`ProjectSessionSnapshot` publishes both undo and redo depths. Qt reads those
depths from the shared runtime publication and executes Undo or Redo on the same
single background editing executor. Successful history restoration clears Qt's
clip selection because the selected stable ID may no longer exist.
MCP does not gain a `redo` tool because macOS currently exposes only `undo`.

## Consequences

- Split, Move, and Remove can be undone and redone repeatedly with exact source
  DOM restoration, stable generated IDs, and one publication per history step.
- A changed edit after Undo creates a new branch and makes the old Redo state
  unreachable.
- History remains process-local and is not serialized into `.palmier` packages.
- Manual Windows UI verification remains required for keyboard shortcuts,
  focus, disabled states, selection, close, and lifecycle interaction.
