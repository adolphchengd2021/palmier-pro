# ADR 0023: Host one preview HWND behind a Qt background executor

- Status: Accepted for M1 Qt preview lifecycle bootstrap
- Date: 2026-08-01
- Owner: Windows Qt shell and preview session
- Applies to: one Qt-native child window and its preview-session lifetime

## Decision

QML embeds a Qt-owned `QWindow` with `WindowContainer`. On Windows the embedded
window supplies the HWND used by `PreviewPresentationSession`; Palmier Pro does
not create a second Win32 child window or wrap an externally owned HWND. The Qt
UI thread owns window embedding, exposure, and geometry only.

The controller reads the HWND client rectangle on the UI thread and publishes
positive physical dimensions to one process-lifetime presentation thread. The
background thread constructs, resizes, closes, and destroys the session. It is
the only executor that can reach decoding, render, D3D11, DXGI, or surface
operations. The initial attach performs one background resize so GPU and swap
chain setup cannot block the Qt UI thread.

Only one synchronous operation is admitted at a time. Repeated exposure events
with the same client size are ignored, and a resize burst keeps one latest
distinct size while an operation is active. Zero-size, hidden, and unexposed
windows submit no resize. A changed HWND is terminal for the current surface;
the controller requests cancellation and close instead of silently moving the
swap chain to another window.

Application close remains asynchronous. QML asks both the project-load and
preview controllers to shut down, vetoes the first close while either owner is
active, and closes only after both receipts arrive. Preview shutdown requests
the active stop source before serialized close. Qt destroys the embedded child
window only after the session has closed and been destroyed on the background
thread. If a controller is destroyed without the normal handshake, it retires
the `QWindow`, closes and destroys the session on the background thread, and
posts final window deletion back to the UI thread without waiting there.
One guarded executable-level drain covers explicit quit before exit,
`aboutToQuit` as the system-termination fallback, and a final post-event-loop
fallback before destroying the QML engine. A preview close failure changes an
otherwise successful process exit code. Controller receipt publication checks
for reentrant shutdown after each signal-emitting state setter so a nested drain
cannot restore `ready` after reaching `closed`.

The Qt shell preset now enables the locked FFmpeg prototype and vcpkg toolchain
because the runtime controller links the real preview session. It shares the
runner-image-specific binary cache key with the media workflow.

## Tests

Deterministic injected tests embed the real Qt `QWindow` with
`WindowContainer`, verify the native `WS_CHILD` relationship, prove factory,
resize, close, and destruction stay on one non-UI thread, and hold the first
resize while a burst confirms only the latest physical size is applied. A
separate WARP smoke uses the real session, Qt child window, background resize,
and close path. A second executable smoke requests guarded application exit
while the real Qt/WARP host is active and succeeds only after shutdown drain.
Controller regressions perform a nested drain from `readyChanged`, prove the
final state remains `closed`, and reenter shutdown from a close-failure signal
without losing the single terminal receipt. Existing QML tests cover read-only
shell state and the combined project/preview shutdown barrier.

## Evidence boundary

A green MSVC `/W4 /WX` build and Windows-platform Qt tests prove native child
ownership, serialized background setup and teardown, bounded resize, and the
shutdown gate on the runner. They do not prove a project media candidate,
playback cadence, visible pixels, overlays, physical GPU performance, DPI and
multi-display behavior, device recreation, A/V synchronization, or Windows 10
build 19045 compatibility.

## References

- [QML WindowContainer](https://doc.qt.io/qt-6/qml-qtquick-windowcontainer.html)
- [QWindow](https://doc.qt.io/qt-6/qwindow.html)
- [GetClientRect](https://learn.microsoft.com/windows/win32/api/winuser/nf-winuser-getclientrect)
- [CreateSwapChainForHwnd](https://learn.microsoft.com/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgifactory2-createswapchainforhwnd)
