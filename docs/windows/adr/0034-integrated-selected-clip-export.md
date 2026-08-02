# ADR 0034: Integrate selected-clip export with the live Qt project

- Status: Accepted for the Windows technical MVP
- Applies to: Technical MVP gate G0

## Decision

The Qt shell exports one selected static video clip through three owned layers:

```text
Main.qml
  -> ProjectExportController
  -> exportProjectClipH264
  -> exportStaticProjectH264
```

QML supplies only the selected persisted track ID, persisted clip ID, and a
local MP4 destination. It never supplies an input media path or timeline ID.
The controller captures one immutable `ProjectSessionSnapshot` from the runtime
mailbox. The Qt-free workflow resolves the active timeline, selected entities,
manifest entry, and canonical media path from that snapshot and package.

The load coordinator marks the presentation unavailable as soon as a newer
runtime revision is observed and marks it ready only after the matching model is
installed. Export admission therefore cannot combine a stale visible selection
with a newer runtime snapshot.

Preview and export share `resolveProjectMediaReference`. Duplicate manifest
IDs, type mismatches, unsafe package-relative paths, package escapes, and
offline files are explicit failures. Synchronous manifest and media path work
runs only on the export worker.

## State and lifecycle

One controller owns at most one job. A second admission is refused without
changing the active job. Editing or replacing the project requests cooperative
cancellation when the published generation or revision differs from the
captured snapshot. Persistence-only state changes do not cancel an export.

Cancellation before destination installation leaves no final output. Once
the exporter returns a receipt, the file is committed: a newer project
generation or revision, or a different package identity, produces
`completedOutdated`, not a false cancellation or failure. A later projection of
that same project does not erase the committed receipt.
Shutdown requests cancellation, rejects late admissions, and waits for the
terminal result before the runtime is closed.

The staging file is flushed through its verified handle after FFmpeg closes the
writer and before independent decode or atomic installation. Flush failure
preserves an existing destination and removes the identified staging object.
The commit handle shares write access with the still-open verification handle;
the verification handle itself continues to deny third-party write opens.

## Tests

- Resolver tests cover unique, duplicate, missing, mismatched, unsafe, offline,
  and cancelled media references.
- Encoder-independent staging tests prove flush-to-install handle compatibility
  and inject a flush failure with file-identity comparison, destination
  preservation, native error retention, and staging cleanup.
- A native workflow test exports through manifest resolution and independently
  decodes the installed H.264 video.
- Qt controller tests prove off-GUI execution, single admission, presentation
  gating, persistence-only refresh, package replacement, stale success, shutdown
  cancellation, and stable error mapping without a production encoder.
- QML smoke coverage requires the export and cancel controls to load.

## Evidence boundary

This slice exports only one explicitly selected static video clip with the
existing CFR, source-frame-zero, H.264 Media Foundation, and video-only limits.
It is not general timeline export and does not prove audio, H.265, VFR, trim
seeking, multiple visible layers, overwrite UX, Windows 10 build 19045, or
physical-GPU behavior. File dialog, focus, Escape, cancellation, and close
behavior still require manual UI acceptance.
