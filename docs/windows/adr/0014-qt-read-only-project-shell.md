# ADR 0014: Add a bounded Qt read-only project shell

- Status: Accepted for M0 prototype
- Date: 2026-08-01
- Owner: Windows application engineering
- Applies to: first `.palmier` picker and read-only timeline shell

## Decision

Add Qt as an optional Windows-only target behind
`PALMIER_ENABLE_QT_SHELL`. Keep the default contract, render, media, and audio
builds independent of Qt. The prototype remains locked to Qt 6.10.3 for MSVC
2022 x86_64 and uses only Core, Gui, Qml, Quick, Quick Controls, Quick Dialogs,
and Concurrent at runtime. Qt Test is a test-only module.
CI installs the exact Qt build with `aqtinstall` 3.3.0 and `py7zr` 1.0.0;
neither the floating install action nor a Qt account is required.
Qt 6.10.3 is the newest Windows desktop release exposed by that pinned client.

The open-source Qt artifacts are for prototype and CI evaluation only. This
decision does not approve product distribution. A release must choose one
licensing route, verify the exact shipped module and third-party notices, and
either satisfy all LGPL/GPL obligations or use compatible commercial terms.
The installer must dynamically deploy the reviewed Qt libraries and QML
imports; static linking, `windeployqt`, signing, source offers, relinking, and
SBOM evidence remain release gates.

Keep `core/project` Qt-free. A synchronous package-reader seam validates a
directory-form `.palmier` package, reads only `project.json`, enforces a 64 MiB
maximum even if the file changes during the read, and caps the parsed DOM at
500,000 JSON values and 64 MiB of decoded string data. It checks cancellation
at I/O boundaries and throughout parsing and normalization, and delegates to the
existing `readProject` source of truth. Its caller must run the synchronous
filesystem work off the UI thread. The full JSON DOM is authoritative while
the read-only document exists, but the Qt shell retains only its immutable
timeline projection. No writer exists, so unknown fields are never rewritten.

The Qt boundary owns only presentation state and orchestration. A folder
dialog produces a local directory URL. The coordinator starts background
loading, snapshots immutable timeline data, and atomically replaces the Qt
models on the GUI thread. A monotonically increasing load generation rejects
stale completion after another open request; the stop token rejects cancelled
work before commit. Failure retains the last successfully loaded model and
exposes a failed terminal state. Cancellation briefly exposes `cancelling`,
then restores the last committed state. Neither path installs partial data or
a success-shaped result.

Each coordinator admits at most one active read and one latest pending request.
A new open cancels the active request and replaces, rather than appends to, the
pending slot. The process-lifetime serial executor never captures the
coordinator. Window close requests cancellation, drops the pending request,
and remains open until the admitted worker reaches its terminal callback;
QObject teardown still disconnects callbacks safely. Synchronous
filesystem calls cannot be interrupted while inside the operating system, so
the worker checks cancellation before and after every 64 KiB read, around JSON
parsing, throughout projection construction, and before GUI commit.

Unsafe clip frame ranges retain the reader's `unsafeFrameRange` warning and are
omitted from this visual projection without rejecting otherwise readable
tracks. Clip end-frame arithmetic is checked before addition. To bound QML
object creation and Qt heap amplification, the prototype projects only the
active timeline and retains only the diagnostic count, first diagnostic, and
explicit unsafe-clip omission count for the UI summary.
It explicitly rejects more than 200 tracks, 500 valid clips on one track, or
10,000 clips in the active timeline with `timelineTooDense`; it never silently
truncates. The worker derives exact
frame text plus bounded floating-point offset and extent ratios for the visual
boundary. QML never converts or calculates with authoritative 64-bit frame
values. It renders one row per active-timeline track and positions read-only
clip blocks from the prepared ratios. The GUI commit moves the completed
snapshot into the model and does not rebuild a second entity tree.

QML renders the immutable models and forwards open or cancel intent. It does
not perform filesystem access, project mutation, frame arithmetic, persistence,
undo, media probing, playback, or export. Layout values that represent product
styling come from the Windows shell's shared QML `AppTheme` tokens rather than
being scattered through business views. Those values mirror their owning
Swift `AppTheme` constants, including the Windows-shell layout group.

## Tests

The Qt-free package-reader tests cover current and legacy packages, retained
unknown fields, missing package or `project.json`, wrong path kinds, empty and
malformed JSON, the exact size boundary, growth beyond the limit, and
deterministic cancellation before and during a chunked read.

Qt tests cover model roles, stable IDs, row ordering, frame values, warnings,
success-failure-success replacement, cancellation, and stale completion. An
offscreen QML smoke creates the shell and exercises empty, loaded, warning, and
failure states without using a real project or native dialog. A second smoke
starts the packaged QML module through the real executable entry point.

## Evidence boundary

A green MSVC and offscreen Qt test proves compilation, package-read contracts,
model publication, and QML object creation on the Windows Server 2022 runner.
It does not prove native folder-dialog behavior, visual quality, keyboard and
focus behavior, high-DPI transitions, accessibility, deployment completeness,
physical Windows 10 build 19045 compatibility, or editing. Those require a
manual desktop matrix before the shell can leave M0.

The prototype does not add save, Save As, project locking, autosave, undo,
recent projects, media resolution, thumbnails, playback, or a `ProjectSession`.
Those capabilities must enter through their owning domain operations rather
than growing inside the Qt coordinator.

## Primary references

- [Qt for Windows](https://doc.qt.io/qt-6/windows.html)
- [Qt CMake QML integration](https://doc.qt.io/qt-6/qtqml-cmake-integration.html)
- [Qt Quick deployment](https://doc.qt.io/qt-6/qtquick-deployment.html)
- [Qt for Windows deployment](https://doc.qt.io/qt-6/windows-deployment.html)
- [Qt open-source licensing](https://www.qt.io/development/download-open-source)
- [Qt LGPL obligations](https://www.qt.io/development/open-source-lgpl-obligations)
