# ADR 0050: Bind recovery choices to exact journal state

- Status: Accepted for the Windows technical MVP
- Date: 2026-08-03
- Owner: Windows project recovery
- Applies to: P0

## Decision

Every recovery choice is bound to a fingerprint of the complete journal file:
its SHA-256 and byte length. Before acting, startup orchestration must read the
current fingerprint again. A mismatch returns `recoveryCandidateChanged` and
must not install, replace, or delete the newer journal.

Keep Saved opens the current disk project and leaves the journal unchanged, so
the choice can be revisited on a later start. Recover Edits explicitly installs
the exact candidate through `ProjectRuntime::installRecovered`. After that
authoritative in-memory commit and the no-throw package activation, the existing
`ProjectRecoveryJournal::write` operation atomically replaces the journal from
the dirty runtime using the current process generation. Replacement failure is
a visible warning; the prior complete journal remains available and the
committed recovered runtime is not rolled back.

Discard Recovery deletes only the exact fingerprint shown to the user. It opens
the derived journal object by handle, hashes the complete bounded content, and
marks that handle for deletion only after the hash matches. It intentionally
does not parse the payload, allowing a corrupt journal to be discarded without
trusting malformed metadata. Cancellation before deletion retains the file.

A `staleBaseline` candidate is never automatic. The user may explicitly recover
it only with a visible warning that the disk project changed outside the
recovery snapshot and that a later Save will replace the disk version.

## Rejected approach

Journal replacement does not run as a callback inside the serial
`ProjectRuntime` executor. File I/O there would block editor and MCP operations,
invert the core-to-Windows package dependency, and still could not make a disk
rename and an in-memory pointer swap one crash-atomic transaction.

## Tests

The project recovery suite proves that a stale fingerprint cannot delete a
newer valid journal, a corrupt journal can be fingerprinted and discarded
without parsing, cancellation retains the corrupt journal, and repeated discard
accurately reports a missing object.

## Evidence boundary

These primitives do not expose a startup prompt. ProjectLoadCoordinator
discovery, package-lease ownership, single-action admission, visible warnings,
recovered Save readback, and QML interaction remain separate gates.
