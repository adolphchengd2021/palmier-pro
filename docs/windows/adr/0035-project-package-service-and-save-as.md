# ADR 0035: Own the Windows project package and commit Save As atomically

- Status: Accepted for the Windows technical MVP
- Applies to: M2 project lifecycle foundation

## Decision

`ProjectPackageService` is the authoritative owner of the active `.palmier`
path, project generation, identity serial, and package write lease. Project
loading acquires a normalized-path Windows named mutex on the background reader
before installing the runtime document. A different process receives the stable
`projectPackageBusy` refusal while that lease is held. A second service in the
same process is refused by the logical lock registry as well. Mutex acquisition and
release stay on one dedicated owner thread because Win32 mutex ownership is
thread-affine.

Save and Save As both enter this service. Save As captures one exact runtime
snapshot, inventories the complete source package, and creates a unique
same-volume sibling `.partial` directory. It copies and independently
verifies every regular file except `project.json`, preserves unknown files and
empty directories, rejects reparse points and nested source/destination paths,
then writes and flushes the canonical runtime snapshot as the staged
`project.json`.

The source inventory must still match and the destination must still be absent
immediately before one handle-based directory rename. A pre-commit error or
cancellation removes only the identified staging directory. The live source is
never replaced or removed.

The package writer does not mark Save As state persisted. After its disk commit,
the service adopts the preallocated destination identity and calls
`ProjectRuntime::markPersisted` inside one identity critical section. A failed
acknowledgement rolls the identity back and leaves runtime state dirty.
Qt then rebases package-local preview media and republishes the identity to Save,
export, and presentation consumers. A newer edit remains dirty; an
unacknowledged committed copy remains at its destination but does not retarget
the live project.

## Invariants

- Package validation, inventory, copy, flush, verification, cleanup, and lock
  acquisition run off the GUI thread.
- The destination must not exist. Save As never silently overwrites a package.
- Reparse points, source/destination containment, missing files, source changes,
  destination races, cancellation, and cleanup failures are explicit outcomes.
- The project generation and identity serial are revalidated before adoption.
- The package lease is retained until replacement or application teardown.

## Tests

- Project-package tests save edited runtime state to a new package and reopen
  both source and destination independently.
- The same test preserves unknown files, media bytes, and an empty directory,
  verifies copy receipts, and confirms that only the destination identity is
  adopted.
- Negative coverage refuses existing and nested destinations without changing
  the live identity.
- Checkpoint coverage cancels each pre-commit phase and requires source, dirty
  state, destination absence, and staging cleanup to remain consistent.
- A child-process test holds the named package lease, proves a second process
  receives `projectPackageBusy`, then proves process exit releases the lease.
- Qt smoke coverage requires the Save As dialog and control to load.

## Evidence boundary

This slice establishes explicit Save and Save As plus a package lifecycle owner.
It does not provide autosave, overwrite UX, recovery journals, package migration,
or serialization with future media import/removal jobs. The UI exposes
cooperative Save cancellation, but file-dialog focus, Escape, close during Save
As, removable-volume failure, low-space behavior, and
clean Windows 10 build 19045 interaction still require manual acceptance.
