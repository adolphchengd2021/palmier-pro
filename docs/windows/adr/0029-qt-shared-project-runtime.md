# ADR 0029: Share one project runtime between Qt and MCP

## Status

Accepted for the Windows technical MVP.

## Decision

The Qt process owns one `ProjectRuntime`. Project loading reads and validates a
candidate off the GUI thread, then installs it through that runtime before the
Qt model commits. The embedded loopback `HttpServerService` receives the same
runtime reference, so UI reads and MCP edits cannot diverge into separate
mutable project owners.

Cancellation and injected delivery checks run before `ProjectRuntime::install`.
Once install returns, its publication is authoritative and the load cannot be
reported as cancelled. A newer load may replace that generation, but the GUI
defers an installed older generation while that newer candidate is pending. If
the newer candidate fails or is cancelled, the deferred current-runtime
projection becomes the displayed model while the newer load error remains
visible. The active projection's committed state, warnings, and structured
error are retained separately so cancelling a later load restores a complete
diagnostic baseline.

`ProjectRuntimeMailbox` is the observer boundary. It owns no `QObject` and
publishes the latest immutable session snapshot with a monotonically increasing
publication token. The token, rather than `stateId`, rejects stale work because
undo can restore a lower state ID and persistence acknowledgement can publish
the same revision and state ID with a different `persistedStateId`.

`ProjectRuntimeProjectionBridge` polls the mailbox on the GUI thread and runs at
most one projection on a dedicated serial pool. A newer content identity
replaces the pending snapshot and cancels the current projection. The GUI
applies a result only while its publication token is still the mailbox latest.
Persistence-only publications do not rebuild the timeline.

A newly observed runtime revision immediately publishes an `invalidated`
preview for the same project generation and revision. This cancels active
playback before the refreshed timeline projection arrives. Initial project
loading may resolve that invalidation with the media-backed preview at the same
generation and revision.

Shutdown rejects new loads, requests MCP stop, drains project loading, preview,
and projection work through nested event-loop notifications, joins MCP and the
runtime off the GUI thread, and only then destroys their owners.

## Boundaries

This decision proves one in-process owner, stale projection rejection, preview
invalidation, and stoppable embedded MCP lifecycle. It does not add project
writing, restore dirty projects after restart, expose MCP service state in QML,
or approve Windows 10 distribution.
