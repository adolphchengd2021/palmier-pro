# ADR 0041: Same-generation preview pause and resume

- Status: Accepted for the Windows technical MVP
- Applies to: Interactive Windows preview transport

## Context

The Qt preview previously started automatically and advanced until completion.
WASAPI already supported exact-generation Stop and Start, but no higher owner
could pause without cancelling playback, clearing the visible frame, and
creating a replacement generation.

## Decision

`AudioPlaybackSession` serializes Pause and Resume beside Play, Cancel, and
Close. Pause stops the exact active WASAPI generation and suspends decode
handoff. Resume starts that same generation. Neither command changes the clock
anchor, source mapping, decoded cursor, queued PCM, or accepted-frame count.
Repeated commands are no-ops; zero, stale, terminal, or closed requests are
refused without touching the device.

`HeadlessAvPlaybackSession` retains the video pump while paused and does not read
the audio clock or select video frames. `PreviewPresentationSession` retains the
compiled layer, selected source frame, pending rendered frame, and HWND content.
Pause does not clear the surface.

The Qt controller admits only one background command. A Pause request cancels
one admitted scheduler tick, then pauses the matching source serial and playback
generation before any later tick. Resume keeps both identities and restarts the
existing completion-triggered cadence. Project replacement, Cancel, and Close
remain authoritative lifecycle operations.

## Evidence and limits

Deterministic tests cover same-generation state, repeated and stale controls,
paused clock suppression, cached-frame retention, active-tick interruption,
resume cadence, cancel from paused state, and close. Hosted CI does not prove
audible physical-device behavior, visible UI interaction, long-run A/V drift,
scrubbing, arbitrary playhead seek, Windows 10 build 19045, or device recovery.

## Manual UI verification

On Windows 10 19045 with an audio endpoint, open a project containing one
supported H.264/AAC clip and wait for visible, audible playback. Choose Pause;
the image and audio must hold without clearing or advancing. Choose Resume; the
same clip must continue without jumping to its start. Repeat both controls,
resize while paused, then close while paused and while playing. No duplicate
audio, blank frame, stale control, hang, or late playback after close is
acceptable.
