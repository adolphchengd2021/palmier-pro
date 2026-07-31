# Palmier Pro compatibility contracts

These files freeze externally observable behavior at the macOS v0.6.16
baseline, commit `457e853a789e16eb104a0bfb43d2485d9e1ac0c8`.

Directory `v1` is the version of this contract documentation. It does not claim
that existing `project.json` files contain `schemaVersion: 1`.

## Sources of truth

- Project data: Swift models plus accepted compatibility ADRs.
- Effects: `EffectRegistry` and `BlendMode`.
- Agent tools: `ToolDefinitions`.

Run:

```powershell
python -B tools/validate_contracts.py --check
```

The check is intentionally read-only. Snapshot changes require an explicit
source and contract change in the same review.

## Status

- Source identifier drift checks: required.
- Fixture and canary integrity: required.
- Project schema: provisional structural validation only; known-field
  inventory, defaults, enums, and client decode behavior are incomplete.
- Media schema: complete known-field structural inventory for the current
  Swift models, checked against field signatures, optionality, required decode
  fields, enum payloads, and focused source-body digests. Real client
  writer/decode behavior still requires Swift golden tests.
- Unknown-field client load-edit-save round trip: blocked.
- Full effect parameter snapshot: not yet captured.
- Complete MCP input-schema snapshot: not yet captured.

The provisional project schema is not an implementation-ready Windows format
specification. The media schema is stricter: its properties, `ClipType`, and
`MediaSource` cases are checked against `media-model.json` and Swift
declarations, but it still does not replace real Swift writer/decode and
load-edit-save tests.
