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
