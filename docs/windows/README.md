# Windows development

Palmier Pro for Windows is a contract-compatible reimplementation of the
current macOS product. It is not a conditional Swift build.

## Current stage

M0 establishes product decisions, compatibility contracts, fixtures, drift
checks, the compiled MSVC contract probe, and a read-only C++ project document.
It now includes an optional read-only Qt shell, isolated media/audio prototypes,
one bounded project-driven H.264 export slice, and a three-tool loopback MCP
editing slice. It does not yet provide a persistent editor, project writer,
audio export, integrated export UI, or installer.

- Requirements: `docs/WINDOWS_10_PORT_REQUIREMENTS.zh-CN.md`
- Decisions: `docs/windows/adr/`
- Versioned contracts: `contracts/`
- Compatibility fixtures: `fixtures/contracts/`
- Toolchain and prototype dependency lock: `windows/toolchain.json`

Run the repository-only contract audit:

```powershell
python -B tools/validate_contracts.py --check
```

The audit verifies source snapshots, a supported local JSON Schema subset,
fixture structure, and type-sensitive canaries. It is not a complete Draft
2020-12 validator. A macOS integration test additionally requires declared
unknown-field canaries to survive a production load, edit, `NSDocument` Save
As, and reopen. Full known-field coverage and the future Windows writer remain
separate gates.

The media contract additionally compares every persisted Swift field signature,
optionality, required decode field, `ClipType`, and `MediaSource` payload with
`media-model.json` and `media.schema.json`. Its positive and negative fixtures
cover both source cases, nullable values, complete generation metadata, missing
required fields, ambiguous sources, path separators, and future enum values.

Do not add an empty CMake project. Add CMake, MSVC, and CTest together with the
first compilable Windows contract probe and a Windows CI build.

The first probe is now available on Windows with Visual Studio 2022:

```powershell
cmake --preset windows-msvc-x64
cmake --build --preset windows-msvc-x64-release --parallel
ctest --preset windows-msvc-x64-release
```

The C++ path uses one strict full-DOM parser, rejects duplicate keys and invalid
UTF-8, checks the v1 boundary, and derives a read-only project projection for
current and legacy fixtures. CTest compares the projection with an independent
Python oracle, verifies canonical source retention including unknown fields,
and covers Unicode paths and negative boundaries. The projection is explicitly
incomplete and no writer exists; see ADR 0008 and
`contracts/project/v1/reader-projection.json`.

This does not replace the Python schema and Swift-source audit. The GitHub
Windows Server build also does not prove Windows 10 19045 runtime compatibility;
that remains a clean-VM gate.

The default build also provides `palmier_windows_mcp`. It loads one project
into a bounded serial `ProjectRuntime` before binding `127.0.0.1:19789`, then
exposes only `get_timeline`, `split_clips`, and `undo` through the runtime's
single owned `ProjectSession`. MCP protocol sessions pin the active project
generation so a future project replacement cannot silently retarget them. A
real-process CTest verifies discovery, stable-ID split,
cross-session readback, invalid-request isolation, shared undo, hostile-Origin
refusal, graceful exit, and port release. Clips carrying persisted fields not
represented by the current C++ projection are refused. This does not prove Qt
integration, project persistence, Windows 10 runtime behavior, or the remaining
MCP surface. See ADR 0026, ADR 0027, and
`contracts/mcp/v1/windows-technical-mvp.json`.

The render boundary is also compiled in Windows CI.
`core/render` defines the immutable v1 RenderPlan and CPU oracle;
`windows/render-d3d11` consumes it through a headless feature-level 11_0 WARP
backend. Its CTest checks CPU/WARP float-frame parity and identical full-quality
preview/export render test entry points. `core/project-render` now compiles one persisted
static video clip through stable IDs into the same plan, including transform,
opacity, normal blend, and one exposure effect. Unsupported visual properties
are explicit refusals. This does not yet establish parity with the Swift BGRA8
compositor or prove multi-layer composition, physical-GPU behavior, or Windows
10 runtime behavior. The first production-owned video-only encoder consumes
this exact compiler and render entry point, records and locks a sibling staging
MP4's file identity, independently decodes it, and commits that verified object
with one handle-based rename. It accepts
only one exact-CFR source at source frame zero and the locked `h264_mf` encoder;
audio, VFR, trim seeking, H.265, UI job ownership, and release approval remain
open. See ADR 0009, ADR 0024, and ADR 0025.

The optional FFmpeg prototype activates the first vcpkg manifest dependency:

```powershell
$env:VCPKG_ROOT = "C:\path\to\vcpkg"
cmake --preset windows-msvc-x64-ffmpeg
cmake --build --preset windows-msvc-x64-ffmpeg-release --parallel
ctest --preset windows-msvc-x64-ffmpeg-release
```

It probes a tiny H.264/AAC MP4 and software-decodes an exact QTRLE alpha frame
through FFmpeg 8.1.2 LGPL DLLs. This is a bounded media-reader proof, not a
player: WASAPI, D3D11VA, synchronized audio, 1080p performance, Windows 10, and
packaged DLL verification remain open. See ADR 0010.

The FFmpeg preset also builds `windows/media-render`. Its adapter validates the
decoded frame before allocation, honors padded row stride, bakes pure cardinal
display rotation into a top-left `SourceFrame`, and feeds the existing CPU and
D3D11 WARP preview/export path. Only explicit opaque and straight alpha are
accepted; unknown and premultiplied alpha are hard refusals. The positive
adapter fixture is synthetic, while the real QTRLE fixture proves that unknown
alpha cannot enter rendering. This does not provide arbitrary-frame decode,
cache ownership, a playback loop, or Swift pixel parity. See ADR 0012.

The default Windows build also compiles a bounded WASAPI environment probe and
pure integer audio-clock conversion tests. The probe opens the default
multimedia render endpoint in event-driven shared mode, validates its engine
period, buffer, render service, and clock frequency, and then exits without
starting the stream. It is safe on CI machines with no audio endpoint or a
stopped audio service. It does not prove audible output, callback behavior,
device recovery, long-run A/V synchronization, or Windows 10 runtime behavior.
See ADR 0011.

The separate WASAPI output prototype adds a bounded PCM queue, transactional
same-thread buffer leases, prime-before-start ordering, event-driven refill,
pause/reset generation rules, invalidation receipts, and generation-checked
cached clock samples. Its
serial native smoke writes silence only, waits at most two seconds for one
render event, then stops and resets. An unavailable CI endpoint remains a
classified diagnostic, not output evidence. It does not yet provide media PCM,
format conversion, automatic device recovery, audible playback, or A/V frame
selection on a real swap chain. The media-session layer preserves the first
admitted source PTS and time base alongside its device clock anchor and now uses
that mapping for deterministic headless frame hold/drop/select behavior.
See ADR 0013 and `contracts/audio/v1/wasapi-output.json`.

The first headless A/V coordinator prepares a separate video candidate before
an exact-generation audio commit, reads one cached audio position per tick, and
uses the shared presentation selector for bounded fill/drop/select. It creates
no timer, presenter, thread, or swap chain and must run on a background owner
executor. Its deterministic tests do not prove a physical endpoint, perceptible
A/V synchronization, device recovery, or Windows 10 runtime behavior. See ADR
0020 and `contracts/media/v1/headless-av-playback-session.json`.

The first native preview surface owns one HWND flip-discard swap chain and a
serialized D3D11 immediate context. It accepts complete frames from the shared
renderer, performs one bounded upload/draw/non-blocking Present, letterboxes to
opaque black, and explicitly classifies occlusion, busy, unavailable, device
loss, cancellation, failure, and close. Its hidden-HWND WARP smoke proves only
runner environment classification; Qt integration, visible pixels, cadence,
physical-GPU performance, device recovery, A/V synchronization, and Windows 10
runtime behavior remain open. See ADR 0021 and
`contracts/render/v1/d3d11-preview-surface.json`.

The first background preview coordinator now owns the headless A/V generation,
latest selected source frame, shared WARP renderer, pending rendered frame, and
one HWND surface. Each scheduler tick consumes the headless audio-clock tick
exactly once. Identical content is coalesced; busy or occluded Present results
retain one rendered frame for one later tick, and resize only marks cached
content dirty. This boundary has no timer or retry loop and must not run on the
Qt UI thread. Its injected tests do not prove Qt integration, visible pixels,
physical A/V synchronization, fractional frame rates, zero-copy performance,
or Windows 10 runtime behavior. See ADR 0022 and
`contracts/media/v1/preview-presentation-session.json`.

The first Qt target is an optional read-only project shell:

```powershell
$env:QT_ROOT_DIR = "C:\Qt\6.10.3\msvc2022_64"
$env:VCPKG_ROOT = "C:\path\to\vcpkg"
cmake --preset windows-msvc-x64-qt-shell
cmake --build --preset windows-msvc-x64-qt-shell-release --parallel
ctest --preset windows-msvc-x64-qt-shell-release
```

It selects a directory-form `.palmier` package, reads `project.json` through a
size-, value-, and string-budgeted cancellable Qt-free loader, and publishes
an immutable active-timeline track/clip projection to QML. Load generations
reject stale results; failure and cancellation retain the last successful
model. The target does not write projects or provide editing, media resolution,
playback, undo, or deployment. Unsafe clips are reported and omitted from the
visual projection; over-dense active timelines are explicitly refused instead
of being truncated. The same preset now embeds a Qt-owned native child window
through `WindowContainer` and constructs, resizes, closes, and destroys the
real preview session on one persistent background thread. Resize bursts retain
only the latest positive client size, and application close waits for both the
project reader and preview session. Windows-platform tests prove the native
child relationship and WARP lifecycle. A serial smoke drives a fixed H.264/AAC
`.palmier` package through project commit and requires render, present, and EOF
receipts when WASAPI is available; a typed matching endpoint-unavailable result
is reported only as Partial. This does not prove visible pixels, interactive
cadence, physical-GPU performance, DPI behavior, physical-device A/V
synchronization, or Windows 10 runtime compatibility. See ADR 0014 and ADR 0023.
