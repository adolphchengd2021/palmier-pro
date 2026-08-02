# ADR 0025: Export one static project clip through the shared render plan

- Status: Accepted for the M1 project export slice
- Date: 2026-08-02
- Owner: Windows media export
- Applies to: bounded video-only H.264 MP4 export from one persisted project clip

## Decision

`windows/export-ffmpeg` owns the first production export slice. It compiles one
persisted video clip through `core/project-render`, decodes its source in
presentation order, calls the shared `makeRenderPlan` and `renderExportFrame`
operations for every output frame, and sends the resulting pixels to FFmpeg's
Windows Media Foundation H.264 encoder. It does not parse project visual data,
reimplement timeline math, or accept another encoder silently.

This slice accepts only an absolute local input, an absolute `.mp4` destination,
one exact-CFR source beginning at source frame zero, an even-sized canvas, and a
bounded positive clip duration. Every decoded timestamp must map exactly to the
expected integer source frame. VFR, source-frame-rate conversion, trim seeking,
audio muxing, multiple visible layers, and every visual state already refused by
the project compiler remain explicit failures.

Encoding writes to a uniquely created sibling staging file on the destination
volume. Its Windows volume and file identity are recorded at creation, and a
low-access tracking handle retains that exact file object through the operation.
The object is locked against writes before verification and checked again on the
commit handle. The independent FFmpeg reader must find H.264 at the requested
dimensions, frame rate, timestamp sequence, and frame count. Only that verified
file object is installed with one handle-based Windows rename. Failure or
cancellation preserves an existing destination and removes the exact staging
identity through the retained object handle.

The FFmpeg reader accepts this output only as explicit BT.709 limited-range
YUV420P or NV12 with H.264's left chroma location. It configures the matrix,
range, and chroma position once per decoded format, then describes the resulting
RGBA bytes as full-range RGB while preserving BT.709 primaries and transfer.
The current sRGB-only render-source adapter still refuses those frames; this
slice does not claim preview color parity or add an implicit transfer-function
conversion.

If that identity cannot be removed, cleanup becomes the reported terminal
failure; the exporter never deletes a different file found at the staging path.

Cancellation is authoritative through the final `beforeInstall` checkpoint.
Once the single OS move or replace call begins, that commit is non-cancellable
and may complete. The exporter serializes encoder ownership process-wide; a
cancelled waiter never acquires the encoder or touches a destination.

The synchronous exporter is a background service seam. A future UI or Agent
controller must own its task, cancellation source, terminal receipt, and actor
hop; it must never call the exporter on the UI thread.

## Tests

The deterministic contract test covers invalid paths and limits, destination
overwrite policy, CFR mismatch, cancellation after staging creation, staging
cleanup, and preservation of an existing destination without requiring an
encoder. The native test writes the fixed H.264 source fixture, reads a real
project document, exports every compiled frame through the CPU renderer, and
independently probes and decodes the installed MP4. It also covers early source
EOF, a locked-destination install failure, and cancellation after verification
but before install. When Windows Server lacks the underlying H.264 MFT, CTest
reports that native test as skipped rather than converting an environment
capability absence into implementation success or failure.

## Evidence boundary

A green MSVC `/W4 /WX` build and passing native CTest prove only the bounded
video-only static clip path with the available `h264_mf` runtime. A skipped
native test proves only explicit capability classification. Neither result
proves audio muxing, H.265, VFR, seeking, GPU export, color parity with the
shipping Swift compositor, long projects, physical Windows 10 encoder
availability, UI progress or cancellation, package installation, or
distribution and codec-patent approval.
