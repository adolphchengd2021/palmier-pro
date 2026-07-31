# ADR 0007: Lock the M0 Windows toolchain and prototype dependencies

- Status: Accepted for M0
- Date: 2026-08-01
- Owner: Windows engineering
- Applies to: M0 and the first MSVC contract probe

## Decision

The Windows build baseline is Visual Studio 2022 17.14, MSVC v143 14.44,
Windows SDK 10.0.26100.0, CMake 3.31.6, and C++20 on x64. CI uses the explicit
`windows-2022` image and rejects a different Visual Studio minor line, MSVC
minor line, CMake version, or missing SDK target until the change is reviewed.

The hosted runner image is serviced weekly. The exact Visual Studio patch and
SDK servicing package are evidence, not reproducibility guarantees. The
reference developer releases are Visual Studio 17.14.37 and Windows SDK
10.0.26100.8038. A future bootstrap image must pin those installers and hashes.

Use vcpkg manifest mode when the first third-party C++ dependency is added.
Start from release `2026.06.24` and builtin baseline
`cd61e1e26a038e82d6550a3ebbe0fbbfe7da78e3`. Disable vcpkg metrics in local
and CI bootstrap. Do not add an empty `vcpkg.json` before a dependency exists.

The M0 prototype versions are:

- Qt 6.11.1 for MSVC 2022 x86_64.
- FFmpeg 8.1.2 as LGPL dynamic libraries.
- ONNX Runtime 1.28.0 with a required CPU path and optional DirectML path.
- Inno Setup 7.0.2 as the first x64 installer prototype.

These runtime versions are prototype locks, not release approval. The
machine-readable source of truth is `windows/toolchain.json`.

## Distribution gates

- Re-evaluate Qt after the first qualified 6.12.x release because Qt 6.12 is
  the final Windows 10 line. Lock the used modules and commercial or open-source
  obligations before release.
- Build FFmpeg without `--enable-gpl` and `--enable-nonfree`, link DLLs
  dynamically, archive matching source and build flags, and complete codec
  patent review before distribution.
- Keep ONNX Runtime CPU inference available. DirectML is optional because it
  requires DirectX 12 and is in sustained engineering; validate model parity,
  driver failure fallback, cancellation, and serialized session use.
- Validate signed clean install, upgrade, downgrade refusal, uninstall, and
  rollback behavior on Windows 10 19045 before selecting Inno Setup for release.
  Confirm the commercial procurement policy before production use.
- Generate an SBOM and third-party notices from the exact shipped artifacts.

## CI meaning

The MSVC contract probe proves that an x64 Windows program can parse the
versioned contracts and compatibility fixtures with the selected compiler
family. GitHub's Windows Server 2022 runner does not prove Windows 10 19045
runtime compatibility. That requires a clean Windows 10 22H2 VM or self-hosted
runner in a later gate.

## Primary references

- [Visual Studio 2022 servicing](https://learn.microsoft.com/en-us/visualstudio/releases/2022/servicing-vs2022)
- [Visual Studio Build Tools components](https://learn.microsoft.com/en-us/visualstudio/install/workload-component-id-vs-build-tools?view=vs-2022)
- [Windows SDK release notes](https://learn.microsoft.com/en-us/windows/apps/windows-sdk/release-notes)
- [CMake 3.31.6 release](https://github.com/Kitware/CMake/releases/tag/v3.31.6)
- [CMake 4.3.3 release](https://github.com/Kitware/CMake/releases/tag/v4.3.3)
- [Ninja 1.13.2 release](https://github.com/ninja-build/ninja/releases/tag/v1.13.2)
- [vcpkg 2026.06.24 release](https://github.com/microsoft/vcpkg/releases/tag/2026.06.24)
- [vcpkg manifest mode](https://learn.microsoft.com/en-us/vcpkg/concepts/manifest-mode)
- [vcpkg privacy](https://learn.microsoft.com/en-us/vcpkg/about/privacy)
- [Qt for Windows](https://doc.qt.io/qt-6/windows.html)
- [Qt Windows 10 support plan](https://www.qt.io/blog/windows-10-eol-plans-in-qt)
- [FFmpeg 8.1.2 sources](https://ffmpeg.org/releases/)
- [FFmpeg legal considerations](https://ffmpeg.org/legal.html)
- [ONNX Runtime 1.28.0](https://github.com/microsoft/onnxruntime/releases/tag/v1.28.0)
- [DirectML execution provider](https://onnxruntime.ai/docs/execution-providers/DirectML-ExecutionProvider.html)
- [Inno Setup 7 history](https://jrsoftware.org/files/is7-whatsnew.htm)

## Revisit when

- The first Qt, FFmpeg, ONNX Runtime, or installer target is added.
- Qt 6.12 reaches general availability.
- The hosted runner changes a gated toolchain family or CMake version.
- A Windows 10 19045 runtime lane is available.
