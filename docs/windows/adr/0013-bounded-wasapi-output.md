# ADR 0013: Bound the first WASAPI output cycle

- Status: Accepted for M0 prototype
- Date: 2026-08-01
- Owner: Windows audio engineering
- Applies to: event-driven shared-mode output ownership and silent smoke only

## Decision

Keep the existing environment probe as a no-start diagnostic. Add a separate
output seam whose mutable state is owned by one serial audio worker. The state
machine exposes ready, primed, running, stopped, invalidated, failed, completed,
and closed
states. It owns generation changes, bounded PCM consumption, underrun counts,
buffer leases, control ordering, and structured receipts. Callers may enqueue
precomputed mix-format PCM only through the future serial worker façade. The
shared `PcmFormat` validates encoding, container and valid bits, channel mask,
block align, sample rate, channel count, and interleaving before construction;
the prototype queue remains private and may not be called across threads.

Every successful nonzero `GetBuffer` has exactly one same-thread
`ReleaseBuffer`. PCM is copied from a preallocated bounded queue and committed
only after `ReleaseBuffer` succeeds. Cancellation before acquire performs no
lease. Cancellation after acquire but before release uses
`ReleaseBuffer(0, 0)` and preserves the queued PCM. Cancellation observed after
successful release reports the committed effect with `lateCancellation`; it
does not return a false cancelled outcome.

The first start validates padding, fills all available frames, then calls
`Start`. A partial packet zeroes its tail, while a fully empty packet uses
`AUDCLNT_BUFFERFLAGS_SILENT`. Runtime shortage increments an underrun counter
unless the queue has reached end of stream. At end of stream, the last lease is
exactly the remaining media-frame count. Once the queue is empty, the worker
waits for endpoint padding to reach zero, stops the client, and enters
completed. Padding greater than the engine buffer is an invariant failure and
is never clamped or subtracted.

Pause calls `Stop` without reset, preserving the stream generation and native
clock position. Reset is accepted only from stopped, primed, or completed, calls the native
reset while no lease exists, clears queued PCM, increments the generation, and
returns to ready. Device, resource, and audio-service invalidation also advance
the generation, clear old PCM, and make the old stream unusable. Reopening and
default-device notification recovery are a later owner-level workflow.

All native setup and output interfaces share `WasapiNativeStream`; the probe and
output path do not duplicate endpoint, format, event, service, or teardown
rules. Its lifetime remains on one dedicated STA thread. An interruptible OS
wait observes the render event and a cancellation event without polling or
sleeping.

## Tests

Injected tests prove prime-before-start order, checked padding subtraction,
same-thread acquire/release, full and partial silence behavior, transactional
PCM commit, cancellation inside and after a lease, event-before-render order,
underrun versus exact-tail end-of-stream completion, invalidation generation changes,
pause/resume/reset/no-op transitions, degraded clock receipts, stage/HRESULT
preservation, and stop-before-close teardown.

A separate serial CTest performs one bounded native silent cycle: setup, prime,
start, wait up to two seconds for a render event, sample the clock, stop, reset,
and close. A documented missing endpoint or stopped/invalidated service remains
an unavailable environment result. Other failures fail the test.

## Evidence boundary

A green injected test proves the state, exact PCM format, and lease contract. A successful
native silent smoke proves that those APIs cooperated on that CI host; an
unavailable result proves only classification. Neither result proves audible
PCM, physical channel mapping, media playback, device-change recovery,
default-device migration, long-run underrun behavior, A/V synchronization,
physical hardware compatibility, or Windows 10 build 19045 behavior.

Before production playback, add the bounded decode-executor to STA-worker
handoff, automatic generation-checked reopen, default-device notifications,
and a queryable asynchronous terminal status. Measure drift and underruns on a
physical Windows 10 fixture rather than inferring them from this silent smoke.

## Primary references

- [Rendering a stream](https://learn.microsoft.com/en-us/windows/win32/coreaudio/rendering-a-stream)
- [GetCurrentPadding](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient-getcurrentpadding)
- [GetBuffer](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudiorenderclient-getbuffer)
- [ReleaseBuffer](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudiorenderclient-releasebuffer)
- [Start](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient-start)
- [Stop](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient-stop)
- [Reset](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient-reset)
- [GetPosition](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclock-getposition)
- [Recovering from an invalid device](https://learn.microsoft.com/en-us/windows/win32/coreaudio/recovering-from-an-invalid-device-error)
