# ADR 0028: Give the embedded MCP server an explicit lifecycle

- Status: Accepted for the M2 Qt integration foundation
- Date: 2026-08-02
- Owner: Windows application lifecycle and Agent transport

## Decision

`HttpServerService` owns the Windows MCP listener and request loop on one
background thread. Construction does not perform network work. `start()` begins
the worker, readiness publishes the actual loopback port, and startup failures
publish a terminal error instead of escaping from detached work.

`requestStop()` signals a service-owned Windows event and requests cancellation.
The worker waits on that event and a Winsock accept event, so it wakes without
polling and remains the only thread that closes the listener. The worker admits
no later connections. A connection already accepted before cancellation checks
the stop token at every receive and send boundary and is also limited by socket
timeouts dynamically reduced to the remaining five-second total deadline.
`join()` waits for that terminal boundary, and destruction performs stop then join.

The Qt application must stop MCP admission and join the server before closing
the shared `ProjectRuntime`. It must not start, stop, join, query the runtime, or
perform socket work on the GUI thread. The standalone MCP executable uses the
same service and retains its readiness and graceful session-close behavior.

## Tests

The service lifecycle test binds an ephemeral loopback port, verifies that an
exclusive competing bind reaches a terminal failure, admits a partial HTTP
request, stops and joins the server, and immediately starts a second service on
the released port. A receive-wait gate proves the partial request reached its
second read boundary, and pure deadline tests cover the per-call ceiling,
remaining milliseconds, sub-millisecond, and expired cases. The test also stops
the second idle listener. The existing real-process MCP test continues to cover
the HTTP and protocol behavior through the standalone wrapper.

## Evidence boundary

This proves deterministic listener shutdown, bounded partial-request
cancellation, and port release under Windows CI. It does not yet prove Qt
ownership, application-close ordering, Windows 10 runtime behavior, or project
persistence.
