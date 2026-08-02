# ADR 0032: Route initial Qt edits through ProjectRuntime

- Status: Accepted for the Windows technical MVP
- Date: 2026-08-02
- Owner: Windows editor UI and project runtime
- Applies to: P0

## Decision

The first direct Qt editing surface exposes one supported safe operation:
splitting a selected clip at an exact timeline frame, plus Undo. QML sends the
stable clip ID and the frame as decimal text to `ProjectEditingController`.
The controller first requires non-empty ASCII decimal digits and then performs
signed 64-bit range conversion. It rejects whitespace, signs, non-ASCII digits,
and overflowing input, and never relies on JavaScript number precision.

The controller invokes `ProjectRuntime::splitClips` and `ProjectRuntime::undo`
on a bounded background pool with the active project generation. The runtime
remains the sole session owner and both UI and MCP therefore use identical
validation, mutation, publication, dirty-state, and undo behavior. QML does not
mutate the model or construct replacement clips.

`ProjectSessionSnapshot` publishes authoritative `undoDepth`. Qt derives its
Undo enabled state from that snapshot, including edits made through MCP. A
project replacement or shutdown invalidates late commands through the expected
generation and cancellation token. Closing while an edit is active waits for
its terminal result, then re-evaluates dirty state before presenting the normal
Save, Discard, or Cancel decision. Shutdown admission refreshes dirty state
directly from the runtime mailbox, so delayed Qt projection delivery cannot
approve a stale clean state.

## Tests

Qt tests split a stable clip through the controller, independently read the
runtime snapshot, verify two clips and one undo entry, execute Undo, and verify
the original clip set and empty undo history. Invalid frame input must return a
stable error without mutation. Existing runtime projection, persistence, close,
MCP, and project-session tests continue to exercise the shared state owner.

## Evidence boundary

This slice does not add trim, move, ripple, linked-clip UI, selection
persistence, keyboard shortcuts, multi-action undo labels, autosave, or Save As.
Mouse selection, focus, disabled-state behavior, close during edit, and visual
feedback still require manual Windows UI verification.
