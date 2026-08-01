#include "palmier/windows/preview_presentation_controller.hpp"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QMetaObject>
#include <QPlatformSurfaceEvent>
#include <QPointer>
#include <QResizeEvent>
#include <QScopeGuard>
#include <QThread>

#include <exception>
#include <stdexcept>
#include <utility>

namespace palmier::windows {
namespace {

class PreviewSessionAdapter final : public detail::QtPreviewSessionPort {
public:
    PreviewSessionAdapter(HWND window, render::D3d11PreviewDriver driver)
        : session_(window, driver) {}

    preview::PreviewPresentationReceipt resize(
        std::uint32_t width,
        std::uint32_t height,
        std::stop_token cancellation
    ) override {
        return session_.resize(width, height, cancellation);
    }

    preview::PreviewPresentationReceipt close() override { return session_.close(); }

private:
    preview::PreviewPresentationSession session_;
};

QThread* previewPresentationThread() {
    // Process-lifetime ownership prevents a UI-thread join during Qt teardown.
    static auto* thread = [] {
        auto* value = new QThread;
        value->setObjectName(QStringLiteral("PalmierPreviewPresentation"));
        value->start();
        return value;
    }();
    return thread;
}

QString outcomeName(preview::PreviewPresentationOutcome outcome) {
    using enum preview::PreviewPresentationOutcome;
    switch (outcome) {
    case changed: return QStringLiteral("ready");
    case noOp: return QStringLiteral("ready");
    case occluded: return QStringLiteral("occluded");
    case cancelled: return QStringLiteral("cancelled");
    case unavailable: return QStringLiteral("unavailable");
    case invalidated: return QStringLiteral("invalidated");
    case refused: return QStringLiteral("failed");
    case failed: return QStringLiteral("failed");
    case presented: return QStringLiteral("ready");
    case stale: return QStringLiteral("failed");
    }
    return QStringLiteral("failed");
}

QString errorName(const preview::PreviewPresentationReceipt& receipt) {
    using enum preview::PreviewPresentationOutcome;
    switch (receipt.outcome) {
    case unavailable: return QStringLiteral("previewUnavailable");
    case invalidated: return QStringLiteral("previewInvalidated");
    case refused: return QStringLiteral("previewRefused");
    case failed: return QStringLiteral("previewFailed");
    default: return {};
    }
}

preview::PreviewPresentationReceipt failedReceipt() {
    preview::PreviewPresentationReceipt receipt;
    receipt.state = preview::PreviewPresentationState::failed;
    receipt.outcome = preview::PreviewPresentationOutcome::failed;
    receipt.stage = preview::PreviewPresentationStage::validate;
    receipt.failure = preview::PreviewPresentationFailureCode::invariantFailure;
    receipt.hresult = E_FAIL;
    return receipt;
}

}

struct detail::QtPreviewWorkerState final {
    std::unique_ptr<QtPreviewSessionPort> session;
};

PreviewNativeWindow::PreviewNativeWindow(QWindow* parent) : QWindow(parent) {
    setFlags(Qt::FramelessWindowHint);
}

bool PreviewNativeWindow::event(QEvent* event) {
    if (event->type() == QEvent::WinIdChange) emit nativeSurfaceChanged();
    if (event->type() == QEvent::PlatformSurface) {
        const auto* surfaceEvent = static_cast<QPlatformSurfaceEvent*>(event);
        if (surfaceEvent->surfaceEventType()
            == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
            emit nativeSurfaceAboutToBeDestroyed();
        }
    }
    return QWindow::event(event);
}

void PreviewNativeWindow::exposeEvent(QExposeEvent* event) {
    QWindow::exposeEvent(event);
    emit nativeSurfaceChanged();
}

void PreviewNativeWindow::resizeEvent(QResizeEvent* event) {
    QWindow::resizeEvent(event);
    emit nativeSurfaceChanged();
}

PreviewPresentationController::PreviewPresentationController(QObject* parent)
    : PreviewPresentationController(render::D3d11PreviewDriver::hardware, parent) {}

PreviewPresentationController::PreviewPresentationController(
    render::D3d11PreviewDriver driver,
    QObject* parent
) : PreviewPresentationController(
    [driver](HWND window) {
        return std::make_unique<PreviewSessionAdapter>(window, driver);
    },
    parent
) {}

PreviewPresentationController::PreviewPresentationController(
    detail::QtPreviewSessionFactory factory,
    QObject* parent
) : QObject(parent),
    factory_(std::move(factory)),
    workerState_(std::make_shared<detail::QtPreviewWorkerState>()),
    dispatcher_(new QObject),
    window_(std::make_unique<PreviewNativeWindow>()) {
    if (!factory_) throw std::invalid_argument("preview session factory is required");
    dispatcher_->moveToThread(previewPresentationThread());
    connect(
        window_.get(),
        &PreviewNativeWindow::nativeSurfaceChanged,
        this,
        &PreviewPresentationController::refreshNativeSurface
    );
    connect(
        window_.get(),
        &PreviewNativeWindow::nativeSurfaceAboutToBeDestroyed,
        this,
        &PreviewPresentationController::nativeSurfaceWillBeDestroyed
    );
}

PreviewPresentationController::~PreviewPresentationController() {
    if (window_) disconnect(window_.get(), nullptr, this, nullptr);
    if (activeCancellation_) activeCancellation_->request_stop();
    if (dispatcher_ == nullptr) return;
    const auto state = workerState_;
    auto* retiredWindow = !shutdownComplete_ && (sessionExists_ || operationActive_)
        ? window_.release()
        : nullptr;
    auto* dispatcher = dispatcher_;
    dispatcher_ = nullptr;
    const QPointer<QObject> uiTarget(QCoreApplication::instance());
    QMetaObject::invokeMethod(dispatcher, [state, dispatcher, retiredWindow, uiTarget] {
        if (state->session) {
            static_cast<void>(state->session->close());
            state->session.reset();
        }
        if (retiredWindow != nullptr && uiTarget) {
            QMetaObject::invokeMethod(uiTarget.data(), [retiredWindow] {
                delete retiredWindow;
            }, Qt::QueuedConnection);
        }
        dispatcher->deleteLater();
    }, Qt::QueuedConnection);
}

QWindow* PreviewPresentationController::window() noexcept { return window_.get(); }
bool PreviewPresentationController::ready() const noexcept { return ready_; }
bool PreviewPresentationController::shutdownComplete() const noexcept {
    return shutdownComplete_;
}
QString PreviewPresentationController::state() const { return state_; }
QString PreviewPresentationController::errorCode() const { return errorCode_; }

bool PreviewPresentationController::requestShutdown() {
    if (shutdownComplete_) return true;
    shutdownRequested_ = true;
    pendingResize_.reset();
    setReady(false);
    setErrorCode({});
    setState(QStringLiteral("closing"));
    if (activeCancellation_) activeCancellation_->request_stop();
    if (operationActive_) return false;
    if (sessionExists_) {
        scheduleClose();
        return false;
    }
    completeShutdown(false);
    return true;
}

void PreviewPresentationController::refreshNativeSurface() {
    if (refreshingSurface_ || shutdownRequested_ || window_ == nullptr
        || !window_->isExposed()
        || QGuiApplication::platformName().compare(
            QStringLiteral("windows"),
            Qt::CaseInsensitive
        ) != 0) {
        return;
    }
    refreshingSurface_ = true;
    [[maybe_unused]] const auto guard = qScopeGuard([this] {
        refreshingSurface_ = false;
    });
    const auto handle = reinterpret_cast<HWND>(window_->winId());
    if (handle == nullptr || !IsWindow(handle)) return;
    RECT client{};
    if (!GetClientRect(handle, &client)) return;
    const auto width = client.right - client.left;
    const auto height = client.bottom - client.top;
    if (width <= 0 || height <= 0) return;
    if (nativeWindow_ == nullptr) {
        nativeWindow_ = handle;
        ++surfaceEpoch_;
        if (surfaceEpoch_ == 0) ++surfaceEpoch_;
        requestedWidth_ = static_cast<std::uint32_t>(width);
        requestedHeight_ = static_cast<std::uint32_t>(height);
        setState(QStringLiteral("attaching"));
        scheduleAttach(
            surfaceEpoch_,
            handle,
            static_cast<std::uint32_t>(width),
            static_cast<std::uint32_t>(height)
        );
        return;
    }
    if (handle != nativeWindow_) {
        nativeSurfaceWillBeDestroyed();
        return;
    }
    const auto pixelWidth = static_cast<std::uint32_t>(width);
    const auto pixelHeight = static_cast<std::uint32_t>(height);
    if (pixelWidth == requestedWidth_ && pixelHeight == requestedHeight_) return;
    requestedWidth_ = pixelWidth;
    requestedHeight_ = pixelHeight;
    PendingResize request{
        surfaceEpoch_,
        pixelWidth,
        pixelHeight,
    };
    if (operationActive_ || !sessionExists_) {
        pendingResize_ = request;
    } else {
        scheduleResize(request);
    }
}

void PreviewPresentationController::nativeSurfaceWillBeDestroyed() {
    if (nativeWindow_ == nullptr || shutdownRequested_) return;
    static_cast<void>(requestShutdown());
}

void PreviewPresentationController::scheduleAttach(
    std::uint64_t surfaceEpoch,
    HWND window,
    std::uint32_t width,
    std::uint32_t height
) {
    operationActive_ = true;
    const auto serial = ++operationSerial_;
    activeCancellation_ = std::make_shared<std::stop_source>();
    const auto cancellation = activeCancellation_;
    const auto state = workerState_;
    const auto factory = factory_;
    const QPointer<PreviewPresentationController> controller(this);
    QMetaObject::invokeMethod(dispatcher_, [
        state, factory, window, width, height, cancellation, controller, serial, surfaceEpoch
    ] {
        OperationResult result;
        result.kind = OperationKind::attach;
        result.serial = serial;
        result.surfaceEpoch = surfaceEpoch;
        try {
            state->session = factory(window);
            result.receipt = state->session->resize(
                width,
                height,
                cancellation->get_token()
            );
            result.sessionExists = true;
        } catch (...) {
            state->session.reset();
            result.receipt = failedReceipt();
        }
        if (!controller) return;
        QMetaObject::invokeMethod(controller.data(), [controller, result] {
            if (controller) controller->completeOperation(result);
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}

void PreviewPresentationController::scheduleResize(PendingResize request) {
    operationActive_ = true;
    const auto serial = ++operationSerial_;
    activeCancellation_ = std::make_shared<std::stop_source>();
    const auto cancellation = activeCancellation_;
    const auto state = workerState_;
    const QPointer<PreviewPresentationController> controller(this);
    QMetaObject::invokeMethod(dispatcher_, [state, request, cancellation, controller, serial] {
        OperationResult result;
        result.kind = OperationKind::resize;
        result.serial = serial;
        result.surfaceEpoch = request.surfaceEpoch;
        try {
            result.receipt = state->session
                ? state->session->resize(
                    request.width,
                    request.height,
                    cancellation->get_token()
                )
                : failedReceipt();
            result.sessionExists = state->session != nullptr;
        } catch (...) {
            result.receipt = failedReceipt();
            result.sessionExists = state->session != nullptr;
        }
        if (!controller) return;
        QMetaObject::invokeMethod(controller.data(), [controller, result] {
            if (controller) controller->completeOperation(result);
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}

void PreviewPresentationController::scheduleClose() {
    operationActive_ = true;
    const auto serial = ++operationSerial_;
    const auto state = workerState_;
    const QPointer<PreviewPresentationController> controller(this);
    QMetaObject::invokeMethod(dispatcher_, [state, controller, serial, epoch = surfaceEpoch_] {
        OperationResult result;
        result.kind = OperationKind::close;
        result.serial = serial;
        result.surfaceEpoch = epoch;
        try {
            result.receipt = state->session ? state->session->close() : failedReceipt();
        } catch (...) {
            result.receipt = failedReceipt();
        }
        state->session.reset();
        result.sessionExists = false;
        if (!controller) return;
        QMetaObject::invokeMethod(controller.data(), [controller, result] {
            if (controller) controller->completeOperation(result);
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}

void PreviewPresentationController::completeOperation(OperationResult result) {
    if (result.serial != operationSerial_) return;
    operationActive_ = false;
    activeCancellation_.reset();
    sessionExists_ = result.sessionExists;
    if (result.kind == OperationKind::close) {
        completeShutdown(true, errorName(result.receipt));
        return;
    }
    if (shutdownRequested_) {
        if (sessionExists_) scheduleClose();
        else completeShutdown();
        return;
    }
    if (result.surfaceEpoch != surfaceEpoch_) return;
    const auto outcome = result.receipt.outcome;
    const bool usable = outcome == preview::PreviewPresentationOutcome::changed
        || outcome == preview::PreviewPresentationOutcome::noOp
        || outcome == preview::PreviewPresentationOutcome::occluded;
    setReady(usable);
    if (shutdownRequested_ || shutdownComplete_) return;
    setState(outcomeName(outcome));
    if (shutdownRequested_ || shutdownComplete_) return;
    setErrorCode(errorName(result.receipt));
    if (shutdownRequested_ || shutdownComplete_) return;
    if (pendingResize_) {
        const auto pending = *pendingResize_;
        pendingResize_.reset();
        scheduleResize(pending);
    }
}

void PreviewPresentationController::completeShutdown(bool notify, QString errorCode) {
    if (shutdownComplete_) return;
    shutdownComplete_ = true;
    sessionExists_ = false;
    nativeWindow_ = nullptr;
    setReady(false);
    setErrorCode(std::move(errorCode));
    setState(errorCode_.isEmpty() ? QStringLiteral("closed") : QStringLiteral("failed"));
    emit shutdownCompleteChanged();
    if (notify) emit shutdownReady();
}

void PreviewPresentationController::setReady(bool value) {
    if (ready_ == value) return;
    ready_ = value;
    emit readyChanged();
}

void PreviewPresentationController::setState(QString value) {
    if (state_ == value) return;
    state_ = std::move(value);
    emit stateChanged();
}

void PreviewPresentationController::setErrorCode(QString value) {
    if (errorCode_ == value) return;
    errorCode_ = std::move(value);
    emit errorCodeChanged();
}

}
