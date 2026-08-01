# ADR 0018: Own production audio scheduling in one playback session

- Status: Accepted for M0 injected-device playback
- Date: 2026-08-01
- Owner: Windows media-session engineering
- Applies to: FFmpeg decode executor to persistent WASAPI device worker

## Decision

`AudioPlaybackSession` is the sole playback-generation owner. One serial decode
executor owns `PresentationAudioDecodePump`, its pending immutable block, the
source-sample cursor, the stream-relative output cursor, and lifecycle command
ordering. A separate persistent STA device worker owns all native WASAPI
interfaces, its bounded PCM queue, render waits, and device terminal receipts.

A play or replacement request first obtains the exact device mix format and
buffer size. It opens and prebuffers a candidate reader before changing the
active generation. Candidate open or decode failure preserves the active
reader, pending block, generation, and device stream. After preparation, the
session installs an exact strictly newer device generation; Stop, Reset, queue
clear, and stale one-slot handoff refusal finish before acknowledgement. Only
then does the session replace its active decode owner.

The decode executor retains a dequeued block until the device worker accepts
it. A lifecycle command requests cancellation of the active one-slot handoff;
an unadmitted block is retracted, while an already admitted block returns its
committed receipt. Decode proceeds in bounded fills without polling or sleeps.
Device configuration, reader open, prebuffer fills, and pre-start PCM admission
share that command token. Decode, handoff, and EOS failure discards the active
generation before its terminal receipt, so the device cannot continue rendering
silence behind a failed session. The first admitted source sample is rebased to output sample zero,
and later blocks must remain contiguous in both domains
with checked arithmetic. Coalescing also rejects a time-base change between
constituent blocks. The session preserves that first block's source
presentation timestamp and time base as the common media origin; a cancelled
or refused handoff cannot publish an anchor for PCM the device never accepted.

EOS uses the ordered PCM lane and names the exact final output sample. Session
completion is published only after the device queue and endpoint padding drain.
Successful device Start must return a generation-matched clock sample. The
session anchors that device position and frequency to the requested integer
timeline frame and preserves correlated QPC and precision metadata. A
generation-checked position query combines this immutable anchor with the
latest clock sample already cached by the device worker. It does not call the
native clock from the session executor, and terminal queries preserve their
cancelled, invalidated, or failed outcome.

Close cancels decode ownership and records a separate cancellation terminal for
an active generation, then closes and joins the device worker before publishing
the session close receipt. Terminal receipts are bounded and kept separate from
the close receipt so completed, cancelled, invalidated, and failed generations
remain queryable.

## Tests

The injected end-to-end test decodes the fixed patterned PCM WAV through the
real FFmpeg reader and decode pump, crosses the production worker handoff, and
compares captured device bytes with an independent canonical decode. It proves
1,536 accepted output frames, exact EOS completion, a generation-matched clock
anchor, and same-thread native construction, calls, close, and destruction. It
also proves source-media anchor preservation, non-regressing cached position
reads, stale-generation refusal, and cache clearing across device generations.

Replacement tests hold an active device wait without sleeps. A missing
candidate preserves generation 1 and remains playing. A successful different
input installs exact generation 2, resets captured old PCM, anchors the new
timeline frame, terminates the old waiter, and completes with only replacement
PCM. Invalid and unchanged requests preserve the generation, a changed timeline
anchor replaces the same input, cancellation is independently queryable, and
concurrent close callers observe one native teardown.

## Evidence boundary

A green MSVC build and injected CTest prove executor ownership, generation
barriers, canonical PCM transport, source-aware clock-anchor receipts, cached
running-position reads, cancellation,
replacement, terminal query, and close ordering against the scripted device.
They do not prove audible output, physical endpoint latency, Windows 10 build
19045 behavior, default-device migration, long-run drift, A/V frame selection,
scrubbing, mixing, effects, gapless replacement while candidate preparation is
slow, or distribution readiness.

## Primary references

- [IAudioClient::Start](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient-start)
- [IAudioClient::Reset](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient-reset)
- [IAudioClock::GetPosition](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclock-getposition)
- [Wait Functions](https://learn.microsoft.com/en-us/windows/win32/sync/wait-functions)
