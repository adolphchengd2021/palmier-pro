# Windows development

Palmier Pro for Windows is a contract-compatible reimplementation of the
current macOS product. It is not a conditional Swift build.

## Current stage

M0 establishes product decisions, compatibility contracts, fixtures, drift
checks, the compiled MSVC contract probe, and a safe-edit C++ project document.
It now includes an optional Qt project shell, isolated media/audio prototypes,
one bounded project-driven H.264 export slice, a five-tool loopback MCP editing
slice, and an atomic `project.json` edit-save-restart slice with stable-ID Split,
Move, Remove, shared Undo/Redo, explicit Save, and dirty-close protection in Qt. The Qt workflow also
builds an unsigned x64 prototype installer with a runtime hash manifest and
distribution evidence, then tests isolated staging, install, launch, uninstall,
   and external user-data preservation. It now also owns the active package
   identity, holds a process-visible write lease, and provides crash-safe Save
   As without discarding unknown package files. It does not yet provide general timeline
   editing, autosave, audio export, general timeline export, a signed
release installer, or clean Windows 10 19045 acceptance.

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
As, and reopen. The Windows writer now enforces the same declared canaries for
its bounded safe-edit slice; full known-field coverage remains a separate gate.

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
and covers Unicode paths and negative boundaries. The projection remains
incomplete, but supported `ProjectSession` edits can now save the full DOM with
an atomic sibling replacement and restart proof; see ADR 0008, ADR 0030, and
`contracts/project/v1/reader-projection.json`.

This does not replace the Python schema and Swift-source audit. The GitHub
Windows Server build also does not prove Windows 10 19045 runtime compatibility;
that remains a clean-VM gate.

The default build also provides `palmier_windows_mcp`. It loads one project
into a bounded serial `ProjectRuntime` before binding `127.0.0.1:19789`, then
exposes only `get_timeline`, `move_clips`, `remove_clips`, `split_clips`, and `undo` through the runtime's
single owned `ProjectSession`. MCP protocol sessions pin the active project
generation so a future project replacement cannot silently retarget them. A
real-process CTest verifies discovery, stable-ID move, remove, and split, exact move
no-op, cross-session readback, invalid-request isolation, shared undo, hostile-Origin
refusal, graceful exit, and port release. Clips carrying persisted fields not
represented by the current C++ projection are refused. This does not prove Qt
integration, project persistence, Windows 10 runtime behavior, or the remaining
MCP surface. See ADR 0026, ADR 0027, ADR 0036, ADR 0037, and
`contracts/mcp/v1/windows-technical-mvp.json`.

The HTTP implementation also exposes a stoppable `HttpServerService`, now owned
by the Qt process and connected to the same `ProjectRuntime` as project loading.
It starts off the caller thread, publishes the actual bound
loopback port or a terminal failure, uses a Windows stop event to wake the
worker-owned listener without polling, bounds an already admitted connection by
cancellation checks and socket timeouts reduced to the remaining total deadline,
and joins before destruction. A lifecycle CTest stops a partial request at its
second receive boundary, releases the port, then stops an idle rebound server.
An atomic non-Qt mailbox assigns every runtime publication a monotonic token;
one latest-only background projection refreshes the Qt model, invalidates stale
playback on edits, and rejects a result unless its token is still current.
Shutdown drains MCP, project loading, projection, preview, and the runtime
without joining their workers on the GUI thread. A background writer now saves
an exact runtime snapshot and acknowledges only that state after the atomic disk
commit. A dedicated Qt persistence controller runs Save on a bounded background
pool, blocks dirty project replacement, waits for an admitted save during close,
   and presents Save, Save As, Discard, and Cancel for dirty windows. Save As
   builds a complete same-volume sibling package, verifies every copied file,
   atomically installs a new destination, and switches identity only after the
   exact runtime snapshot is acknowledged. Autosave and broader concurrent media
   mutation coordination remain open; see ADR 0028 through ADR 0031 and ADR 0035.

The shared session also keeps a process-local Redo branch for Split, Move, and
Remove. Undo captures the exact post-action source DOM; Redo restores that state
without replaying command arguments, including stable generated IDs. A changed
edit clears Redo only at commit, while failures, cancellation, stale requests,
and exact Move no-ops preserve it. Qt consumes the published undo/redo depths on
the existing serial edit executor. MCP intentionally remains on the macOS-compatible
five-tool inventory, which exposes Undo but not Redo; see ADR 0038.

The same Qt workflow stages the Release executable through CMake install, runs
the pinned `windeployqt` QML deployment, requires the complete app-local FFmpeg
DLL set, and includes exact Qt, FFmpeg, Palmier Pro, and Visual C++ runtime
notices or records. A compiled probe refuses GPL/nonfree FFmpeg configuration
and writes runtime license/configuration evidence. The pinned Inno Setup 7.0.2
x64 compiler is release-attestation and Authenticode verified before it builds
the unsigned installer. CI then launches the staged and installed application
without development Qt/vcpkg paths, uninstalls it, verifies external user data
survives, and uploads the installer plus evidence for 14 days. This is Windows
Server 2022 automation, not clean Windows 10 certification or public-release
approval; see ADR 0033 and
`docs/windows/PROTOTYPE_REDISTRIBUTION_CONCLUSION.md`.

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
audio, VFR, trim seeking, H.265, and release approval remain open at the
low-level boundary. The Qt shell now owns one background selected-clip export
job, captures an immutable live runtime snapshot, resolves the media reference
through the shared package-safe resolver, supports cancellation and shutdown
drain, and reports a committed result from an older revision as
`completedOutdated`. The staging file is durably flushed before independent
decode and atomic installation. This remains a selected static video-clip slice,
not general timeline export; see ADR 0009, ADR 0024, ADR 0025, and ADR 0034.

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
