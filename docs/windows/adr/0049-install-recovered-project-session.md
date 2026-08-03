# ADR 0049: Install recovered project state as dirty

- Status: Accepted for the Windows technical MVP
- Date: 2026-08-03
- Owner: Windows project runtime
- Applies to: P0

## Decision

Recovery uses the dedicated `ProjectRuntime::installRecovered` operation. A
normal install represents disk state and starts clean; reusing it for recovery
would silently mark unsaved edits as persisted.

`ProjectRecoverySessionState` carries the journal revision, state ID, and
persisted state ID into `ProjectSession`. Revision must be positive and leave
one representable next revision inside the signed MCP integer domain. State
and persisted state must differ, and their maximum must leave a representable
next identity and allocation cursor. The first runtime publication preserves
those exact values and remains dirty.

The recovery payload contains a complete project DOM but no command history.
The recovered session therefore starts with empty Undo and Redo journals. The
next edit increments the recovered revision and allocates
`max(stateId, persistedStateId) + 1`. After a successful Save,
`markPersisted(stateId)` clears dirty state. A later edit creates one normal
Undo entry, and Undo can return to the persisted recovered state without
inventing pre-crash actions.

Runtime installation validates the new process generation, dirty replacement
policy, cancellation, and recovery identity before swapping the active
session. Failure leaves the prior runtime unchanged. The caller supplies the
current process generation; the journal's prior process generation remains the
old recovery object's identity until startup orchestration safely rekeys or
retires it.

## Tests

ProjectSession tests prove exact dirty identity, empty initial history,
Save-to-clean, the next state allocation, and Undo back to the persisted
recovered state. Boundary tests reject zero revision, revision overflow, clean
identity, and state identity overflow. ProjectRuntime tests prove one exact
publication, save snapshot parity, normal post-recovery edit and Undo, clean
recovery refusal, cancellation, and unchanged active state on failure.

## Evidence boundary

This decision provides the runtime installation primitive only. Startup
inspection, cross-process journal rekey, explicit Keep Saved / Recover Edits /
Discard Recovery actions, stale-candidate presentation, independent package
readback, and visible UI verification remain separate gates.
