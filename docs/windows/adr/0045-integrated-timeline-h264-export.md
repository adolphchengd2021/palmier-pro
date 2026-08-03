# ADR 0045: Export the active timeline from the Qt shell

- Status: Accepted for the Windows technical MVP
- Applies to: Qt/QML H.264 export admission, lifecycle, and receipts

## Context

The Qt shell currently requires a selected video clip and calls the legacy
selected-clip workflow. The lower-level exporter now supports the complete
bounded static video schedule. A product export action must represent the
filmmaker intent to export the active timeline, not expose an implementation
slice or require an otherwise unrelated selection.

## Decision

The QML action is **Export Timeline** and supplies only one local MP4
destination. It does not supply track IDs, clip IDs, media IDs, source paths,
timeline IDs, or render settings.

```text
Main.qml
  -> ProjectExportController::exportTimeline
  -> exportProjectTimelineH264
  -> compileStaticVideoTimeline
  -> exportStaticProjectTimelineH264
```

The controller captures the current immutable `ProjectSessionSnapshot` and
package identity only after the matching presentation revision is installed.
The background workflow compiles the active timeline and resolves every stable
scheduled clip through the package-safe media resolver before encoding. The
controller never copies timeline eligibility or source-resolution logic.

The existing single-job, cancellation, shutdown drain, immutable revision,
`completedOutdated`, terminal error, and output receipt rules from ADR 0034 are
unchanged. A second admission is refused. A project generation, edit revision,
package identity, or presentation-availability change requests cancellation;
a successfully committed older snapshot remains an explicit outdated success.

The selected-clip exporter and workflow remain internal compatibility seams
with their tests. They are no longer the Qt product action.

## Evidence boundary

Controller tests verify the timeline request reaches one background worker,
duplicate admission is refused, cancellation and shutdown drain, persistence-
only publications do not cancel, newer revisions mark committed output
outdated, and every exporter failure is stable. Offscreen QML tests verify the
timeline export and cancel controls load without requiring a clip selection.
Manual Windows testing is still required for the file dialog, focus, Escape,
cancel confirmation, close behavior, and the visible completed-output path.
