# ADR 0026: Route the Windows MCP editing slice through ProjectSession

- Status: Accepted for the M1 technical MCP slice
- Date: 2026-08-02
- Owner: Windows project editing and Agent transport
- Applies to: `get_timeline`, `split_clips`, and `undo` over loopback HTTP

## Decision

`core/project-session` is the single mutable owner for the first Windows edit
slice. It owns the in-memory project projection, revision, dirty state, and a
shared inverse-delta undo journal. HTTP sessions contain protocol identity only.
They submit typed queries and commands to the same `ProjectSession`; neither the
transport nor a future Qt controller may cache or mutate timeline state.

`split_clips` supports the existing stable-ID and track-frame request modes.
Every clip ID, frame, duplicate point, and linked partner is resolved before a
planned project copy is committed. The left segment retains its persisted ID;
right segments receive new session-stable IDs. Linked right halves share a new
link group. One call advances the revision once and creates one undo entry.
Failure, cancellation, and rejected empty requests do not create revisions or
undo entries. Duplicate points are coalesced inside the one committed action.

The read-only C++ projection does not represent every persisted Swift clip
field. A clip with a source field outside the safe projection, a synthesized
ID, caption membership, or multicam membership is therefore refused before
mutation. This slice does not write `project.json` and makes no save/reopen or
unknown-field round-trip claim.

`windows/mcp-http` binds exactly IPv4 `127.0.0.1:19789`, accepts `/mcp` only,
validates local Origin when present, requires JSON content negotiation and MCP
protocol version `2025-06-18`, bounds headers, bodies, and live sessions, and
serializes requests through the project owner. Per-connection I/O has a bounded
deadline. Idle sessions expire, and capacity pressure evicts the least recently
used session so abandoned clients cannot permanently exhaust the service. It
advertises only the three implemented tools. Unimplemented tools return an MCP
tool failure rather than a success-shaped placeholder.

The versioned Technical-MVP discovery and receipt boundary lives at
`contracts/mcp/v1/windows-technical-mvp.json`. It is an explicit subset of the
full MCP surface while `contracts/mcp/v1/tools.json` remains incomplete.

## Tests

The command test covers exact split timing, stable left IDs, new right IDs,
duplicate and multi-cut behavior, linked A/V splitting, one-action undo,
invalid-batch atomicity, cancellation, and refusal of unrepresented source
fields.

The HTTP CTest starts the production server against an isolated fixture copy,
waits for readiness after project load and loopback bind, initializes two MCP
sessions, compares discovery with the checked-in contract, splits in one
session, reads back through the other, verifies invalid requests do not mutate
or add undo, performs shared undo, and checks graceful exit and port
release. It also rejects a hostile Origin and wrong content type.

## Evidence boundary

A green MSVC `/W4 /WX` build and passing CTests prove the in-memory command and
real loopback process boundary on the CI runner. They do not prove Windows 10
19045, Qt and MCP sharing one live session, project persistence, crash recovery,
interleaving with UI edits, or compatibility for the remaining MCP tools.
