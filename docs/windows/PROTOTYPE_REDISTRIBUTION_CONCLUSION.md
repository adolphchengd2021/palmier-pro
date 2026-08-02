# Windows prototype redistribution conclusion

Status: **GO WITH CONDITIONS for internal Technical MVP testing**

Public release: **NO-GO**

Reviewed: 2026-08-02

This is an engineering distribution assessment, not legal advice. It applies
only to the unsigned x64 prototype produced from the locked repository build.

## Included runtime

| Component | Prototype evidence | Current conclusion |
|---|---|---|
| Palmier Pro 0.6.16 | GPLv3 license and repository source are identified in the stage | Internal testing allowed; release source-delivery process still required |
| Qt 6.10.3 | Dynamic DLL/QML deployment through `windeployqt`; verified qtbase license text set included | Conditional; complete deployed-module attribution, final LGPL/commercial choice, and Qt 6.12 Windows 10 review required |
| FFmpeg 8.1.2 | Dynamic vcpkg DLLs; runtime probe rejects GPL/nonfree configuration and records license/configuration; vcpkg copyright included | Conditional; archive matching source, patches and build flags, then complete codec-patent review |
| Visual C++ Runtime | Official x64 redistributable selected from Visual Studio 2022 17.14 | Conditional on Microsoft redistribution terms; installed as a prerequisite |
| Inno Setup 7.0.2 | Compiler asset is release-attestation and Authenticode verified; compiler is not shipped | Conditional; commercial-use procurement decision is required before release |
| H.264 Media Foundation encoder | Uses the operating-system encoder path; no encoder binary is redistributed by this installer | Technical prototype only; availability and patent/commercial review remain release gates |
| ONNX Runtime, models, bundled fonts | Not linked or shipped by the current Qt executable | Outside this artifact; must be reassessed when first included |

Each CI artifact contains `RUNTIME_MANIFEST.json` with relative paths, byte
sizes, SHA-256 hashes, and source classifications, plus
`FFMPEG_RUNTIME_EVIDENCE.txt`, `THIRD_PARTY_NOTICES.txt`, and dependency license
records. This manifest is an auditable prototype inventory, not a release SBOM.

## Conditions to continue engineering

- Keep the artifact explicitly unsigned and internal.
- Do not publish it as Windows 10 compatible until a clean Windows 10 19045 x64
  machine without Visual Studio, Qt, vcpkg, or repository paths passes the G0
  manual test.
- Do not add GPL/nonfree FFmpeg build flags or statically link the locked Qt or
  FFmpeg runtime without a new distribution review.
- Do not add models, fonts, H.265, hardware codecs, or cloud SDK binaries without
  extending the manifest, notices, source evidence, and relevant legal review.
- Purchase or formally clear Inno Setup commercial use before a commercial
  build pipeline or public release.

## Clean Windows 10 19045 gate

Record the installer SHA-256, OS build, CPU, GPU/driver, audio device and result
for each step:

1. Install the unsigned prototype and record the expected SmartScreen/signing
   warning; verify no developer tool or repository path is required.
2. Launch, open a copied `.palmier` fixture, preview on the physical GPU and
   audio device, then Split, Undo, Save, close and reopen.
3. Confirm MCP binds loopback only, performs split/readback/undo against the same
   live session, and releases its port on exit.
4. Exercise dirty Save, Discard and Cancel; minimize, sleep/wake, and close.
5. Uninstall and verify the copied project and unrelated user data remain
   unchanged.

## Release blockers

- Code signing certificate and signed installer evidence.
- Clean install, upgrade, interrupted-upgrade recovery, downgrade refusal,
  rollback and uninstall matrices on the supported Windows versions.
- Final SPDX or CycloneDX SBOM, complete third-party notices, matching-source
  archive and source-offer process.
- Qt licensing choice, Inno Setup procurement, FFmpeg/codec patent review, and
  font/model redistribution review.
- Windows 10 19045 physical GPU/audio, DPI, accessibility, storage and long-run
  reliability evidence.

Verdict changes to public-release GO only when every blocker has authoritative
evidence and release approval. Installer creation or a green hosted CI run does
not change that verdict.
