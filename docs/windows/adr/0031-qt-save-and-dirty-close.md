# ADR 0031: Own Qt save and dirty-close lifecycle explicitly

- Status: Accepted for the Windows technical MVP
- Date: 2026-08-02
- Owner: Windows project persistence UI
- Applies to: P0

## Decision

`ProjectPersistenceController` is the Qt-facing owner of save state. It tracks
the active package path and project generation, derives dirty state from the
shared runtime mailbox, and invokes the synchronous project package writer only
on a process-lifetime serial background pool. QML never calls filesystem APIs or
the runtime writer directly.

The controller and every admitted writer task retain shared ownership of the
`ProjectRuntime`. Destroying the QObject requests cancellation and disconnects
UI delivery, while the background task keeps its runtime dependency alive until
the writer returns. Teardown therefore never waits on the GUI thread and cannot
leave a writer with a dangling runtime reference.

One admitted Save keeps its package path and project generation snapshot. The
writer revalidates that generation through `ProjectRuntime`; a replaced or
closed runtime cannot be silently saved under the previous UI state. Completion
refreshes dirty state from the authoritative mailbox. A write failure leaves the
runtime dirty and publishes a stable error code and message. A committed write
that cannot acknowledge the runtime, or that is followed by a newer edit,
publishes a distinct warning instead of a clean-success presentation.

Opening another project while the active runtime is dirty is refused before a
new load generation is admitted. Closing a dirty window requires an explicit
Save, Discard, or Cancel choice. Save closes only if the completed write leaves
the current runtime clean; a concurrent newer edit keeps the window open.
Discard is scoped to that close request. An already admitted save is drained
before persistence, preview, MCP, projection, and runtime teardown complete.

## Tests

Qt tests verify that the writer runs off the GUI thread, shutdown waits for a
gated admitted save, successful acknowledgement clears the exact dirty state,
write failure preserves dirty state, refusal without explicit discard remains
observable, and a dirty runtime cannot be replaced by a new project load. QML
offscreen and native-preview tests retain their close-drain coverage with the
new persistence participant.

## Evidence boundary

This slice provides explicit manual Save and window-close protection for the
current safe-edit project operations. Save As and deterministic autosave are
defined by ADR 0035 and ADR 0046. It does not add package-wide media mutation,
crash recovery, direct timeline editing controls, or Windows 10 19045 runtime
certification. The dialog still requires manual Windows UI verification.
