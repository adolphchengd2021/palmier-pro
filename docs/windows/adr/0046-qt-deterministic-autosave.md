# ADR 0046: Debounce Qt autosave without retry loops

- Status: Accepted for the Windows technical MVP
- Date: 2026-08-03
- Owner: Windows project persistence UI
- Applies to: P0

## Decision

`ProjectPersistenceController` owns one 30-second single-shot autosave timer on
the GUI thread. A newer dirty runtime publication restarts that timer. The timer
only admits work; the existing process-lifetime serial background pool and
`ProjectPackageService` still own snapshotting, package serialization, durable
flush, atomic replacement, and runtime acknowledgement.

The monotonic runtime publication token is the retry boundary. Each admitted
save records the latest observed token. If that save completes while a newer
dirty publication exists, the controller schedules one new debounce interval.
If a save fails and the token has not advanced, autosave remains stopped. It
does not poll, back off, or repeatedly retry an unchanged failing state.

Manual Save and Save As supersede a pending autosave by stopping the timer
before admission. Project replacement resets the pending timer and observed
token. An admitted operation remains the only writer until completion; an edit
during that operation cannot start a parallel save. Accepted shutdown stops a
pending timer and drains any already admitted writer through the existing close
lifecycle.

Autosave failures use the existing stable error code and message surface. A
committed older snapshot retains the existing
`saveCommittedNewerChangesRemain` warning until the newer state is saved.

## Tests

Qt tests inject a zero-duration debounce without sleeps. They prove that two
dirty publications in one event-loop turn coalesce into one off-GUI save, an
unchanged failure leaves no timer armed, a later publication admits exactly one
new attempt, and an edit during an admitted save causes one serialized follow-up
save of the newer state. A manual Save and accepted shutdown each prove that a
long pending autosave is disarmed before it can admit work.

## Evidence boundary

This slice autosaves the same supported `project.json` mutation surface as
manual Save. It does not add a recovery journal, crash-recovery chooser,
package-wide media transactions, low-space recovery, removable-volume policy,
or Windows 10 19045 manual UI certification.
