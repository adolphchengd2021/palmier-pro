# ADR 0047: Persist one bounded recovery snapshot outside the live package

- Status: Accepted for the Windows technical MVP
- Date: 2026-08-03
- Owner: Windows project persistence
- Applies to: P0

## Decision

`ProjectRecoveryJournal` is the synchronous off-main filesystem boundary for
one dirty project recovery snapshot. Its default root is
`FOLDERID_LocalAppData/Palmier Pro/Recovery/v1`. The file name is the SHA-256 of
the case-folded canonical package path, so directory listings do not expose a
project name or path.

The journal embeds the complete project source DOM plus exact runtime
generation, revision, state ID, persisted state ID, creation time, canonical
payload SHA-256, and the SHA-256 of the live `project.json` bytes used as its
baseline. Unknown project fields remain inside the same DOM. Clean runtime
states are refused because they do not require recovery.

Writing uses a unique sibling `.partial` file in the recovery directory. The
writer checks cancellation across bounded 1 MiB chunks, flushes the file,
reads the complete content back through the same handle, and installs it with
one handle-based atomic replacement. A failure or pre-commit cancellation
retains the previous journal and removes the staging file. The live `.palmier`
package is never changed by journal creation.

Inspection recomputes the current live `project.json` SHA-256. An exact match is
`recoverable`; a mismatch is `staleBaseline`. A stale candidate may be displayed
or retired later but must never be applied automatically. Malformed metadata,
payload hashes, package keys, versions, or clean-state journals fail explicitly.
If the live baseline also equals the recovery payload, inspection reports
`redundant` rather than offering a no-op recovery.

Retirement opens and validates the exact journal object before deletion. The
runtime generation must match and the journal revision must not be newer than
the committed revision. This preserves a newer dirty snapshot when an older
Save completes.

## Tests

The Windows project-package suite proves complete unknown-field recovery,
unchanged live package bytes, path-private naming, recoverable and stale
inspection, generation and revision retirement rules, corrupt-journal refusal,
clean-state refusal, and cancellation at every pre-commit checkpoint. Every
cancelled replacement must preserve the prior journal and leave no `.partial`
file. A second writer waiting on the serial mutation gate must be cancellable
without disturbing the admitted writer. Cancellation observed after atomic
commit must retain and report the installed journal.

## Evidence boundary

This decision provides the durable journal primitive only. Qt dirty-state
scheduling, save-time retirement, startup discovery, recovery choice UI,
independent reopen of the recovered snapshot, media payload transactions, and
Windows 10 19045 manual lifecycle verification remain required before crash
recovery is user-visible or releasable.
