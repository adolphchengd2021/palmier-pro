# ADR 0020: Coordinate injected headless A/V playback

- Status: Accepted for M1 headless coordination
- Date: 2026-08-01
- Owner: Windows media session
- Applies to: production audio and decoded-video lifecycle before presentation

## Decision

`HeadlessAvPlaybackSession` is the single A/V generation and lifecycle owner.
It creates no timer, presenter, thread, or executor. Its synchronous media work
must be called from a background coordinator executor; a future Qt surface must
not call it from the UI thread.

Replacement first opens a separate `PresentationVideoDecodePump` candidate and
performs one bounded prefill. The active generation is unchanged if video
preparation fails. The coordinator then calls the audio session's
exact-generation entry point. Audio preparation or install refusal destroys
the candidate and preserves the active A/V generation. After audio commits,
the remaining video commit is a no-fail `unique_ptr` swap.

Audio may still reach a terminal handoff, EOS, or device-start failure after
its exact generation has committed. That receipt advances the coordinator to
the committed generation, terminates the matching video candidate, and leaves
the next generation usable; it never revives the old video beside newer audio.

One tick reads `AudioPlaybackSession::position` exactly once. It validates the
generation and passes that immutable anchor, sample, source PTS, source time
base, and stored editor frame rate to `PresentationVideoDecodePump::select`.
The coordinator does not call the audio timeline helper, calculate a PTS cutoff,
clamp time, or maintain a second timing rule.

Selection runs before each bounded fill. A tick stops on an early frame, video
EOF, no progress, cancellation, stale state, or the configured fill-call
ceiling. If multiple frames become selected during catch-up, only the newest is
returned; earlier selections and selector-reported superseded frames contribute
to one explicit drop count. The default ceiling is two fill calls and the hard
ceiling is four; each pump fill retains its existing 1-32 frame bound.

Video EOF freezes presentation ownership outside this headless boundary while
audio continues. Audio is the master terminal. Completion without a final
cached sample does not extrapolate from QPC or wall time. Cancellation,
invalidation, failure, and close clear the current video decode generation.
Repeated or concurrent close reaches the audio owner once and returns the same
receipt.

Each admitted play or tick publishes an operation stop handle behind a narrow
admission mutex before blocking work. Cancel and close request it before waiting for the state mutex.
The exact audio command receives that operation's stop token and binds it only
after installing its own handoff source, so cancellation cannot stop the old
generation or disappear between queue admission and command start. Close first
publishes a permanent admission gate under the same mutex. A cancellation also
publishes its expected generation there, so an entry that already owns editor
state but has not installed its stop handle cannot escape the request, and no
later operation can pass that pending lifecycle barrier. All later play, tick,
and cancel calls are refused after close. The operation rechecks
cancellation after every preparation phase; overlapping close may linearize
after a completed commit, then closes it.

## Tests

Deterministic tests use a scripted audio port and real fixed FFmpeg video
fixtures. They cover one exact generation, one clock read per tick, stale tick
refusal, video- and audio-side replacement failure preservation, bounded
three-frame catch-up, latest-frame delivery and drop accounting, stable video
EOF while audio plays, missing clock samples, audio completion and failure,
stale and repeated cancellation, invalid limits, and concurrent exactly-once
close. A gated audio preparation proves close interrupts an admitted play
without a sleep. A post-commit audio failure proves the next generation remains
usable. No test sleeps, opens a window, starts WASAPI, or uses a swap chain.

The existing `AudioPlaybackSession` test separately proves that requested exact
generations are accepted only when they match its next device generation.

## Evidence boundary

A green MSVC build and CTest prove the headless command, generation, bounded
scheduling, real video decode, clock handoff, and terminal contracts. They do
not prove a timer cadence, interactive presentation, a physical audio endpoint,
perceptible A/V synchronization, final endpoint position, long-run drift,
device recovery, or Windows 10 build 19045.

## Primary references

- [IAudioClock::GetPosition](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclock-getposition)
- [Task cancellation](https://www.swift.org/documentation/server/guides/libraries/concurrency-adoption-guidelines.html#task-cancellation)
