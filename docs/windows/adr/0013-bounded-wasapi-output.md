# ADR 0013: Bound the first WASAPI output cycle

- Status: Accepted for M0 prototype
- Date: 2026-08-01
- Owner: Windows audio engineering
- Applies to: event-driven shared-mode output ownership and silent smoke only

## Decision

Keep the existing environment probe as a no-start diagnostic. Add a separate
output seam whose mutable state is owned by one serial audio worker. The state
machine exposes ready, primed, running, stopped, invalidated, failed, completed,
and closed states. The persistent device worker constructs, calls, closes, and
destroys the native stream on its one STA thread. It owns bounded PCM
consumption, underrun counts, buffer leases, control ordering, and structured
receipts. The higher-level control owner supplies exact generation identities.
Callers enqueue precomputed mix-format PCM only through a bounded one-slot
handoff. The shared `PcmFormat` validates encoding, container and valid bits, channel mask,
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
clock position. The control owner installs an exact strictly newer generation.
The audio owner first calls `Stop` when running, then `Reset` when native audio
may be pending and no lease exists, clears queued PCM, installs the requested
identity, and returns to ready. Discard performs the same required engine flush
while preserving the current identity; an empty ready generation is a no-op.
Device, resource, and audio-service invalidation report the current generation
instead of inventing a replacement identity. Reopening and
default-device notification recovery are a later owner-level workflow.

All native setup and output interfaces share `WasapiNativeStream`; the probe and
output path do not duplicate endpoint, format, event, service, or teardown
rules. Its lifetime remains on one dedicated STA thread. An interruptible OS
wait observes the render event and a cancellation event without polling or
sleeping.

The worker uses a bounded priority control lane and a separate PCM lane. A new
control command or PCM handoff cancels the active render wait so the device
thread can process it without polling. Every PCM block carries its generation, first
output sample, frame count, exact `PcmFormat`, and bytes. The worker rejects
stale generations, format drift, non-contiguous samples, invalid sizes, and
overflow before queue admission. End of stream shares the ordered PCM lane and
must name the exact final output sample.

The device thread publishes its latest generation-matched clock sample after
Start and render work. Read-only clock queries return that cached value under
the worker mutex. They never call `IAudioClock` from the caller's executor, and
a generation install or discard clears the cache before acknowledgement.
Runtime failure or invalidation also clears it, and subsequent reads preserve
the terminal classification and HRESULT instead of returning an old sample.

## Tests

Injected tests prove prime-before-start order, checked padding subtraction,
same-thread acquire/release, full and partial silence behavior, transactional
PCM commit, cancellation inside and after a lease, event-before-render order,
underrun versus exact-tail end-of-stream completion, invalidation generation preservation,
pause/resume/reset/no-op transitions, degraded clock receipts, stage/HRESULT
preservation, and stop-before-close teardown.

Worker tests prove that control interrupts a registered render wait, stale
commands cannot mutate a generation, PCM sample discontinuities are refused,
ordered PCM and end of stream produce one terminal completion, setup failures
remain observable, cancellation retracts an unadmitted handoff, concurrent
close joins exactly once, and all stream calls plus construction and
destruction stay on the device thread. They also prove that cached clock reads
reject stale generations, clear across generation installation, and remain
unavailable after worker close.

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

Before production playback, connect the bounded decode executor to the STA
worker, add automatic generation-checked reopen and default-device
notifications, and expose terminal receipts through the session owner. Measure
drift and underruns on a physical Windows 10 fixture rather than inferring them
from this silent smoke.

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
