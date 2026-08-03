#include "palmier/preview/preview_presentation_session.hpp"

#include "palmier/render/d3d11_warp_renderer.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace palmier::preview {
namespace {

template <typename Action>
class ScopeExit final {
public:
    explicit ScopeExit(Action action) : action_(std::move(action)) {}
    ~ScopeExit() { action_(); }

    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;

private:
    Action action_;
};

bool settingsEqual(
    const PreviewPresentationSettings& lhs,
    const PreviewPresentationSettings& rhs
) noexcept {
    const auto& left = lhs.renderLayer;
    const auto& right = rhs.renderLayer;
    return left.canvasWidth == right.canvasWidth
        && left.canvasHeight == right.canvasHeight
        && left.framesPerSecond == right.framesPerSecond
        && left.timelineId == right.timelineId
        && left.trackId == right.trackId
        && left.clipId == right.clipId
        && left.mediaId == right.mediaId
        && left.timelineStartFrame == right.timelineStartFrame
        && left.durationFrames == right.durationFrames
        && left.sourceStartFrame == right.sourceStartFrame
        && left.transform.centerX == right.transform.centerX
        && left.transform.centerY == right.transform.centerY
        && left.transform.width == right.transform.width
        && left.transform.height == right.transform.height
        && left.transform.rotationDegrees == right.transform.rotationDegrees
        && left.opacity == right.opacity
        && left.exposureEv == right.exposureEv;
}

PreviewPresentationState stateFor(media::HeadlessAvPlaybackState state) noexcept {
    switch (state) {
    case media::HeadlessAvPlaybackState::idle:
        return PreviewPresentationState::idle;
    case media::HeadlessAvPlaybackState::playing:
        return PreviewPresentationState::playing;
    case media::HeadlessAvPlaybackState::paused:
        return PreviewPresentationState::paused;
    case media::HeadlessAvPlaybackState::completed:
        return PreviewPresentationState::completed;
    case media::HeadlessAvPlaybackState::cancelled:
        return PreviewPresentationState::cancelled;
    case media::HeadlessAvPlaybackState::invalidated:
        return PreviewPresentationState::invalidated;
    case media::HeadlessAvPlaybackState::failed:
        return PreviewPresentationState::failed;
    case media::HeadlessAvPlaybackState::closed:
        return PreviewPresentationState::closed;
    }
    return PreviewPresentationState::failed;
}

PreviewPresentationOutcome outcomeFor(
    media::HeadlessAvPlaybackOutcome outcome
) noexcept {
    switch (outcome) {
    case media::HeadlessAvPlaybackOutcome::changed:
        return PreviewPresentationOutcome::changed;
    case media::HeadlessAvPlaybackOutcome::noOp:
        return PreviewPresentationOutcome::noOp;
    case media::HeadlessAvPlaybackOutcome::stale:
        return PreviewPresentationOutcome::stale;
    case media::HeadlessAvPlaybackOutcome::cancelled:
        return PreviewPresentationOutcome::cancelled;
    case media::HeadlessAvPlaybackOutcome::refused:
        return PreviewPresentationOutcome::refused;
    case media::HeadlessAvPlaybackOutcome::failed:
        return PreviewPresentationOutcome::failed;
    case media::HeadlessAvPlaybackOutcome::invalidated:
        return PreviewPresentationOutcome::invalidated;
    }
    return PreviewPresentationOutcome::failed;
}

PreviewPresentationOutcome outcomeFor(
    render::D3d11PreviewSurfaceOutcome outcome
) noexcept {
    switch (outcome) {
    case render::D3d11PreviewSurfaceOutcome::presented:
        return PreviewPresentationOutcome::presented;
    case render::D3d11PreviewSurfaceOutcome::cleared:
        return PreviewPresentationOutcome::changed;
    case render::D3d11PreviewSurfaceOutcome::noOp:
        return PreviewPresentationOutcome::noOp;
    case render::D3d11PreviewSurfaceOutcome::cancelled:
        return PreviewPresentationOutcome::cancelled;
    case render::D3d11PreviewSurfaceOutcome::refused:
        return PreviewPresentationOutcome::refused;
    case render::D3d11PreviewSurfaceOutcome::occluded:
        return PreviewPresentationOutcome::occluded;
    case render::D3d11PreviewSurfaceOutcome::unavailable:
        return PreviewPresentationOutcome::unavailable;
    case render::D3d11PreviewSurfaceOutcome::invalidated:
        return PreviewPresentationOutcome::invalidated;
    case render::D3d11PreviewSurfaceOutcome::failed:
        return PreviewPresentationOutcome::failed;
    }
    return PreviewPresentationOutcome::failed;
}

class PlaybackAdapter final : public detail::PreviewPlaybackPort {
public:
    explicit PlaybackAdapter(media::HeadlessAvPlaybackLimits limits)
        : playback_(limits) {}

    media::HeadlessAvPlaybackReceipt play(
        const std::filesystem::path& input,
        std::int64_t timelineFrame,
        audio::FrameRate timelineFrameRate,
        media::DecodeFrameStart decodeStart,
        std::stop_token cancellation
    ) override {
        return playback_.play(
            input,
            timelineFrame,
            timelineFrameRate,
            decodeStart,
            cancellation
        );
    }

    media::HeadlessAvPlaybackReceipt tick(
        std::uint64_t expectedGeneration,
        std::stop_token cancellation
    ) override {
        return playback_.tick(expectedGeneration, cancellation);
    }

    media::HeadlessAvPlaybackReceipt seek(
        std::uint64_t expectedGeneration,
        const std::filesystem::path& input,
        std::int64_t timelineFrame,
        audio::FrameRate timelineFrameRate,
        media::DecodeFrameStart start,
        media::HeadlessAvPlaybackSeekMode mode,
        std::stop_token cancellation
    ) override {
        return playback_.seek(
            expectedGeneration,
            input,
            timelineFrame,
            timelineFrameRate,
            start,
            mode,
            cancellation
        );
    }

    media::HeadlessAvPlaybackReceipt cancel(
        std::uint64_t expectedGeneration
    ) override {
        return playback_.cancel(expectedGeneration);
    }

    media::HeadlessAvPlaybackReceipt pause(
        std::uint64_t expectedGeneration
    ) override {
        return playback_.pause(expectedGeneration);
    }

    media::HeadlessAvPlaybackReceipt resume(
        std::uint64_t expectedGeneration
    ) override {
        return playback_.resume(expectedGeneration);
    }

    media::HeadlessAvPlaybackReceipt snapshot() const override {
        return playback_.snapshot();
    }

    media::HeadlessAvPlaybackReceipt close() override { return playback_.close(); }

private:
    media::HeadlessAvPlaybackSession playback_;
};

class RenderAdapter final : public detail::PreviewRenderPort {
public:
    render::RenderedFrame render(
        const media::PresentedVideoFrame& frame,
        std::int64_t targetTimelineFrame,
        const PreviewPresentationSettings& settings,
        std::stop_token cancellation
    ) override {
        const auto plan = project_render::makeRenderPlan(
            settings.renderLayer,
            targetTimelineFrame
        );
        const auto expectedSourceFrame = plan.layers().front().sourceFrame;
        const auto resolver = [&](std::string_view mediaId, std::int64_t sourceFrame)
            -> const render::SourceFrame* {
            if (
                mediaId == settings.renderLayer.mediaId
                && sourceFrame == expectedSourceFrame
            ) {
                return &frame.source;
            }
            return nullptr;
        };
        return render::renderPreviewFrame(plan, resolver, renderer_, cancellation);
    }

private:
    render::D3d11WarpRenderer renderer_;
};

class SurfaceAdapter final : public detail::PreviewSurfacePort {
public:
    SurfaceAdapter(
        HWND window,
        render::D3d11PreviewDriver driver,
        render::D3d11PreviewSurfaceLimits limits
    ) : surface_(window, driver, limits) {}

    render::D3d11PreviewSurfaceReceipt resize(
        std::uint32_t width,
        std::uint32_t height,
        std::stop_token cancellation
    ) override {
        return surface_.resize(width, height, cancellation);
    }

    render::D3d11PreviewSurfaceReceipt present(
        const render::RenderedFrame& frame,
        std::stop_token cancellation
    ) override {
        return surface_.present(frame, cancellation);
    }

    render::D3d11PreviewSurfaceReceipt clear(
        std::stop_token cancellation
    ) override {
        return surface_.clear(cancellation);
    }

    render::D3d11PreviewSurfaceReceipt snapshot() const override {
        return surface_.snapshot();
    }

    render::D3d11PreviewSurfaceReceipt close() override { return surface_.close(); }

private:
    render::D3d11PreviewSurface surface_;
};

bool isSurfaceTerminal(render::D3d11PreviewSurfaceOutcome outcome) noexcept {
    return outcome == render::D3d11PreviewSurfaceOutcome::unavailable
        || outcome == render::D3d11PreviewSurfaceOutcome::invalidated
        || outcome == render::D3d11PreviewSurfaceOutcome::failed;
}

}

PreviewPresentationSession::PreviewPresentationSession(
    HWND window,
    render::D3d11PreviewDriver driver,
    PreviewPresentationLimits limits
) : PreviewPresentationSession(
    std::make_unique<PlaybackAdapter>(limits.playback),
    std::make_unique<RenderAdapter>(),
    std::make_unique<SurfaceAdapter>(window, driver, limits.surface)
) {}

PreviewPresentationSession::PreviewPresentationSession(
    std::unique_ptr<detail::PreviewPlaybackPort> playback,
    std::unique_ptr<detail::PreviewRenderPort> renderer,
    std::unique_ptr<detail::PreviewSurfacePort> surface
) : playback_(std::move(playback)),
    renderer_(std::move(renderer)),
    surface_(std::move(surface)) {
    if (playback_ == nullptr || renderer_ == nullptr || surface_ == nullptr) {
        throw std::invalid_argument("preview presentation ports must not be null");
    }
}

PreviewPresentationSession::~PreviewPresentationSession() { close(); }

PreviewPresentationReceipt PreviewPresentationSession::play(
    const std::filesystem::path& input,
    std::int64_t timelineFrame,
    audio::FrameRate timelineFrameRate,
    PreviewPresentationSettings settings,
    std::stop_token cancellation
) {
    std::unique_lock operationLock(operationMutex_);
    {
        std::scoped_lock lifecycleLock(lifecycleMutex_);
        if (closeRequested_) {
            return refused(
                PreviewPresentationStage::startPlayback,
                PreviewPresentationFailureCode::invalidRequest,
                E_ILLEGAL_METHOD_CALL
            );
        }
    }
    if (terminalSurfaceReceipt_.has_value()) {
        auto value = receipt(
            outcomeFor(terminalSurfaceReceipt_->outcome),
            PreviewPresentationStage::startPlayback
        );
        value.failure = PreviewPresentationFailureCode::surfaceFailure;
        value.hresult = terminalSurfaceReceipt_->hresult;
        return value;
    }
    auto operation = beginOperation(generation_);
    if (operation == nullptr) {
        return refused(
            PreviewPresentationStage::startPlayback,
            PreviewPresentationFailureCode::invalidRequest,
            HRESULT_FROM_WIN32(ERROR_INVALID_STATE)
        );
    }
    ScopeExit cleanup([&] { finishOperation(operation); });
    std::stop_callback cancellationCallback(
        cancellation,
        [&operation] { operation->cancellation.request_stop(); }
    );

    if (operation->cancellation.stop_requested()) {
        return refused(
            PreviewPresentationStage::validate,
            PreviewPresentationFailureCode::none,
            HRESULT_FROM_WIN32(ERROR_CANCELLED)
        );
    }
    if (settings.renderLayer.framesPerSecond <= 0
        || settings.renderLayer.sourceStartFrame < 0
        || timelineFrameRate.denominator != 1
        || timelineFrameRate.numerator
            != static_cast<std::uint32_t>(settings.renderLayer.framesPerSecond)) {
        return refused(
            PreviewPresentationStage::validate,
            PreviewPresentationFailureCode::invalidRequest,
            E_INVALIDARG
        );
    }
    try {
        static_cast<void>(project_render::makeRenderPlan(
            settings.renderLayer,
            timelineFrame
        ));
    } catch (const std::exception&) {
        return refused(
            PreviewPresentationStage::validate,
            PreviewPresentationFailureCode::invalidRequest,
            E_INVALIDARG
        );
    }

    const auto playback = playback_->play(
        input,
        timelineFrame,
        timelineFrameRate,
        media::DecodeFrameStart{
            settings.renderLayer.sourceStartFrame,
            {settings.renderLayer.framesPerSecond, 1},
        },
        operation->cancellation.get_token()
    );
    const auto playbackOutcome = outcomeFor(playback.outcome);
    if (playbackOutcome != PreviewPresentationOutcome::changed
        && playbackOutcome != PreviewPresentationOutcome::noOp) {
        if (playback.generation != generation_) {
            generation_ = playback.generation;
            settings_.reset();
            input_.clear();
            timelineFrameRate_ = {};
            clearFrameState();
        }
        state_ = stateFor(playback.state);
        auto value = receipt(playbackOutcome, PreviewPresentationStage::startPlayback);
        value.failure = playback.failure == media::HeadlessAvPlaybackFailureCode::none
            ? PreviewPresentationFailureCode::none
            : PreviewPresentationFailureCode::playbackFailure;
        value.hresult = playback.hresult;
        value.mediaFailureCode = playback.mediaFailureCode;
        value.audioFailure = playback.audioFailure;
        return value;
    }

    const bool generationChanged = playback.generation != generation_;
    const bool settingsChanged = !settings_.has_value()
        || !settingsEqual(*settings_, settings);
    const bool sourceMappingChanged = settings_.has_value()
        && (settings_->renderLayer.framesPerSecond
                != settings.renderLayer.framesPerSecond
            || settings_->renderLayer.timelineStartFrame
                != settings.renderLayer.timelineStartFrame
            || settings_->renderLayer.sourceStartFrame
                != settings.renderLayer.sourceStartFrame);
    if (!generationChanged && sourceMappingChanged) {
        return refused(
            PreviewPresentationStage::validate,
            PreviewPresentationFailureCode::invalidRequest,
            E_INVALIDARG
        );
    }
    generation_ = playback.generation;
    state_ = stateFor(playback.state);
    if (generationChanged) {
        clearFrameState();
    }
    settings_ = std::move(settings);
    input_ = input;
    timelineFrameRate_ = timelineFrameRate;
    if (generationChanged || settingsChanged) {
        pendingRenderedFrame_.reset();
        pendingRenderedSourceTimestamp_.reset();
        pendingRenderedTargetFrame_.reset();
        presentationDirty_ = cachedFrame_.has_value();
    }
    return receipt(
        generationChanged || settingsChanged
            ? PreviewPresentationOutcome::changed
            : PreviewPresentationOutcome::noOp,
        PreviewPresentationStage::startPlayback
    );
}

PreviewPresentationReceipt PreviewPresentationSession::tick(
    std::uint64_t expectedGeneration,
    std::stop_token cancellation
) {
    std::unique_lock operationLock(operationMutex_);
    {
        std::scoped_lock lifecycleLock(lifecycleMutex_);
        if (closeRequested_) {
            return refused(
                PreviewPresentationStage::tickPlayback,
                PreviewPresentationFailureCode::invalidRequest,
                E_ILLEGAL_METHOD_CALL
            );
        }
    }
    if (expectedGeneration == 0 || expectedGeneration != generation_) {
        return receipt(PreviewPresentationOutcome::stale, PreviewPresentationStage::tickPlayback);
    }
    if (state_ != PreviewPresentationState::playing
        && !(state_ == PreviewPresentationState::completed
            && presentationDirty_ && cachedFrame_.has_value())) {
        return receipt(PreviewPresentationOutcome::noOp, PreviewPresentationStage::tickPlayback);
    }
    auto operation = beginOperation(expectedGeneration);
    if (operation == nullptr) {
        return refused(
            PreviewPresentationStage::tickPlayback,
            PreviewPresentationFailureCode::invalidRequest,
            HRESULT_FROM_WIN32(ERROR_INVALID_STATE)
        );
    }
    ScopeExit cleanup([&] { finishOperation(operation); });
    std::stop_callback cancellationCallback(
        cancellation,
        [&operation] { operation->cancellation.request_stop(); }
    );

    if (operation->cancellation.stop_requested()) {
        auto value = receipt(
            PreviewPresentationOutcome::cancelled,
            PreviewPresentationStage::tickPlayback
        );
        value.hresult = HRESULT_FROM_WIN32(ERROR_CANCELLED);
        return value;
    }

    auto playback = playback_->tick(
        expectedGeneration,
        operation->cancellation.get_token()
    );
    const auto playbackOutcome = outcomeFor(playback.outcome);
    state_ = stateFor(playback.state);
    bool reachedClipBoundary = false;
    if (
        (playbackOutcome == PreviewPresentationOutcome::changed
            || playbackOutcome == PreviewPresentationOutcome::noOp)
        && playback.hasTargetTimelineFrame
        && settings_.has_value()
    ) {
        const auto& layer = settings_->renderLayer;
        if (
            playback.targetTimelineFrame >= layer.timelineStartFrame
            && playback.targetTimelineFrame - layer.timelineStartFrame
                >= layer.durationFrames
        ) {
            const auto stopped = playback_->cancel(expectedGeneration);
            const auto stopOutcome = outcomeFor(stopped.outcome);
            if (
                stopOutcome != PreviewPresentationOutcome::changed
                && stopOutcome != PreviewPresentationOutcome::noOp
                && stopOutcome != PreviewPresentationOutcome::cancelled
            ) {
                state_ = stateFor(stopped.state);
                auto value = receipt(stopOutcome, PreviewPresentationStage::tickPlayback);
                value.failure = stopped.failure == media::HeadlessAvPlaybackFailureCode::none
                    ? PreviewPresentationFailureCode::none
                    : PreviewPresentationFailureCode::playbackFailure;
                value.hresult = stopped.hresult;
                value.mediaFailureCode = stopped.mediaFailureCode;
                value.audioFailure = stopped.audioFailure;
                return value;
            }
            state_ = PreviewPresentationState::completed;
            playback.frame.reset();
            playback.hasTargetTimelineFrame = false;
            reachedClipBoundary = true;
        }
    }
    if (playback.frame.has_value()) {
        cachedFrame_ = std::move(playback.frame);
        presentationDirty_ = true;
        pendingRenderedFrame_.reset();
        pendingRenderedSourceTimestamp_.reset();
        pendingRenderedTargetFrame_.reset();
    }
    if (playback.hasTargetTimelineFrame) {
        if (!targetTimelineFrame_.has_value()
            || *targetTimelineFrame_ != playback.targetTimelineFrame) {
            presentationDirty_ = true;
        }
        targetTimelineFrame_ = playback.targetTimelineFrame;
    }

    if (playbackOutcome == PreviewPresentationOutcome::cancelled) {
        clearFrameState();
    }
    if (playbackOutcome == PreviewPresentationOutcome::stale
        || playbackOutcome == PreviewPresentationOutcome::cancelled
        || playbackOutcome == PreviewPresentationOutcome::refused
        || playbackOutcome == PreviewPresentationOutcome::failed
        || playbackOutcome == PreviewPresentationOutcome::invalidated) {
        if (playbackOutcome == PreviewPresentationOutcome::failed
            || playbackOutcome == PreviewPresentationOutcome::invalidated) {
            clearFrameState();
        }
        auto value = receipt(playbackOutcome, PreviewPresentationStage::tickPlayback);
        value.failure = playback.failure == media::HeadlessAvPlaybackFailureCode::none
            ? PreviewPresentationFailureCode::none
            : PreviewPresentationFailureCode::playbackFailure;
        value.hresult = playback.hresult;
        value.mediaFailureCode = playback.mediaFailureCode;
        value.audioFailure = playback.audioFailure;
        return value;
    }
    auto presented = presentCurrentFrame(
        playbackOutcome,
        operation->cancellation.get_token()
    );
    presented.reachedClipBoundary = reachedClipBoundary;
    return presented;
}

PreviewPresentationReceipt PreviewPresentationSession::seek(
    std::uint64_t expectedGeneration,
    std::int64_t targetTimelineFrame,
    media::HeadlessAvPlaybackSeekMode mode,
    std::stop_token cancellation
) {
    std::unique_lock operationLock(operationMutex_);
    if (!settings_.has_value()) {
        return refused(
            PreviewPresentationStage::seekPlayback,
            PreviewPresentationFailureCode::invalidRequest,
            E_ILLEGAL_METHOD_CALL
        );
    }
    return seekSourceLocked(
        expectedGeneration,
        input_,
        targetTimelineFrame,
        timelineFrameRate_,
        *settings_,
        mode,
        cancellation
    );
}

PreviewPresentationReceipt PreviewPresentationSession::seekSource(
    std::uint64_t expectedGeneration,
    const std::filesystem::path& input,
    std::int64_t targetTimelineFrame,
    audio::FrameRate timelineFrameRate,
    PreviewPresentationSettings settings,
    media::HeadlessAvPlaybackSeekMode mode,
    std::stop_token cancellation
) {
    std::unique_lock operationLock(operationMutex_);
    return seekSourceLocked(
        expectedGeneration,
        input,
        targetTimelineFrame,
        timelineFrameRate,
        std::move(settings),
        mode,
        cancellation
    );
}

PreviewPresentationReceipt PreviewPresentationSession::seekSourceLocked(
    std::uint64_t expectedGeneration,
    const std::filesystem::path& input,
    std::int64_t targetTimelineFrame,
    audio::FrameRate timelineFrameRate,
    PreviewPresentationSettings settings,
    media::HeadlessAvPlaybackSeekMode mode,
    std::stop_token cancellation
) {
    {
        std::scoped_lock lifecycleLock(lifecycleMutex_);
        if (closeRequested_) {
            return refused(
                PreviewPresentationStage::seekPlayback,
                PreviewPresentationFailureCode::invalidRequest,
                E_ILLEGAL_METHOD_CALL
            );
        }
    }
    if (expectedGeneration == 0 || expectedGeneration != generation_) {
        return receipt(
            PreviewPresentationOutcome::stale,
            PreviewPresentationStage::seekPlayback
        );
    }
    if (mode != media::HeadlessAvPlaybackSeekMode::paused
        && mode != media::HeadlessAvPlaybackSeekMode::playing) {
        return refused(
            PreviewPresentationStage::seekPlayback,
            PreviewPresentationFailureCode::invalidRequest,
            E_INVALIDARG
        );
    }
    if (state_ != PreviewPresentationState::playing
        && state_ != PreviewPresentationState::paused
        && state_ != PreviewPresentationState::completed) {
        return refused(
            PreviewPresentationStage::seekPlayback,
            PreviewPresentationFailureCode::invalidRequest,
            E_ILLEGAL_METHOD_CALL
        );
    }
    if (input.empty()
        || settings.renderLayer.framesPerSecond <= 0
        || settings.renderLayer.sourceStartFrame < 0
        || timelineFrameRate.denominator != 1
        || timelineFrameRate.numerator
            != static_cast<std::uint32_t>(settings.renderLayer.framesPerSecond)) {
        return refused(
            PreviewPresentationStage::seekPlayback,
            PreviewPresentationFailureCode::invalidRequest,
            E_INVALIDARG
        );
    }
    const auto& layer = settings.renderLayer;
    if (targetTimelineFrame < layer.timelineStartFrame
        || layer.durationFrames <= 0) {
        return refused(
            PreviewPresentationStage::seekPlayback,
            PreviewPresentationFailureCode::invalidRequest,
            E_INVALIDARG
        );
    }
    const auto offset = targetTimelineFrame - layer.timelineStartFrame;
    if (offset >= layer.durationFrames
        || layer.sourceStartFrame
            > (std::numeric_limits<std::int64_t>::max)() - offset) {
        return refused(
            PreviewPresentationStage::seekPlayback,
            PreviewPresentationFailureCode::invalidRequest,
            E_INVALIDARG
        );
    }
    auto operation = beginOperation(expectedGeneration);
    if (operation == nullptr) {
        return refused(
            PreviewPresentationStage::seekPlayback,
            PreviewPresentationFailureCode::invalidRequest,
            HRESULT_FROM_WIN32(ERROR_INVALID_STATE)
        );
    }
    ScopeExit cleanup([&] { finishOperation(operation); });
    std::stop_callback cancellationCallback(
        cancellation,
        [&operation] { operation->cancellation.request_stop(); }
    );
    if (operation->cancellation.stop_requested()) {
        auto value = receipt(
            PreviewPresentationOutcome::cancelled,
            PreviewPresentationStage::seekPlayback
        );
        value.hresult = HRESULT_FROM_WIN32(ERROR_CANCELLED);
        return value;
    }

    auto playback = playback_->seek(
        expectedGeneration,
        input,
        targetTimelineFrame,
        timelineFrameRate,
        {
            layer.sourceStartFrame + offset,
            {layer.framesPerSecond, 1},
        },
        mode,
        operation->cancellation.get_token()
    );
    const auto playbackOutcome = outcomeFor(playback.outcome);
    if (playbackOutcome == PreviewPresentationOutcome::noOp) {
        const bool settingsChanged = !settings_.has_value()
            || !settingsEqual(*settings_, settings);
        settings_ = std::move(settings);
        input_ = input;
        timelineFrameRate_ = timelineFrameRate;
        if (settingsChanged) {
            pendingRenderedFrame_.reset();
            pendingRenderedSourceTimestamp_.reset();
            pendingRenderedTargetFrame_.reset();
            presentationDirty_ = cachedFrame_.has_value();
        }
        return presentCurrentFrame(
            PreviewPresentationOutcome::noOp,
            operation->cancellation.get_token()
        );
    }
    if (playbackOutcome != PreviewPresentationOutcome::changed
        || playback.generation == expectedGeneration
        || !playback.frame.has_value()
        || !playback.hasTargetTimelineFrame
        || playback.targetTimelineFrame != targetTimelineFrame) {
        if (playback.generation != generation_) {
            generation_ = playback.generation;
            state_ = stateFor(playback.state);
            settings_.reset();
            input_.clear();
            timelineFrameRate_ = {};
            clearFrameState();
        }
        auto value = receipt(
            playbackOutcome == PreviewPresentationOutcome::changed
                ? PreviewPresentationOutcome::failed
                : playbackOutcome,
            PreviewPresentationStage::seekPlayback
        );
        value.failure = playbackOutcome == PreviewPresentationOutcome::changed
            || playback.failure
                != media::HeadlessAvPlaybackFailureCode::none
            ? PreviewPresentationFailureCode::playbackFailure
            : PreviewPresentationFailureCode::none;
        value.hresult = playbackOutcome == PreviewPresentationOutcome::changed
            ? E_UNEXPECTED
            : playback.hresult;
        value.mediaFailureCode = playback.mediaFailureCode;
        value.audioFailure = playback.audioFailure;
        return value;
    }

    generation_ = playback.generation;
    state_ = stateFor(playback.state);
    clearFrameState();
    settings_ = std::move(settings);
    input_ = input;
    timelineFrameRate_ = timelineFrameRate;
    cachedFrame_ = std::move(playback.frame);
    targetTimelineFrame_ = targetTimelineFrame;
    presentationDirty_ = true;
    return presentCurrentFrame(
        PreviewPresentationOutcome::changed,
        operation->cancellation.get_token()
    );
}

PreviewPresentationReceipt PreviewPresentationSession::presentCurrentFrame(
    PreviewPresentationOutcome playbackOutcome,
    std::stop_token cancellation
) {
    if (!presentationDirty_ || !cachedFrame_.has_value()
        || !targetTimelineFrame_.has_value() || !settings_.has_value()) {
        return receipt(playbackOutcome, PreviewPresentationStage::tickPlayback);
    }
    if (cancellation.stop_requested()) {
        auto value = receipt(
            PreviewPresentationOutcome::cancelled,
            PreviewPresentationStage::render
        );
        value.hresult = HRESULT_FROM_WIN32(ERROR_CANCELLED);
        return value;
    }

    const auto sourceTimestamp = cachedFrame_->presentationTimestamp;
    const auto targetFrame = *targetTimelineFrame_;
    if (!pendingRenderedFrame_.has_value()
        || pendingRenderedSourceTimestamp_ != sourceTimestamp
        || pendingRenderedTargetFrame_ != targetFrame) {
        try {
            pendingRenderedFrame_ = renderer_->render(
                *cachedFrame_,
                targetFrame,
                *settings_,
                cancellation
            );
            pendingRenderedSourceTimestamp_ = sourceTimestamp;
            pendingRenderedTargetFrame_ = targetFrame;
            ++renderSerial_;
        } catch (const render::RenderError& error) {
            if (error.code == "cancelled" && cancellation.stop_requested()) {
                auto value = receipt(
                    PreviewPresentationOutcome::cancelled,
                    PreviewPresentationStage::render
                );
                value.hresult = HRESULT_FROM_WIN32(ERROR_CANCELLED);
                return value;
            }
            state_ = PreviewPresentationState::failed;
            static_cast<void>(playback_->cancel(generation_));
            clearFrameState();
            auto value = receipt(
                PreviewPresentationOutcome::failed,
                PreviewPresentationStage::render
            );
            value.failure = PreviewPresentationFailureCode::renderFailure;
            value.hresult = E_FAIL;
            return value;
        } catch (const std::exception&) {
            state_ = PreviewPresentationState::failed;
            static_cast<void>(playback_->cancel(generation_));
            clearFrameState();
            auto value = receipt(
                PreviewPresentationOutcome::failed,
                PreviewPresentationStage::render
            );
            value.failure = PreviewPresentationFailureCode::renderFailure;
            value.hresult = E_FAIL;
            return value;
        }
    }

    if (cancellation.stop_requested()) {
        auto value = receipt(
            PreviewPresentationOutcome::cancelled,
            PreviewPresentationStage::present
        );
        value.hresult = HRESULT_FROM_WIN32(ERROR_CANCELLED);
        return value;
    }
    const auto surface = surface_->present(
        *pendingRenderedFrame_,
        cancellation
    );
    presentSerial_ = surface.presentSerial;
    if (surface.outcome == render::D3d11PreviewSurfaceOutcome::presented) {
        presentationDirty_ = false;
        pendingRenderedFrame_.reset();
        pendingRenderedSourceTimestamp_.reset();
        pendingRenderedTargetFrame_.reset();
    }
    const bool terminalSurface = isSurfaceTerminal(surface.outcome);
    if (terminalSurface) {
        terminalSurfaceReceipt_ = surface;
        state_ = surface.outcome == render::D3d11PreviewSurfaceOutcome::failed
            ? PreviewPresentationState::failed
            : PreviewPresentationState::invalidated;
        static_cast<void>(playback_->cancel(generation_));
        clearFrameState();
    }
    auto value = receipt(outcomeFor(surface.outcome), PreviewPresentationStage::present);
    value.hresult = surface.hresult;
    if (terminalSurface) {
        value.failure = PreviewPresentationFailureCode::surfaceFailure;
    }
    return value;
}

PreviewPresentationReceipt PreviewPresentationSession::pause(
    std::uint64_t expectedGeneration
) {
    std::unique_lock operationLock(operationMutex_);
    {
        std::scoped_lock lifecycleLock(lifecycleMutex_);
        if (closeRequested_) {
            return refused(
                PreviewPresentationStage::pausePlayback,
                PreviewPresentationFailureCode::invalidRequest,
                E_ILLEGAL_METHOD_CALL
            );
        }
    }
    if (expectedGeneration == 0 || expectedGeneration != generation_) {
        return receipt(
            PreviewPresentationOutcome::stale,
            PreviewPresentationStage::pausePlayback
        );
    }
    if (state_ == PreviewPresentationState::paused) {
        return receipt(
            PreviewPresentationOutcome::noOp,
            PreviewPresentationStage::pausePlayback
        );
    }
    if (state_ != PreviewPresentationState::playing) {
        return refused(
            PreviewPresentationStage::pausePlayback,
            PreviewPresentationFailureCode::invalidRequest,
            E_ILLEGAL_METHOD_CALL
        );
    }
    const auto playback = playback_->pause(expectedGeneration);
    const auto playbackOutcome = outcomeFor(playback.outcome);
    state_ = stateFor(playback.state);
    auto value = receipt(playbackOutcome, PreviewPresentationStage::pausePlayback);
    value.hresult = playback.hresult;
    value.mediaFailureCode = playback.mediaFailureCode;
    value.audioFailure = playback.audioFailure;
    if ((playbackOutcome != PreviewPresentationOutcome::changed
            && playbackOutcome != PreviewPresentationOutcome::noOp)
        || state_ != PreviewPresentationState::paused) {
        value.failure = PreviewPresentationFailureCode::playbackFailure;
    }
    return value;
}

PreviewPresentationReceipt PreviewPresentationSession::resume(
    std::uint64_t expectedGeneration
) {
    std::unique_lock operationLock(operationMutex_);
    {
        std::scoped_lock lifecycleLock(lifecycleMutex_);
        if (closeRequested_) {
            return refused(
                PreviewPresentationStage::resumePlayback,
                PreviewPresentationFailureCode::invalidRequest,
                E_ILLEGAL_METHOD_CALL
            );
        }
    }
    if (expectedGeneration == 0 || expectedGeneration != generation_) {
        return receipt(
            PreviewPresentationOutcome::stale,
            PreviewPresentationStage::resumePlayback
        );
    }
    if (state_ == PreviewPresentationState::playing) {
        return receipt(
            PreviewPresentationOutcome::noOp,
            PreviewPresentationStage::resumePlayback
        );
    }
    if (state_ != PreviewPresentationState::paused) {
        return refused(
            PreviewPresentationStage::resumePlayback,
            PreviewPresentationFailureCode::invalidRequest,
            E_ILLEGAL_METHOD_CALL
        );
    }
    const auto playback = playback_->resume(expectedGeneration);
    const auto playbackOutcome = outcomeFor(playback.outcome);
    state_ = stateFor(playback.state);
    auto value = receipt(playbackOutcome, PreviewPresentationStage::resumePlayback);
    value.hresult = playback.hresult;
    value.mediaFailureCode = playback.mediaFailureCode;
    value.audioFailure = playback.audioFailure;
    if ((playbackOutcome != PreviewPresentationOutcome::changed
            && playbackOutcome != PreviewPresentationOutcome::noOp)
        || state_ != PreviewPresentationState::playing) {
        value.failure = PreviewPresentationFailureCode::playbackFailure;
    }
    return value;
}

PreviewPresentationReceipt PreviewPresentationSession::resize(
    std::uint32_t width,
    std::uint32_t height,
    std::stop_token cancellation
) {
    std::unique_lock operationLock(operationMutex_);
    {
        std::scoped_lock lifecycleLock(lifecycleMutex_);
        if (closeRequested_) {
            return refused(
                PreviewPresentationStage::resize,
                PreviewPresentationFailureCode::invalidRequest,
                E_ILLEGAL_METHOD_CALL
            );
        }
    }
    if (terminalSurfaceReceipt_.has_value()) {
        auto value = receipt(
            outcomeFor(terminalSurfaceReceipt_->outcome),
            PreviewPresentationStage::resize
        );
        value.failure = PreviewPresentationFailureCode::surfaceFailure;
        value.hresult = terminalSurfaceReceipt_->hresult;
        return value;
    }
    auto operation = beginOperation(generation_);
    if (operation == nullptr) {
        return refused(
            PreviewPresentationStage::resize,
            PreviewPresentationFailureCode::invalidRequest,
            HRESULT_FROM_WIN32(ERROR_INVALID_STATE)
        );
    }
    ScopeExit cleanup([&] { finishOperation(operation); });
    std::stop_callback cancellationCallback(
        cancellation,
        [&operation] { operation->cancellation.request_stop(); }
    );
    const auto surface = surface_->resize(
        width,
        height,
        operation->cancellation.get_token()
    );
    presentSerial_ = surface.presentSerial;
    if (surface.outcome == render::D3d11PreviewSurfaceOutcome::cleared) {
        presentationDirty_ = cachedFrame_.has_value();
        pendingRenderedFrame_.reset();
        pendingRenderedSourceTimestamp_.reset();
        pendingRenderedTargetFrame_.reset();
    }
    const bool terminalSurface = isSurfaceTerminal(surface.outcome);
    if (terminalSurface) {
        terminalSurfaceReceipt_ = surface;
        state_ = surface.outcome == render::D3d11PreviewSurfaceOutcome::failed
            ? PreviewPresentationState::failed
            : PreviewPresentationState::invalidated;
        if (generation_ != 0) {
            static_cast<void>(playback_->cancel(generation_));
        }
        clearFrameState();
    }
    auto value = receipt(outcomeFor(surface.outcome), PreviewPresentationStage::resize);
    value.hresult = surface.hresult;
    if (terminalSurface) {
        value.failure = PreviewPresentationFailureCode::surfaceFailure;
    }
    return value;
}

PreviewPresentationReceipt PreviewPresentationSession::cancel(
    std::uint64_t expectedGeneration
) {
    const bool invalidGeneration = expectedGeneration == 0;
    bool closeGate;
    {
        std::scoped_lock lifecycleLock(lifecycleMutex_);
        closeGate = closeRequested_;
        if (!invalidGeneration && !closeGate && activeOperation_ != nullptr
            && activeOperation_->admittedGeneration == expectedGeneration) {
            activeOperation_->cancellation.request_stop();
        }
    }
    std::unique_lock operationLock(operationMutex_);
    {
        std::scoped_lock lifecycleLock(lifecycleMutex_);
        closeGate = closeGate || closeRequested_;
    }
    if (closeGate) {
        return refused(
            PreviewPresentationStage::cancel,
            PreviewPresentationFailureCode::invalidRequest,
            E_ILLEGAL_METHOD_CALL
        );
    }
    if (invalidGeneration) {
        auto value = receipt(
            PreviewPresentationOutcome::stale,
            PreviewPresentationStage::cancel
        );
        value.failure = PreviewPresentationFailureCode::invalidRequest;
        value.hresult = E_INVALIDARG;
        return value;
    }
    if (expectedGeneration != generation_) {
        return receipt(PreviewPresentationOutcome::stale, PreviewPresentationStage::cancel);
    }
    const auto playback = playback_->cancel(expectedGeneration);
    const auto surface = surface_->clear({});
    state_ = stateFor(playback.state);
    presentSerial_ = surface.presentSerial;
    settings_.reset();
    clearFrameState();
    const bool terminalSurface = isSurfaceTerminal(surface.outcome);
    if (terminalSurface) {
        terminalSurfaceReceipt_ = surface;
        state_ = surface.outcome == render::D3d11PreviewSurfaceOutcome::failed
            ? PreviewPresentationState::failed
            : PreviewPresentationState::invalidated;
    }
    auto value = receipt(
        terminalSurface ? outcomeFor(surface.outcome) : outcomeFor(playback.outcome),
        PreviewPresentationStage::cancel
    );
    value.hresult = terminalSurface ? surface.hresult : playback.hresult;
    value.mediaFailureCode = terminalSurface ? -1 : playback.mediaFailureCode;
    value.audioFailure = terminalSurface
        ? media::AudioPlaybackFailureCode::none
        : playback.audioFailure;
    if (terminalSurface) {
        value.failure = PreviewPresentationFailureCode::surfaceFailure;
    } else if (playback.failure != media::HeadlessAvPlaybackFailureCode::none) {
        value.failure = PreviewPresentationFailureCode::playbackFailure;
    }
    return value;
}

PreviewPresentationReceipt PreviewPresentationSession::snapshot() const {
    std::unique_lock operationLock(operationMutex_);
    return receipt(PreviewPresentationOutcome::noOp);
}

PreviewPresentationReceipt PreviewPresentationSession::close() {
    {
        std::scoped_lock lifecycleLock(lifecycleMutex_);
        if (closeReceipt_.has_value()) {
            return *closeReceipt_;
        }
        closeRequested_ = true;
        if (activeOperation_ != nullptr) {
            activeOperation_->cancellation.request_stop();
        }
    }
    std::unique_lock operationLock(operationMutex_);
    {
        std::scoped_lock lifecycleLock(lifecycleMutex_);
        if (closeReceipt_.has_value()) {
            return *closeReceipt_;
        }
    }
    const auto playback = playback_->close();
    const auto surface = surface_->close();
    state_ = PreviewPresentationState::closed;
    presentSerial_ = surface.presentSerial;
    clearFrameState();
    const auto playbackOutcome = outcomeFor(playback.outcome);
    const bool playbackFailed = playbackOutcome == PreviewPresentationOutcome::failed
        || playbackOutcome == PreviewPresentationOutcome::invalidated
        || playbackOutcome == PreviewPresentationOutcome::refused;
    const bool surfaceFailed = isSurfaceTerminal(surface.outcome)
        || surface.outcome == render::D3d11PreviewSurfaceOutcome::refused;
    auto value = receipt(
        playbackFailed
            ? playbackOutcome
            : surfaceFailed
                ? outcomeFor(surface.outcome)
                : PreviewPresentationOutcome::changed,
        PreviewPresentationStage::close
    );
    value.hresult = FAILED(playback.hresult) ? playback.hresult : surface.hresult;
    value.mediaFailureCode = playback.mediaFailureCode;
    value.audioFailure = playback.audioFailure;
    if (playbackFailed) {
        value.failure = PreviewPresentationFailureCode::playbackFailure;
    } else if (surfaceFailed) {
        value.failure = PreviewPresentationFailureCode::surfaceFailure;
    }
    {
        std::scoped_lock lifecycleLock(lifecycleMutex_);
        closeReceipt_ = value;
    }
    return value;
}

PreviewPresentationReceipt PreviewPresentationSession::receipt(
    PreviewPresentationOutcome outcome,
    PreviewPresentationStage stage
) const {
    PreviewPresentationReceipt value;
    value.generation = generation_;
    value.state = state_;
    value.outcome = outcome;
    value.stage = stage;
    value.hasTargetTimelineFrame = targetTimelineFrame_.has_value();
    value.targetTimelineFrame = targetTimelineFrame_.value_or(0);
    value.hasSourcePresentationTimestamp = cachedFrame_.has_value();
    value.sourcePresentationTimestamp = cachedFrame_.has_value()
        ? cachedFrame_->presentationTimestamp
        : 0;
    value.hasCachedFrame = cachedFrame_.has_value();
    value.renderSerial = renderSerial_;
    value.presentSerial = presentSerial_;
    return value;
}

PreviewPresentationReceipt PreviewPresentationSession::refused(
    PreviewPresentationStage stage,
    PreviewPresentationFailureCode failure,
    HRESULT hresult
) const {
    auto value = receipt(
        hresult == HRESULT_FROM_WIN32(ERROR_CANCELLED)
            ? PreviewPresentationOutcome::cancelled
            : PreviewPresentationOutcome::refused,
        stage
    );
    value.failure = failure;
    value.hresult = hresult;
    return value;
}

std::shared_ptr<detail::PreviewPresentationActiveOperation>
PreviewPresentationSession::beginOperation(std::uint64_t admittedGeneration) {
    std::scoped_lock lifecycleLock(lifecycleMutex_);
    if (closeRequested_) {
        return nullptr;
    }
    auto operation = std::make_shared<detail::PreviewPresentationActiveOperation>();
    operation->admittedGeneration = admittedGeneration;
    activeOperation_ = operation;
    return operation;
}

void PreviewPresentationSession::finishOperation(
    const std::shared_ptr<detail::PreviewPresentationActiveOperation>& operation
) {
    std::scoped_lock lifecycleLock(lifecycleMutex_);
    if (activeOperation_ == operation) {
        activeOperation_.reset();
    }
}

void PreviewPresentationSession::clearFrameState() {
    cachedFrame_.reset();
    targetTimelineFrame_.reset();
    pendingRenderedFrame_.reset();
    pendingRenderedSourceTimestamp_.reset();
    pendingRenderedTargetFrame_.reset();
    presentationDirty_ = false;
}

}
