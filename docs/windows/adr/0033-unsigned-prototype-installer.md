# ADR 0033: Build an auditable unsigned prototype installer

- Status: Accepted for the Windows technical MVP
- Date: 2026-08-02
- Owner: Windows packaging
- Applies to: Technical MVP gate G0

## Decision

The Qt Windows workflow stages the Release application through CMake install,
then runs the pinned Qt 6.10.3 `windeployqt` tool against the staged executable
and the repository QML source directory. The stage copies the exact app-local
FFmpeg DLL closure emitted by the locked vcpkg build and the official x64 Visual
C++ redistributable from the selected Visual Studio 17.14 installation.

One compiled FFmpeg distribution probe records the loaded libavcodec,
libavformat, libavutil, libswresample, and libswscale versions, license strings,
and configure strings. Packaging fails if any component reports a non-LGPL
license or `--enable-gpl`/`--enable-nonfree`. The stage also requires the Qt
license directory and vcpkg FFmpeg copyright record, includes Palmier Pro's
GPLv3 license and the prototype third-party notice, and creates a sorted SHA-256
runtime manifest before installer compilation.

CI obtains the pinned Inno Setup 7.0.2 x64 compiler from its immutable GitHub
release, verifies the GitHub release attestation and valid Authenticode
signature, and installs the compiler only into the ephemeral runner. The Inno
script creates one administrator-level x64 installer with a Windows 10 build
19045 minimum. It installs the official Visual C++ redistributable, application,
Qt/QML, and FFmpeg runtime files. The uninstaller owns only the files declared
by the installer and has no rule targeting `.palmier` packages or user-data
directories.

The workflow launches both the staged and installed application with the
development Qt and vcpkg paths removed, silently uninstalls it, independently
checks that an external user-project sentinel is unchanged, and uploads the
installer, hashes, manifest, notices, and current redistribution conclusion.

## Invariants

- Packaging uses Release x64 artifacts from the same build that passed Qt tests.
- Missing Qt/QML, FFmpeg, license, Visual C++ runtime, or distribution-probe
  evidence fails packaging; it is never silently omitted.
- The staged runtime and installer are unsigned and named as prototypes.
- Installer generation does not imply Windows 10 compatibility, licensing,
  codec-patent approval, signing, upgrade, recovery, or public-release approval.
- User projects remain outside the install root and are never uninstall targets.

## Evidence boundary

The hosted workflow proves movable staging, installer compilation, unattended
install, isolated startup, unattended uninstall, and external user-data
preservation on Windows Server 2022. Technical MVP G0 remains Partial until the
same artifact passes the documented clean Windows 10 19045 manual matrix,
including project open/edit/save/reopen, physical preview/audio, MCP loopback,
and uninstall. Public release remains blocked on procurement, final notices and
source offer, codec-patent review, signing, upgrade/recovery/rollback, SBOM, and
release approval.
