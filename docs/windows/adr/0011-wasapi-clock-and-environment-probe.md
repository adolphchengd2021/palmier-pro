# ADR 0011: Establish WASAPI clock math and an environment probe

- Status: Accepted for M0 prototype
- Date: 2026-08-01
- Owner: Windows audio engineering
- Applies to: WASAPI shared-mode initialization and audio-clock math only

## Decision

`windows/audio-wasapi` owns two bounded foundations: pure integer conversion
from an `IAudioClock` device position to a timeline frame, and a default render
endpoint capability probe. It owns no application state, PCM queue, callback
thread, playback state machine, media decode, resampling, project state, or UI.

Clock conversion uses the stream-relative device position and the matching
frequency reported by `IAudioClock`. It preserves the correlated QPC sample in
100-nanosecond units, records whether the reading was degraded, uses a stream
generation to reject stale samples, rejects position discontinuities, and
performs floor rounding with checked 128-bit intermediate arithmetic. A stream
reset or replacement must create a new generation and anchor.

The probe owns a dedicated STA thread and releases every interface there, even
when its caller is already in an MTA. It selects the default `eRender` /
`eMultimedia` endpoint, activates
`IAudioClient3`, applies the media category, queries and validates shared-mode
engine periods, initializes the default event-driven period, attaches an
auto-reset event, and obtains `IAudioRenderClient` plus `IAudioClock` and its
frequency. It never calls `IAudioClient::Start` and never writes a buffer.

Only a missing default endpoint, stopped audio service, invalidated device,
invalidated audio resources, device competition, or endpoint creation failure
produces an `unavailable` result, and only at API stages where that result is a
documented environment outcome. The same HRESULT at an internal or unrelated
stage remains a failure. COM model errors, access failures, unsupported
interfaces, invalid arguments, and invariant violations remain failures. Every
terminal result is emitted as one JSON line.

## Tests

The pure clock test covers NTSC floor rounding, an exact frame boundary, zero
delta, degraded QPC metadata, invalid rates and frequency, negative anchors,
stale generations, backward positions, and arithmetic overflow.

An injected session test fixes the exact no-start call sequence and selected
period, proves short-circuiting, and distinguishes external unavailability from
implementation failures. The environment contract also fixes the unavailable
result whitelist, period invariants, and JSON terminal shape. A serial CTest
invokes the real probe with a 30-second timeout. It succeeds when the environment
is either available or explicitly unavailable and fails for all other HRESULTs.

## Evidence boundary

A green Windows Server 2022 job proves compilation against the pinned Windows
SDK, checked clock math, and the injected no-start orchestration contract. The
real probe additionally records an available or stage-classified unavailable
diagnostic; an unavailable result is not independent proof that each native API
was reached. It does not prove that audio was played, that an event callback
fired, that buffer leases are balanced, or that device invalidation recovery
works. It also does not prove PCM conversion, underrun behavior,
pause/seek/reset semantics, long-run A/V synchronization, physical-device
compatibility, or Windows 10 build 19045 runtime behavior.

Before integration, add one owner for the output state machine, a bounded PCM
queue, exact buffer acquire/release scopes, cooperative cancellation, and
generation-checked recovery for default-device changes. Exercise start, pause,
seek, reset, close, invalidation, service restart, no-device, and underrun paths
on an isolated player fixture. Long-run drift must be measured against the
audio clock rather than inferred from these unit tests.

## Primary references

- [IAudioClient3 shared-mode periods](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient3-getsharedmodeengineperiod)
- [InitializeSharedAudioStream](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient3-initializesharedaudiostream)
- [IAudioClient event handle](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient-seteventhandle)
- [IAudioClock position](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclock-getposition)
- [Recovering from an invalid device](https://learn.microsoft.com/en-us/windows/win32/coreaudio/recovering-from-an-invalid-device-error)
