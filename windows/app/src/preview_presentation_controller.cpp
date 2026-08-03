#include "palmier/windows/preview_presentation_controller.hpp"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QMetaObject>
#include <QPlatformSurfaceEvent>
#include <QPointer>
#include <QResizeEvent>
#include <QScopeGuard>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <exception>
#include <limits>
#include <stdexcept>
#include <utility>

namespace palmier::windows {
namespace {

preview::PreviewPresentationReceipt failedReceipt();

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

    preview::PreviewPresentationReceipt play(
        const PreviewMediaCandidateProjection& candidate,
        std::stop_token cancellation
    ) override {
        const auto* layer = candidate.firstRenderLayer();
        const auto* source = layer == nullptr
            ? nullptr
            : candidate.sourceForClip(layer->clipId);
        if (
            layer == nullptr
            || source == nullptr
            || candidate.renderTimeline.framesPerSecond <= 0
        ) {
            auto receipt = failedReceipt();
            receipt.outcome = preview::PreviewPresentationOutcome::refused;
            receipt.failure = preview::PreviewPresentationFailureCode::invalidRequest;
            receipt.hresult = E_INVALIDARG;
            return receipt;
        }
        return session_.play(
            source->inputPath,
            layer->timelineStartFrame,
            {static_cast<std::uint32_t>(candidate.renderTimeline.framesPerSecond), 1},
            {*layer},
            cancellation
        );
    }

    preview::PreviewPresentationReceipt tick(
        std::uint64_t expectedGeneration,
        std::stop_token cancellation
    ) override {
        return session_.tick(expectedGeneration, cancellation);
    }

    preview::PreviewPresentationReceipt seek(
        std::uint64_t expectedGeneration,
        std::int64_t targetTimelineFrame,
        media::HeadlessAvPlaybackSeekMode mode,
        std::stop_token cancellation
    ) override {
        return session_.seek(
            expectedGeneration,
            targetTimelineFrame,
            mode,
            cancellation
        );
    }

    preview::PreviewPresentationReceipt cancel(
        std::uint64_t expectedGeneration
    ) override {
        return session_.cancel(expectedGeneration);
    }

    preview::PreviewPresentationReceipt pause(
        std::uint64_t expectedGeneration
    ) override {
        return session_.pause(expectedGeneration);
    }

    preview::PreviewPresentationReceipt resume(
        std::uint64_t expectedGeneration
    ) override {
        return session_.resume(expectedGeneration);
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

QString presentationStateName(const preview::PreviewPresentationReceipt& receipt) {
    using enum preview::PreviewPresentationState;
    switch (receipt.state) {
    case idle: return QStringLiteral("ready");
    case playing: return QStringLiteral("playing");
    case paused: return QStringLiteral("paused");
    case completed: return QStringLiteral("completed");
    case cancelled: return QStringLiteral("cancelled");
    case invalidated: return QStringLiteral("invalidated");
    case failed: return QStringLiteral("failed");
    case closed: return QStringLiteral("closed");
    }
    return outcomeName(receipt.outcome);
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
qint64 PreviewPresentationController::currentFrame() const noexcept {
    return currentFrame_;
}
qint64 PreviewPresentationController::minimumFrame() const noexcept {
    return minimumFrame_;
}
qint64 PreviewPresentationController::maximumFrame() const noexcept {
    return maximumFrame_;
}

preview::PreviewPresentationReceipt
PreviewPresentationController::latestPlaybackReceipt() const noexcept {
    return latestPlaybackReceipt_;
}

void PreviewPresentationController::replaceProjectPreview(
    std::uint64_t projectGeneration,
    ProjectPreviewProjection preview
) {
    replaceProjectPreview(projectGeneration, 0, std::move(preview));
}

void PreviewPresentationController::replaceProjectPreview(
    std::uint64_t projectGeneration,
    std::uint64_t projectRevision,
    ProjectPreviewProjection preview
) {
    if (shutdownRequested_ || projectGeneration == 0) return;
    if (desiredPreview_) {
        const bool older = projectGeneration < desiredPreview_->projectGeneration
            || (projectGeneration == desiredPreview_->projectGeneration
                && projectRevision < desiredPreview_->projectRevision);
        const bool sameState = projectGeneration == desiredPreview_->projectGeneration
            && projectRevision == desiredPreview_->projectRevision;
        const bool resolvesInvalidation = sameState
            && desiredPreview_->preview.availability
                == PreviewCandidateAvailability::invalidated
            && preview.availability != PreviewCandidateAvailability::invalidated;
        if (older || (sameState && !resolvesInvalidation)) return;
    }
    ++sourceSerial_;
    if (sourceSerial_ == 0) ++sourceSerial_;
    desiredPreview_ = PendingPreview{
        sourceSerial_,
        projectGeneration,
        projectRevision,
        std::move(preview),
    };
    pendingSeek_.reset();
    if (desiredPreview_->preview.candidate.has_value()) {
        const auto* layer = desiredPreview_->preview.candidate->firstRenderLayer();
        if (layer != nullptr
            && layer->timelineStartFrame >= 0 && layer->durationFrames > 0
            && layer->timelineStartFrame
                <= (std::numeric_limits<std::int64_t>::max)()
                    - (layer->durationFrames - 1)) {
            setPreviewRange(
                layer->timelineStartFrame,
                layer->timelineStartFrame + layer->durationFrames - 1
            );
            setCurrentFrame(layer->timelineStartFrame);
        } else {
            setPreviewRange(0, -1);
            setCurrentFrame(0);
        }
    } else {
        setPreviewRange(0, -1);
        setCurrentFrame(0);
    }
    suppressedPreviewSerial_ = 0;
    tickScheduled_ = false;
    tickPending_ = false;
    pauseRequested_ = false;
    resumeRequested_ = false;
    if (
        activeCancellation_
        && activeOperationKind_
        && (*activeOperationKind_ == OperationKind::play
            || *activeOperationKind_ == OperationKind::tick)
    ) {
        activeCancellation_->request_stop();
    }
    setErrorCode({});
    if (shutdownRequested_ || shutdownComplete_) return;
    setState(QStringLiteral("switching"));
    if (shutdownRequested_ || shutdownComplete_) return;
    servicePendingWork();
}

bool PreviewPresentationController::pause() {
    if (shutdownRequested_ || shutdownComplete_ || !sessionExists_
        || playbackGeneration_ == 0) {
        return false;
    }
    if (pauseRequested_) return true;
    const bool resumeActive = resumeRequested_
        || (activeOperationKind_ && *activeOperationKind_ == OperationKind::resume);
    if (state_ == QStringLiteral("paused") && !resumeActive) return true;
    if (state_ != QStringLiteral("playing")
        && state_ != QStringLiteral("paused")) return false;
    pauseRequested_ = true;
    resumeRequested_ = false;
    tickPending_ = false;
    if (activeCancellation_ && activeOperationKind_
        && *activeOperationKind_ == OperationKind::tick) {
        activeCancellation_->request_stop();
    }
    servicePendingWork();
    return true;
}

bool PreviewPresentationController::resume() {
    if (shutdownRequested_ || shutdownComplete_ || !sessionExists_
        || playbackGeneration_ == 0) {
        return false;
    }
    if (resumeRequested_) return true;
    const bool pauseActive = pauseRequested_
        || (activeOperationKind_ && *activeOperationKind_ == OperationKind::pause);
    if (state_ == QStringLiteral("playing") && !pauseActive) return true;
    if (state_ != QStringLiteral("playing")
        && state_ != QStringLiteral("paused")) return false;
    resumeRequested_ = true;
    pauseRequested_ = false;
    servicePendingWork();
    return true;
}

bool PreviewPresentationController::seekToFrame(qint64 targetTimelineFrame) {
    if (shutdownRequested_ || shutdownComplete_ || !sessionExists_
        || playbackGeneration_ == 0 || !desiredPreview_
        || !desiredPreview_->preview.candidate.has_value()
        || targetTimelineFrame < minimumFrame_
        || targetTimelineFrame > maximumFrame_
        || (state_ != QStringLiteral("playing")
            && state_ != QStringLiteral("paused")
            && state_ != QStringLiteral("completed"))) {
        return false;
    }
    pendingSeek_ = PendingSeek{
        activePreviewSerial_,
        playbackGeneration_,
        targetTimelineFrame,
        state_ == QStringLiteral("playing")
            ? media::HeadlessAvPlaybackSeekMode::playing
            : media::HeadlessAvPlaybackSeekMode::paused,
    };
    pauseRequested_ = false;
    resumeRequested_ = false;
    tickPending_ = false;
    tickScheduled_ = false;
    if (activeCancellation_ && activeOperationKind_
        && (*activeOperationKind_ == OperationKind::tick
            || *activeOperationKind_ == OperationKind::seek)) {
        activeCancellation_->request_stop();
    }
    servicePendingWork();
    return true;
}

bool PreviewPresentationController::stepFrame(int delta) {
    if (delta != -1 && delta != 1) return false;
    if (shutdownRequested_ || shutdownComplete_ || !sessionExists_
        || playbackGeneration_ == 0 || !desiredPreview_
        || !desiredPreview_->preview.candidate.has_value()
        || (state_ != QStringLiteral("playing")
            && state_ != QStringLiteral("paused")
            && state_ != QStringLiteral("completed"))
        || currentFrame_ < minimumFrame_ || currentFrame_ > maximumFrame_)
        return false;
    if ((delta < 0 && currentFrame_ == minimumFrame_)
        || (delta > 0 && currentFrame_ == maximumFrame_)) {
        return false;
    }
    const auto target = currentFrame_ + delta;
    pendingSeek_ = PendingSeek{
        activePreviewSerial_,
        playbackGeneration_,
        target,
        media::HeadlessAvPlaybackSeekMode::paused,
    };
    pauseRequested_ = false;
    resumeRequested_ = false;
    tickPending_ = false;
    tickScheduled_ = false;
    if (activeCancellation_ && activeOperationKind_
        && (*activeOperationKind_ == OperationKind::tick
            || *activeOperationKind_ == OperationKind::seek)) {
        activeCancellation_->request_stop();
    }
    servicePendingWork();
    return true;
}

bool PreviewPresentationController::requestShutdown() {
    if (shutdownComplete_) return true;
    shutdownRequested_ = true;
    ++sourceSerial_;
    desiredPreview_.reset();
    pendingSeek_.reset();
    pendingResize_.reset();
    tickScheduled_ = false;
    tickPending_ = false;
    pauseRequested_ = false;
    resumeRequested_ = false;
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

void PreviewPresentationController::publishUnavailablePreview(
    const ProjectPreviewProjection& preview
) {
    switch (preview.availability) {
    case PreviewCandidateAvailability::noCandidate:
        setErrorCode({});
        if (shutdownRequested_ || shutdownComplete_) return;
        setState(QStringLiteral("empty"));
        break;
    case PreviewCandidateAvailability::offline:
        setErrorCode(QString::fromStdString(preview.reasonCode));
        if (shutdownRequested_ || shutdownComplete_) return;
        setState(QStringLiteral("offline"));
        break;
    case PreviewCandidateAvailability::unsupported:
        setErrorCode(QString::fromStdString(preview.reasonCode));
        if (shutdownRequested_ || shutdownComplete_) return;
        setState(QStringLiteral("unsupported"));
        break;
    case PreviewCandidateAvailability::invalidated:
        setErrorCode(QString::fromStdString(preview.reasonCode));
        if (shutdownRequested_ || shutdownComplete_) return;
        setState(QStringLiteral("invalidated"));
        break;
    case PreviewCandidateAvailability::available:
        break;
    }
}

void PreviewPresentationController::servicePendingWork() {
    if (operationActive_ || shutdownRequested_ || !sessionExists_) return;
    if (
        desiredPreview_
        && playbackGeneration_ != 0
        && activePreviewSerial_ != desiredPreview_->sourceSerial
    ) {
        scheduleCancel(desiredPreview_->sourceSerial, playbackGeneration_);
        return;
    }
    if (pendingResize_) {
        const auto pending = *pendingResize_;
        pendingResize_.reset();
        scheduleResize(pending);
        return;
    }
    if (!desiredPreview_) return;
    if (
        desiredPreview_->preview.availability != PreviewCandidateAvailability::available
        || !desiredPreview_->preview.candidate
    ) {
        activePreviewSerial_ = desiredPreview_->sourceSerial;
        playbackGeneration_ = 0;
        publishUnavailablePreview(desiredPreview_->preview);
        return;
    }
    if (activePreviewSerial_ != desiredPreview_->sourceSerial) {
        if (suppressedPreviewSerial_ == desiredPreview_->sourceSerial) return;
        schedulePlay(*desiredPreview_);
        return;
    }
    if (pendingSeek_.has_value()) {
        auto request = *pendingSeek_;
        pendingSeek_.reset();
        scheduleSeek(request);
        return;
    }
    if (pauseRequested_ && playbackGeneration_ != 0) {
        scheduleTransport(
            OperationKind::pause,
            activePreviewSerial_,
            playbackGeneration_
        );
        return;
    }
    if (resumeRequested_ && playbackGeneration_ != 0) {
        scheduleTransport(
            OperationKind::resume,
            activePreviewSerial_,
            playbackGeneration_
        );
        return;
    }
    if (tickPending_ && playbackGeneration_ != 0
        && state_ == QStringLiteral("playing")) {
        tickPending_ = false;
        scheduleTick(activePreviewSerial_, playbackGeneration_);
    }
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
    activeOperationKind_ = OperationKind::attach;
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
    activeOperationKind_ = OperationKind::resize;
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

void PreviewPresentationController::schedulePlay(PendingPreview request) {
    if (!request.preview.candidate) return;
    operationActive_ = true;
    activeOperationKind_ = OperationKind::play;
    const auto serial = ++operationSerial_;
    activeCancellation_ = std::make_shared<std::stop_source>();
    const auto cancellation = activeCancellation_;
    const auto state = workerState_;
    const auto candidate = *request.preview.candidate;
    const QPointer<PreviewPresentationController> controller(this);
    QMetaObject::invokeMethod(dispatcher_, [
        state,
        candidate,
        cancellation,
        controller,
        serial,
        epoch = surfaceEpoch_,
        sourceSerial = request.sourceSerial
    ] {
        OperationResult result;
        result.kind = OperationKind::play;
        result.serial = serial;
        result.surfaceEpoch = epoch;
        result.sourceSerial = sourceSerial;
        try {
            result.receipt = state->session
                ? state->session->play(candidate, cancellation->get_token())
                : failedReceipt();
            result.sessionExists = state->session != nullptr;
        } catch (...) {
            result.receipt = failedReceipt();
            result.sessionExists = state->session != nullptr;
        }
        result.playbackGeneration = result.receipt.generation;
        if (!controller) return;
        QMetaObject::invokeMethod(controller.data(), [controller, result] {
            if (controller) controller->completeOperation(result);
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}

void PreviewPresentationController::scheduleTick(
    std::uint64_t sourceSerial,
    std::uint64_t playbackGeneration
) {
    operationActive_ = true;
    activeOperationKind_ = OperationKind::tick;
    const auto serial = ++operationSerial_;
    activeCancellation_ = std::make_shared<std::stop_source>();
    const auto cancellation = activeCancellation_;
    const auto state = workerState_;
    const QPointer<PreviewPresentationController> controller(this);
    QMetaObject::invokeMethod(dispatcher_, [
        state,
        cancellation,
        controller,
        serial,
        epoch = surfaceEpoch_,
        sourceSerial,
        playbackGeneration
    ] {
        OperationResult result;
        result.kind = OperationKind::tick;
        result.serial = serial;
        result.surfaceEpoch = epoch;
        result.sourceSerial = sourceSerial;
        result.playbackGeneration = playbackGeneration;
        try {
            result.receipt = state->session
                ? state->session->tick(
                    playbackGeneration,
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

void PreviewPresentationController::scheduleSeek(PendingSeek request) {
    operationActive_ = true;
    activeOperationKind_ = OperationKind::seek;
    const auto serial = ++operationSerial_;
    activeCancellation_ = std::make_shared<std::stop_source>();
    const auto cancellation = activeCancellation_;
    const auto state = workerState_;
    const QPointer<PreviewPresentationController> controller(this);
    QMetaObject::invokeMethod(dispatcher_, [
        state,
        cancellation,
        controller,
        serial,
        epoch = surfaceEpoch_,
        request
    ] {
        OperationResult result;
        result.kind = OperationKind::seek;
        result.serial = serial;
        result.surfaceEpoch = epoch;
        result.sourceSerial = request.sourceSerial;
        result.playbackGeneration = request.playbackGeneration;
        try {
            result.receipt = state->session
                ? state->session->seek(
                    request.playbackGeneration,
                    request.targetTimelineFrame,
                    request.mode,
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

void PreviewPresentationController::scheduleCancel(
    std::uint64_t sourceSerial,
    std::uint64_t playbackGeneration
) {
    operationActive_ = true;
    activeOperationKind_ = OperationKind::cancel;
    const auto serial = ++operationSerial_;
    const auto state = workerState_;
    const QPointer<PreviewPresentationController> controller(this);
    QMetaObject::invokeMethod(dispatcher_, [
        state,
        controller,
        serial,
        epoch = surfaceEpoch_,
        sourceSerial,
        playbackGeneration
    ] {
        OperationResult result;
        result.kind = OperationKind::cancel;
        result.serial = serial;
        result.surfaceEpoch = epoch;
        result.sourceSerial = sourceSerial;
        result.playbackGeneration = playbackGeneration;
        try {
            result.receipt = state->session
                ? state->session->cancel(playbackGeneration)
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

void PreviewPresentationController::scheduleTransport(
    OperationKind kind,
    std::uint64_t sourceSerial,
    std::uint64_t playbackGeneration
) {
    if (kind != OperationKind::pause && kind != OperationKind::resume) return;
    operationActive_ = true;
    activeOperationKind_ = kind;
    const auto serial = ++operationSerial_;
    const auto state = workerState_;
    const QPointer<PreviewPresentationController> controller(this);
    QMetaObject::invokeMethod(dispatcher_, [
        state,
        controller,
        serial,
        epoch = surfaceEpoch_,
        sourceSerial,
        playbackGeneration,
        kind
    ] {
        OperationResult result;
        result.kind = kind;
        result.serial = serial;
        result.surfaceEpoch = epoch;
        result.sourceSerial = sourceSerial;
        result.playbackGeneration = playbackGeneration;
        try {
            result.receipt = state->session
                ? kind == OperationKind::pause
                    ? state->session->pause(playbackGeneration)
                    : state->session->resume(playbackGeneration)
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

void PreviewPresentationController::scheduleNextTick(
    std::uint64_t sourceSerial,
    std::uint64_t playbackGeneration,
    std::int64_t framesPerSecond
) {
    if (
        shutdownRequested_
        || tickScheduled_
        || pauseRequested_
        || state_ != QStringLiteral("playing")
        || framesPerSecond <= 0
        || sourceSerial != sourceSerial_
    ) {
        return;
    }
    const auto interval = std::clamp<std::int64_t>(
        1000 / framesPerSecond,
        1,
        1000
    );
    tickScheduled_ = true;
    QTimer::singleShot(static_cast<int>(interval), this, [
        this,
        sourceSerial,
        playbackGeneration
    ] {
        if (
            shutdownRequested_
            || sourceSerial != sourceSerial_
            || playbackGeneration != playbackGeneration_
            || activePreviewSerial_ != sourceSerial
            || pauseRequested_
            || state_ != QStringLiteral("playing")
        ) {
            tickScheduled_ = false;
            return;
        }
        tickScheduled_ = false;
        tickPending_ = true;
        servicePendingWork();
    });
}

void PreviewPresentationController::scheduleClose() {
    operationActive_ = true;
    activeOperationKind_ = OperationKind::close;
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
    activeOperationKind_.reset();
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
    if (result.kind == OperationKind::play || result.kind == OperationKind::tick
        || result.kind == OperationKind::seek
        || result.kind == OperationKind::pause
        || result.kind == OperationKind::resume) {
        latestPlaybackReceipt_ = result.receipt;
        const bool currentSource = result.sourceSerial == sourceSerial_
            && (result.kind == OperationKind::play
                || result.sourceSerial == activePreviewSerial_);
        if (currentSource && result.receipt.hasTargetTimelineFrame) {
            setCurrentFrame(result.receipt.targetTimelineFrame);
        }
    }
    const auto outcome = result.receipt.outcome;
    if (result.kind == OperationKind::attach || result.kind == OperationKind::resize) {
        const bool usable = outcome == preview::PreviewPresentationOutcome::changed
            || outcome == preview::PreviewPresentationOutcome::noOp
            || outcome == preview::PreviewPresentationOutcome::occluded;
        setReady(usable);
        if (shutdownRequested_ || shutdownComplete_) return;
        if (!usable || !desiredPreview_) {
            setState(outcomeName(outcome));
            if (shutdownRequested_ || shutdownComplete_) return;
            setErrorCode(errorName(result.receipt));
        }
        if (!usable) return;
        servicePendingWork();
        return;
    }
    if (result.kind == OperationKind::cancel) {
        tickScheduled_ = false;
        tickPending_ = false;
        activePreviewSerial_ = 0;
        if (
            outcome == preview::PreviewPresentationOutcome::stale
            && result.receipt.generation != 0
            && result.receipt.generation != result.playbackGeneration
        ) {
            playbackGeneration_ = result.receipt.generation;
        } else {
            playbackGeneration_ = 0;
        }
        const auto error = errorName(result.receipt);
        if (!error.isEmpty()) {
            setState(presentationStateName(result.receipt));
            if (shutdownRequested_ || shutdownComplete_) return;
            setErrorCode(error);
        }
        servicePendingWork();
        return;
    }
    if (result.kind == OperationKind::play) {
        if (result.receipt.generation != 0) {
            playbackGeneration_ = result.receipt.generation;
        }
        activePreviewSerial_ = result.sourceSerial;
        if (result.sourceSerial != sourceSerial_) {
            servicePendingWork();
            return;
        }
        setState(presentationStateName(result.receipt));
        if (shutdownRequested_ || shutdownComplete_) return;
        setErrorCode(errorName(result.receipt));
        if (shutdownRequested_ || shutdownComplete_) return;
        const bool started = (
            outcome == preview::PreviewPresentationOutcome::changed
            || outcome == preview::PreviewPresentationOutcome::noOp
        ) && playbackGeneration_ != 0;
        suppressedPreviewSerial_ = started ? 0 : result.sourceSerial;
        if (
            started
            && result.receipt.state == preview::PreviewPresentationState::playing
            && desiredPreview_
            && desiredPreview_->preview.candidate
        ) {
            scheduleNextTick(
                result.sourceSerial,
                playbackGeneration_,
                desiredPreview_->preview.candidate->renderTimeline.framesPerSecond
            );
        }
        servicePendingWork();
        return;
    }
    if (result.kind == OperationKind::seek) {
        tickScheduled_ = false;
        tickPending_ = false;
        if (result.receipt.generation != 0) {
            playbackGeneration_ = result.receipt.generation;
            if (pendingSeek_
                && pendingSeek_->sourceSerial == result.sourceSerial) {
                pendingSeek_->playbackGeneration = playbackGeneration_;
            }
        }
        if (result.sourceSerial != sourceSerial_
            || result.sourceSerial != activePreviewSerial_) {
            servicePendingWork();
            return;
        }
        setState(presentationStateName(result.receipt));
        if (shutdownRequested_ || shutdownComplete_) return;
        const bool changedGenerationOutcome =
            outcome == preview::PreviewPresentationOutcome::changed
            || outcome == preview::PreviewPresentationOutcome::presented
            || outcome == preview::PreviewPresentationOutcome::occluded;
        const bool stale = outcome == preview::PreviewPresentationOutcome::stale
            || (changedGenerationOutcome
                && result.receipt.generation == result.playbackGeneration);
        setErrorCode(stale
            ? QStringLiteral("previewStale")
            : errorName(result.receipt));
        if (shutdownRequested_ || shutdownComplete_) return;
        if (result.receipt.state == preview::PreviewPresentationState::playing
            && !pendingSeek_
            && desiredPreview_ && desiredPreview_->preview.candidate) {
            scheduleNextTick(
                result.sourceSerial,
                playbackGeneration_,
                desiredPreview_->preview.candidate->renderTimeline.framesPerSecond
            );
        }
        servicePendingWork();
        return;
    }
    if (result.kind == OperationKind::pause
        || result.kind == OperationKind::resume) {
        if (result.kind == OperationKind::pause) pauseRequested_ = false;
        else resumeRequested_ = false;
        if (result.sourceSerial != sourceSerial_
            || result.sourceSerial != activePreviewSerial_
            || result.playbackGeneration != playbackGeneration_) {
            servicePendingWork();
            return;
        }
        setState(presentationStateName(result.receipt));
        if (shutdownRequested_ || shutdownComplete_) return;
        const bool stale = outcome == preview::PreviewPresentationOutcome::stale
            || (result.receipt.generation != 0
                && result.receipt.generation != result.playbackGeneration);
        setErrorCode(stale
            ? QStringLiteral("previewStale")
            : errorName(result.receipt));
        if (shutdownRequested_ || shutdownComplete_) return;
        if (result.receipt.state == preview::PreviewPresentationState::playing
            && desiredPreview_ && desiredPreview_->preview.candidate) {
            scheduleNextTick(
                result.sourceSerial,
                playbackGeneration_,
                desiredPreview_->preview.candidate->renderTimeline.framesPerSecond
            );
        }
        servicePendingWork();
        return;
    }
    if (result.kind == OperationKind::tick) {
        if (
            result.sourceSerial != sourceSerial_
            || result.playbackGeneration != playbackGeneration_
            || result.sourceSerial != activePreviewSerial_
        ) {
            servicePendingWork();
            return;
        }
        if (
            outcome == preview::PreviewPresentationOutcome::stale
            || (result.receipt.generation != 0
                && result.receipt.generation != result.playbackGeneration)
        ) {
            tickScheduled_ = false;
            tickPending_ = false;
            suppressedPreviewSerial_ = result.sourceSerial;
            activePreviewSerial_ = 0;
            if (result.receipt.generation != 0) {
                playbackGeneration_ = result.receipt.generation;
            }
            setState(QStringLiteral("failed"));
            if (shutdownRequested_ || shutdownComplete_) return;
            setErrorCode(QStringLiteral("previewStale"));
            if (shutdownRequested_ || shutdownComplete_) return;
            servicePendingWork();
            return;
        }
        if (outcome == preview::PreviewPresentationOutcome::cancelled
            && pendingSeek_) {
            servicePendingWork();
            return;
        }
        setState(presentationStateName(result.receipt));
        if (shutdownRequested_ || shutdownComplete_) return;
        setErrorCode(errorName(result.receipt));
        if (shutdownRequested_ || shutdownComplete_) return;
        if (
            result.receipt.state == preview::PreviewPresentationState::playing
            && desiredPreview_
            && desiredPreview_->preview.candidate
        ) {
            scheduleNextTick(
                result.sourceSerial,
                playbackGeneration_,
                desiredPreview_->preview.candidate->renderTimeline.framesPerSecond
            );
        }
        servicePendingWork();
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

void PreviewPresentationController::setCurrentFrame(std::int64_t value) {
    if (currentFrame_ == value) return;
    currentFrame_ = value;
    emit currentFrameChanged();
}

void PreviewPresentationController::setPreviewRange(
    std::int64_t minimum,
    std::int64_t maximum
) {
    if (minimumFrame_ == minimum && maximumFrame_ == maximum) return;
    minimumFrame_ = minimum;
    maximumFrame_ = maximum;
    emit previewRangeChanged();
}

}
