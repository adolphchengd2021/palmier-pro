# ADR 0048: Schedule recovery beside package autosave

- Status: Accepted for the Windows technical MVP
- Date: 2026-08-03
- Owner: Windows project persistence
- Applies to: P0

## Decision

`ProjectPersistenceController` owns recovery scheduling because it already owns
the active package identity, runtime publication token, Save, Save As, package
autosave, cancellation, and shutdown admission. Recovery filesystem work stays
inside `ProjectRecoveryJournal` and runs on the controller's existing
process-lifetime serial persistence pool.

Each newer dirty runtime publication restarts one 2-second recovery timer. The
existing 30-second package autosave remains an independent policy. One admitted
recovery write snapshots an exact runtime generation and publication. A newer
publication arriving during the write schedules one follow-up after completion.
An unchanged recovery failure is visible but does not retry until a newer
publication arrives.

Manual Save, Save As, and package autosave stop the recovery timer and cancel an
admitted recovery write before queueing package persistence. A successful
package commit retires only a journal for the old package identity whose
generation matches and whose revision is no newer than the committed receipt.
Save As retires the source identity before the controller adopts the destination.
A retirement failure does not turn a committed Save into failure; it remains a
separate visible recovery warning.

If Save fails, the unchanged dirty state schedules recovery again because the
Save attempt superseded its prior protection. Package autosave still waits for
a newer publication, so an unchanged disk failure cannot create a retry loop.

Accepted shutdown stops both timers, cancels admitted recovery work, and waits
for both recovery and Save completion. Neither watcher joins its worker on the
GUI thread. The last finishing operation emits the existing shutdown-ready
signal exactly once for the active combination.

## Tests

Qt tests prove dirty-publication coalescing, off-GUI execution, failure waiting
for a newer publication, Save cancellation plus exact retirement, failed-Save
recovery re-arming, and shutdown cancellation and drain. The lower-level ADR
0047 tests continue to own atomic replacement, stale identity, corruption,
size, and staging cleanup behavior.

## Evidence boundary

This decision makes recovery snapshots automatic and retires them after Save.
Startup inspection, recovered-session installation, recovery choice UI, stale
candidate presentation, media transaction recovery, and Windows 10 19045
manual lifecycle verification remain required. Offscreen Qt tests do not prove
the visible status or warning layout.
