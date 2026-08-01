# ADR 0022: Coordinate one bounded preview presentation owner

- Status: Accepted for M1 background presentation coordination
- Date: 2026-08-01
- Owner: Windows preview session
- Applies to: selected video frames between the headless A/V clock and one HWND

## Decision

`PreviewPresentationSession` is the single owner above the existing headless
A/V session, shared preview renderer, and D3D11 HWND surface. Its synchronous
methods perform decode scheduling, rendering, GPU setup, and presentation, so a
future Qt adapter must invoke the session from one background presentation
executor and never from the UI thread.

One admitted scheduler tick calls `HeadlessAvPlaybackSession::tick` exactly
once. The coordinator copies no clock calculation, PTS cutoff, frame-rate
conversion, or drop rule. It consumes the returned target timeline frame and
latest selected source frame as one immutable render input. The first boundary
accepts one resolved layer at an integer frame rate; unsupported project
composition and fractional rates are refused before playback begins.

The latest selected source frame stays owned by the coordinator until playback
replacement, cancellation, or close. An unchanged source PTS, target timeline
frame, render settings, and surface is coalesced without another render or
Present. If non-blocking Present reports busy or occluded, the complete rendered
frame remains pending and is retried at most once on a later scheduler tick.
There is no polling loop, timer, or recursive retry.

A successful resize only marks cached content dirty. It does not decode or
render inside the resize request; the next background scheduler tick rerenders
the cached source and presents it. A render exception or terminal surface
failure explicitly stops the matching playback generation. Device recreation
requires a new session instead of silently changing the presentation target.

Each admitted operation publishes a stop source before it reaches blocking
work. Cancellation is rechecked before the shared renderer and before Present.
The current shared WARP render call is one bounded, non-interruptible frame;
close waits if cancellation arrives inside that call. Close requests the stop
source before waiting for serialized ownership, closes playback and surface
exactly once, and publishes one stable receipt. A request arriving after the
close gate is refused.

## Tests

Injected deterministic ports prove one playback tick per scheduler tick,
single render and Present ownership, identical-frame coalescing, busy retry
without rerender, completed final-frame retry, resize-driven cached-frame
presentation, settings replacement without a playback generation change,
pre-render cancellation, invalid and stale refusal, terminal surface and render
failure propagation, and close interruption of an admitted tick. A
post-commit playback failure proves the terminal generation replaces the old
video state. Invalid generation zero cannot cancel the admitted first play;
cancel and close preserve surface and playback teardown failures. The close
test uses a stop-token condition-variable gate and no sleep.

## Evidence boundary

A green MSVC `/W4 /WX` build and CTest prove the injected coordination state
machine and compile the real headless, WARP renderer, and HWND surface adapters.
They do not prove Qt integration, visible pixels, timer cadence, physical GPU or
audio output, perceptible A/V synchronization, fractional frame rates,
zero-copy performance, device recovery, or Windows 10 build 19045 behavior.
