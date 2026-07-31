# ADR 0004: Use one Windows project session and command path

- Status: Proposed
- Date: 2026-07-31
- Owner: Project core
- Applies to: M1 and later

## Proposal

Use one C++ `ProjectSession` as the mutable Windows project owner. UI and MCP
submit the same typed commands and receive structured receipts containing:

- changed or no-op state;
- stable IDs;
- warnings and skipped entities;
- actionable errors;
- durable job IDs for long-running work.

Validation completes before mutation and before an undo entry is opened.
Undo stores domain deltas or inverse commands, not UI snapshots. Background
results carry the project revision and are rejected when stale.

## Invariants

- One user intent creates one atomic and understandable undo action.
- Failed, refused, cancelled, and unchanged commands do not dirty the project.
- Qt, MCP, persistence, preview, and export do not implement separate timeline
  mutation rules.

## Open work

- Define command and receipt schemas.
- Define revision and long-job state transitions.
- Prove linked clips, nesting, cancellation, close, and project switching.

