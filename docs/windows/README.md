# Windows development

Palmier Pro for Windows is a contract-compatible reimplementation of the
current macOS product. It is not a conditional Swift build.

## Current stage

M0 establishes product decisions, compatibility contracts, fixtures, drift
checks, the compiled MSVC contract probe, and a read-only C++ project document.
It does not yet provide a Windows application, project writer, media pipeline,
or installer.

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

The next synthetic rendering boundary is also compiled in Windows CI.
`core/render` defines the immutable v1 RenderPlan and CPU oracle;
`windows/render-d3d11` consumes it through a headless feature-level 11_0 WARP
backend. Its CTest checks CPU/WARP float-frame parity and identical full-quality
preview/export entry points. This does not yet establish parity with the Swift
BGRA8 compositor. It has no project compiler, Swift pixel goldens, FFmpeg, WASAPI,
swapchain, Qt, physical-GPU, or Windows 10 runtime evidence yet. ADR 0009 lists
the required hard refusals before project integration.

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
pause/reset generation rules, invalidation receipts, and clock samples. Its
serial native smoke writes silence only, waits at most two seconds for one
render event, then stops and resets. An unavailable CI endpoint remains a
classified diagnostic, not output evidence. It does not yet provide media PCM,
format conversion, automatic device recovery, audible playback, or A/V sync.
See ADR 0013 and `contracts/audio/v1/wasapi-output.json`.
