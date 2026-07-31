# Windows development

Palmier Pro for Windows is a contract-compatible reimplementation of the
current macOS product. It is not a conditional Swift build.

## Current stage

M0 establishes product decisions, compatibility contracts, fixtures, and drift
checks. It does not yet provide a Windows application, media pipeline, or
installer.

- Requirements: `docs/WINDOWS_10_PORT_REQUIREMENTS.zh-CN.md`
- Decisions: `docs/windows/adr/`
- Versioned contracts: `contracts/`
- Compatibility fixtures: `fixtures/contracts/`

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
