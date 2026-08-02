# ADR 0042: Replace the A/V generation for bounded playhead seek

- Status: Accepted for the Windows technical MVP
- Applies to: Exact-CFR project preview seek and frame stepping

## Context

Exact source-head seeking and same-generation pause already align one clip's
video, audio, project frame, and visible cache. The Qt transport still could not
move to another frame without replaying from the clip start. Starting audio and
then immediately pausing is not a valid frame-step implementation because the
device may advance before Stop reaches it.

## Decision

A seek validates the current playback generation and the compiled layer's
end-exclusive timeline interval before doing media work. The target source frame
is `sourceStartFrame + (targetTimelineFrame - timelineStartFrame)` with checked
integer arithmetic. Invalid, stale, negative, or out-of-range requests are
refused; the implementation never silently clamps or retargets them.

The headless A/V owner prepares one newer exact generation. Playing seek starts
the new WASAPI generation and continues cadence. Paused seek prebuffers and
primes source-aligned PCM without starting WASAPI or publishing a clock anchor.
Its first exact decoded video frame is removed from the bounded presentation
queue, rendered through the shared project RenderPlan path, and presented while
the new generation remains paused. Resume starts that prepared generation and
establishes its source-aware clock anchor at the requested project frame.

The Qt controller keeps only the latest seek intent and allows one background
command at a time. A seek interrupts one admitted tick. Direct seek preserves
playing or paused intent; Previous Frame and Next Frame always request paused
mode. The controller publishes the exact current frame and inclusive UI range.

## Consequences

- Preview seek and frame step reuse the same exact-CFR source mapping as trim
  playback and selected-clip export.
- Every changed seek replaces generation; stale completions cannot mutate the
  later target.
- A paused step cannot emit audible samples before Resume.
- Continuous scrub, VFR, source-rate conversion, speed changes, multi-clip
  timeline navigation, and physical-device A/V drift remain outside this slice.

## Evidence and limits

Deterministic tests cover paused preparation without Start, first-Resume clock
anchoring, exact first-frame PTS, playing and paused generation replacement,
stale and cancelled requests, end-exclusive bounds, active-tick interruption,
frame-step pause semantics, and Qt current-frame publication. Hosted CI does not
prove visible pixels, audible hardware, sustained synchronization, continuous
dragging, Windows 10 build 19045, or device recovery.

## Manual UI verification

On Windows 10 19045 with a supported H.264/AAC project, wait for playback and
choose Next Frame. Audio must stop and the preview must advance by exactly one
frame. Choose Previous Frame and verify the exact prior frame. Enter the first,
middle, and final valid frame and choose Seek while paused; each frame must
appear without audio. Resume, seek again, and verify playback continues from the
new frame. Attempts before the first or after the final frame must not move the
preview. Repeat seek rapidly, resize during a paused frame, then close during an
active seek. No blank frame, audible step, stale target, hang, or late playback
after close is acceptable.
