# ADR 0027: Own the Windows project session on one serial runtime

- Status: Accepted for the M2 project integration foundation
- Date: 2026-08-02
- Owner: Windows project lifecycle, Qt editor, and Agent transport

## Decision

`core/project-runtime` is the application-level owner of the active Windows
project. It owns exactly one `ProjectSession` and executes install, query,
mutation, undo, persistence acknowledgement, and snapshot publication on one
dedicated serial worker. Qt and MCP may submit typed operations but may not
retain or call the session directly.

Every installed project has a positive, monotonically increasing generation.
A completed operation publishes that generation with the session revision and
an immutable full-document snapshot. Replacement refuses an older generation
and refuses a dirty active project unless the caller has already obtained an
explicit discard decision. This keeps project switching, stale completion, and
unsaved-change behavior separate from timeline positions or display labels.
Callers that already hold a project identity pass its expected generation into
the typed operation; the runtime checks it inside the same serialized queue item
that performs the query or mutation. A cancellation observed after a domain
commit cannot turn that committed operation into a failure-shaped response.

The runtime bounds pending operations, rejects recursive calls from its owner
thread, observes cancellation before work and at the domain boundary, drains
admitted operations on close, and rejects late admission. No caller may use the
synchronous gateway from the Qt GUI thread; Qt integration must call it from an
owned background operation and publish immutable results back to the GUI.

The standalone Windows MCP executable now installs its project into this
runtime before binding the loopback listener. MCP protocol sessions record the
active project generation at initialization. A project replacement invalidates
the old protocol session instead of silently retargeting its subsequent tools.

## Tests

The runtime test covers mutation and undo publication from one session, dirty
replacement refusal, explicit discard, monotonic generations, queued
cancellation, recursive-call refusal, concurrent close, and late admission.
The existing real-process MCP test continues to prove cross-session timeline
readback and shared undo through the runtime boundary.

## Evidence boundary

This decision establishes the unique runtime owner and moves the standalone MCP
path onto it. It does not yet prove Qt and MCP sharing the same live runtime,
stoppable embedded HTTP lifecycle, project-package persistence, dirty UI
confirmation, or Windows 10 manual behavior. Those are later integration gates.
