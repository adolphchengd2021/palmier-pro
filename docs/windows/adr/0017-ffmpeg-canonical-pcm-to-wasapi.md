# ADR 0017: Normalize FFmpeg audio before bounded WASAPI output

- Status: Accepted for M0 deterministic integration
- Date: 2026-08-01
- Owner: Windows media and audio engineering
- Applies to: injected real-media PCM path, not physical-device playback

## Decision

`core/audio` owns the platform-neutral interleaved `PcmFormat`. Sample rate,
integer or IEEE-float encoding, channel count, container and valid bits, block
align, channel mask, and interleaving are one immutable identity. FFmpeg and
WASAPI reuse this type; neither side infers a format from block align or bit
depth alone. A multichannel format must carry an explicit channel mask; only
mono and stereo may use an unspecified mask.

`FfmpegAudioFrameReader` reuses the same receive-before-supply codec driver as
video. It owns one decoder and one `SwrContext`, refuses a source-format change,
converts to signed 16-bit or float 32-bit interleaved PCM, drains the resampler with null input
after codec EOF, and maps the first valid source PTS
to the target sample domain with floor rounding. Later blocks advance by the
actual converted sample count and must remain continuous.

`PresentationAudioDecodePump` runs on one serial media executor. It retains at
most one decoded block and slices it into a generation-aware bounded queue. A
replacement reader must open before the active reader, pending remainder, and
queue are cleared. Cancellation and conversion failure are terminal for only
the current generation. Configuration is capped at 4,194,304 queued frames,
65,536 admitted frames per fill, and 256 MiB of queued PCM.

This batch intentionally does not create threads. The deterministic integration
test transfers immutable pump blocks to the existing private WASAPI PCM queue
and drives an injected backend. Production scheduling between a decode executor
and a dedicated STA WASAPI worker is a separate lifecycle batch.

## WASAPI end of stream

Non-EOS underrun may request all available endpoint frames, copy the media
prefix, fill the tail with silence, and release the complete packet. At EOS,
the state machine requests only the remaining media frames and releases that
same count. Once the queue is empty it waits without acquiring while endpoint
padding remains. Padding zero stops the client and enters `completed`.

This follows the `IAudioRenderClient` lease rule: a successful nonzero
`GetBuffer` is paired on the same thread with the same nonzero `ReleaseBuffer`
count; `ReleaseBuffer(0)` is reserved for abandoning an acquired packet before
commit.

## Evidence

The fixed `patternedPcmWav` fixture is mono PCM S16 at 24 kHz with 768 samples
across three sentinel patterns. SHA-256:
`0e153aa9a380f0702c7b701a0c3bf8ad095869c4c78b774cbaa3adcd0eb9d241`.

The direct reader proves exact same-format samples and a 1,536-frame 48 kHz
stereo conversion with equal channels. The injected pipeline uses a 150-frame
pump, 50-frame fill budget, 250-frame WASAPI queue, and 100-frame endpoint
packet. It compares every captured media byte with a separate direct decode,
exercises backpressure, cancellation, failed and successful replacement, and
requires a final exact 36-frame lease before `completed`.

Green CI proves compile, deterministic decode/conversion, bounded admission,
and injected buffer-lease correctness. It does not prove audible playback,
COM-worker scheduling, device format changes, A/V synchronization, codec
priming, physical latency or drift, or Windows 10 build 19045 behavior.

## Primary references

- [FFmpeg libswresample](https://ffmpeg.org/doxygen/trunk/group__lswr.html)
- [FFmpeg send and receive API](https://ffmpeg.org/doxygen/trunk/group__lavc__encdec.html)
- [WAVEFORMATEXTENSIBLE](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ksmedia/ns-ksmedia-waveformatextensible)
- [IAudioRenderClient::GetBuffer](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudiorenderclient-getbuffer)
- [IAudioRenderClient::ReleaseBuffer](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudiorenderclient-releasebuffer)
