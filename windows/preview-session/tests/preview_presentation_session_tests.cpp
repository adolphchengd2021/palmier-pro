#include "palmier/preview/preview_presentation_session.hpp"

#include <Windows.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>

namespace palmier::preview::detail {

class PreviewPresentationSessionTestAccess final {
public:
    static std::unique_ptr<PreviewPresentationSession> make(
        std::unique_ptr<PreviewPlaybackPort> playback,
        std::unique_ptr<PreviewRenderPort> renderer,
        std::unique_ptr<PreviewSurfacePort> surface
    ) {
        return std::unique_ptr<PreviewPresentationSession>(
            new PreviewPresentationSession(
                std::move(playback),
                std::move(renderer),
                std::move(surface)
            )
        );
    }
};

}

namespace {

using palmier::media::HeadlessAvPlaybackOutcome;
using palmier::media::HeadlessAvPlaybackReceipt;
using palmier::media::HeadlessAvPlaybackState;
using palmier::preview::PreviewPresentationOutcome;
using palmier::preview::PreviewPresentationFailureCode;
using palmier::preview::PreviewPresentationSession;
using palmier::preview::PreviewPresentationSettings;
using palmier::preview::PreviewPresentationStage;
using palmier::preview::PreviewPresentationState;
using palmier::preview::detail::PreviewPlaybackPort;
using palmier::preview::detail::PreviewPresentationSessionTestAccess;
using palmier::preview::detail::PreviewRenderPort;
using palmier::preview::detail::PreviewSurfacePort;
using palmier::render::D3d11PreviewSurfaceOutcome;
using palmier::render::D3d11PreviewSurfaceReceipt;
using palmier::render::D3d11PreviewSurfaceState;
using palmier::render::RenderedFrame;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

palmier::media::PresentedVideoFrame frame(
    std::uint64_t generation,
    std::int64_t timestamp,
    float red
) {
    return {
        generation,
        timestamp,
        {1, 10},
        {1, 1, {{red, 0, 0, 1}}},
    };
}

HeadlessAvPlaybackReceipt playbackReceipt(
    std::uint64_t generation,
    HeadlessAvPlaybackState state,
    HeadlessAvPlaybackOutcome outcome
) {
    HeadlessAvPlaybackReceipt value;
    value.generation = generation;
    value.state = state;
    value.outcome = outcome;
    return value;
}

D3d11PreviewSurfaceReceipt surfaceReceipt(
    D3d11PreviewSurfaceState state,
    D3d11PreviewSurfaceOutcome outcome,
    std::uint64_t presentSerial,
    HRESULT result = S_OK
) {
    D3d11PreviewSurfaceReceipt value;
    value.state = state;
    value.outcome = outcome;
    value.hresult = result;
    value.width = 640;
    value.height = 360;
    value.presentSerial = presentSerial;
    return value;
}

struct PlaybackState final {
    std::uint64_t generation{};
    HeadlessAvPlaybackState state{HeadlessAvPlaybackState::idle};
    std::size_t playCalls{};
    std::size_t tickCalls{};
    std::size_t seekCalls{};
    std::size_t pauseCalls{};
    std::size_t resumeCalls{};
    std::size_t cancelCalls{};
    std::size_t closeCalls{};
    std::deque<HeadlessAvPlaybackReceipt> plays;
    std::deque<HeadlessAvPlaybackReceipt> ticks;
    bool blockNextTick{};
    bool tickEntered{};
    bool blockNextPlay{};
    bool playEntered{};
    bool releasePlay{};
    bool playCancellationObserved{};
    bool failClose{};
    bool failCancel{};
    std::int64_t targetTimelineFrame{};
    std::optional<palmier::media::DecodeFrameStart> decodeStart;
    std::optional<palmier::media::HeadlessAvPlaybackSeekMode> seekMode;
    std::function<void()> afterTick;
    std::mutex mutex;
    std::condition_variable condition;
};

class FakePlayback final : public PreviewPlaybackPort {
public:
    explicit FakePlayback(std::shared_ptr<PlaybackState> state)
        : state_(std::move(state)) {}

    HeadlessAvPlaybackReceipt play(
        const std::filesystem::path&,
        std::int64_t,
        palmier::audio::FrameRate,
        palmier::media::DecodeFrameStart decodeStart,
        std::stop_token cancellation
    ) override {
        std::unique_lock lock(state_->mutex);
        ++state_->playCalls;
        state_->decodeStart = decodeStart;
        if (state_->blockNextPlay) {
            state_->playEntered = true;
            state_->condition.notify_all();
            std::stop_callback cancellationCallback(
                cancellation,
                [this] { state_->condition.notify_all(); }
            );
            state_->condition.wait(
                lock,
                [this, &cancellation] {
                    return state_->releasePlay || cancellation.stop_requested();
                }
            );
            state_->playCancellationObserved = cancellation.stop_requested();
            state_->blockNextPlay = false;
        }
        if (cancellation.stop_requested()) {
            return playbackReceipt(
                state_->generation,
                state_->state,
                HeadlessAvPlaybackOutcome::cancelled
            );
        }
        if (!state_->plays.empty()) {
            auto value = std::move(state_->plays.front());
            state_->plays.pop_front();
            state_->generation = value.generation;
            state_->state = value.state;
            return value;
        }
        if (state_->generation == 0) {
            state_->generation = 1;
            state_->state = HeadlessAvPlaybackState::playing;
            return playbackReceipt(1, state_->state, HeadlessAvPlaybackOutcome::changed);
        }
        return playbackReceipt(
            state_->generation,
            state_->state,
            HeadlessAvPlaybackOutcome::noOp
        );
    }

    HeadlessAvPlaybackReceipt tick(
        std::uint64_t expectedGeneration,
        std::stop_token cancellation
    ) override {
        std::unique_lock lock(state_->mutex);
        ++state_->tickCalls;
        if (state_->blockNextTick) {
            state_->tickEntered = true;
            state_->condition.notify_all();
            std::stop_callback cancellationCallback(
                cancellation,
                [this] { state_->condition.notify_all(); }
            );
            state_->condition.wait(
                lock,
                [&cancellation] { return cancellation.stop_requested(); }
            );
            state_->blockNextTick = false;
            state_->state = HeadlessAvPlaybackState::cancelled;
            return playbackReceipt(
                expectedGeneration,
                state_->state,
                HeadlessAvPlaybackOutcome::cancelled
            );
        }
        if (state_->ticks.empty()) {
            auto value = playbackReceipt(
                expectedGeneration,
                state_->state,
                HeadlessAvPlaybackOutcome::noOp
            );
            value.hasTargetTimelineFrame = true;
            value.targetTimelineFrame = state_->targetTimelineFrame;
            return value;
        }
        auto value = std::move(state_->ticks.front());
        state_->ticks.pop_front();
        state_->state = value.state;
        if (value.hasTargetTimelineFrame) {
            state_->targetTimelineFrame = value.targetTimelineFrame;
        }
        if (state_->afterTick) {
            state_->afterTick();
        }
        return value;
    }

    HeadlessAvPlaybackReceipt seek(
        std::uint64_t expectedGeneration,
        const std::filesystem::path&,
        std::int64_t targetTimelineFrame,
        palmier::audio::FrameRate,
        palmier::media::DecodeFrameStart decodeStart,
        palmier::media::HeadlessAvPlaybackSeekMode mode,
        std::stop_token cancellation
    ) override {
        std::scoped_lock lock(state_->mutex);
        ++state_->seekCalls;
        if (expectedGeneration != state_->generation) {
            return playbackReceipt(
                state_->generation,
                state_->state,
                HeadlessAvPlaybackOutcome::stale
            );
        }
        if (cancellation.stop_requested()) {
            return playbackReceipt(
                state_->generation,
                state_->state,
                HeadlessAvPlaybackOutcome::cancelled
            );
        }
        ++state_->generation;
        state_->state = mode == palmier::media::HeadlessAvPlaybackSeekMode::paused
            ? HeadlessAvPlaybackState::paused
            : HeadlessAvPlaybackState::playing;
        state_->targetTimelineFrame = targetTimelineFrame;
        state_->decodeStart = decodeStart;
        state_->seekMode = mode;
        auto value = playbackReceipt(
            state_->generation,
            state_->state,
            HeadlessAvPlaybackOutcome::changed
        );
        value.hasTargetTimelineFrame = true;
        value.targetTimelineFrame = targetTimelineFrame;
        value.frame = frame(state_->generation, decodeStart.frameIndex, 0.5F);
        return value;
    }

    HeadlessAvPlaybackReceipt cancel(std::uint64_t expectedGeneration) override {
        std::scoped_lock lock(state_->mutex);
        ++state_->cancelCalls;
        state_->state = state_->failCancel
            ? HeadlessAvPlaybackState::failed
            : HeadlessAvPlaybackState::cancelled;
        auto value = playbackReceipt(
            expectedGeneration,
            state_->state,
            state_->failCancel
                ? HeadlessAvPlaybackOutcome::failed
                : HeadlessAvPlaybackOutcome::cancelled
        );
        if (state_->failCancel) {
            value.failure = palmier::media::HeadlessAvPlaybackFailureCode::audioFailure;
            value.hresult = E_FAIL;
        }
        return value;
    }

    HeadlessAvPlaybackReceipt pause(std::uint64_t expectedGeneration) override {
        std::scoped_lock lock(state_->mutex);
        ++state_->pauseCalls;
        if (expectedGeneration != state_->generation) {
            return playbackReceipt(
                state_->generation,
                state_->state,
                HeadlessAvPlaybackOutcome::stale
            );
        }
        const auto changed = state_->state != HeadlessAvPlaybackState::paused;
        state_->state = HeadlessAvPlaybackState::paused;
        return playbackReceipt(
            state_->generation,
            state_->state,
            changed ? HeadlessAvPlaybackOutcome::changed
                    : HeadlessAvPlaybackOutcome::noOp
        );
    }

    HeadlessAvPlaybackReceipt resume(std::uint64_t expectedGeneration) override {
        std::scoped_lock lock(state_->mutex);
        ++state_->resumeCalls;
        if (expectedGeneration != state_->generation) {
            return playbackReceipt(
                state_->generation,
                state_->state,
                HeadlessAvPlaybackOutcome::stale
            );
        }
        const auto changed = state_->state != HeadlessAvPlaybackState::playing;
        state_->state = HeadlessAvPlaybackState::playing;
        return playbackReceipt(
            state_->generation,
            state_->state,
            changed ? HeadlessAvPlaybackOutcome::changed
                    : HeadlessAvPlaybackOutcome::noOp
        );
    }

    HeadlessAvPlaybackReceipt snapshot() const override {
        std::scoped_lock lock(state_->mutex);
        return playbackReceipt(
            state_->generation,
            state_->state,
            HeadlessAvPlaybackOutcome::noOp
        );
    }

    HeadlessAvPlaybackReceipt close() override {
        std::scoped_lock lock(state_->mutex);
        ++state_->closeCalls;
        state_->state = HeadlessAvPlaybackState::closed;
        auto value = playbackReceipt(
            state_->generation,
            state_->state,
            state_->failClose
                ? HeadlessAvPlaybackOutcome::failed
                : HeadlessAvPlaybackOutcome::changed
        );
        if (state_->failClose) {
            value.failure = palmier::media::HeadlessAvPlaybackFailureCode::audioFailure;
            value.hresult = E_FAIL;
        }
        return value;
    }

private:
    std::shared_ptr<PlaybackState> state_;
};

struct RenderState final {
    std::size_t calls{};
    bool fail{};
    std::int64_t lastSourceTimestamp{};
    std::int64_t lastTargetTimelineFrame{};
};

class FakeRenderer final : public PreviewRenderPort {
public:
    explicit FakeRenderer(std::shared_ptr<RenderState> state)
        : state_(std::move(state)) {}

    RenderedFrame render(
        const palmier::media::PresentedVideoFrame& source,
        std::int64_t targetTimelineFrame,
        const PreviewPresentationSettings& settings,
        std::stop_token cancellation
    ) override {
        ++state_->calls;
        state_->lastSourceTimestamp = source.presentationTimestamp;
        state_->lastTargetTimelineFrame = targetTimelineFrame;
        if (state_->fail) {
            throw std::runtime_error("scripted render failure");
        }
        if (cancellation.stop_requested()) {
            throw palmier::render::RenderError("cancelled", "/", "cancelled");
        }
        return {
            settings.renderLayer.canvasWidth,
            settings.renderLayer.canvasHeight,
            std::vector<palmier::render::Rgba32Float>(
                static_cast<std::size_t>(settings.renderLayer.canvasWidth)
                    * settings.renderLayer.canvasHeight,
                source.source.pixels.front()
            ),
        };
    }

private:
    std::shared_ptr<RenderState> state_;
};

struct SurfaceState final {
    std::size_t resizeCalls{};
    std::size_t presentCalls{};
    std::size_t clearCalls{};
    std::size_t closeCalls{};
    std::uint64_t presentSerial{};
    std::deque<D3d11PreviewSurfaceOutcome> presentOutcomes;
    D3d11PreviewSurfaceOutcome clearOutcome{D3d11PreviewSurfaceOutcome::cleared};
    D3d11PreviewSurfaceOutcome closeOutcome{D3d11PreviewSurfaceOutcome::cleared};
};

class FakeSurface final : public PreviewSurfacePort {
public:
    explicit FakeSurface(std::shared_ptr<SurfaceState> state)
        : state_(std::move(state)) {}

    D3d11PreviewSurfaceReceipt resize(
        std::uint32_t,
        std::uint32_t,
        std::stop_token cancellation
    ) override {
        ++state_->resizeCalls;
        if (cancellation.stop_requested()) {
            return surfaceReceipt(
                D3d11PreviewSurfaceState::ready,
                D3d11PreviewSurfaceOutcome::cancelled,
                state_->presentSerial,
                HRESULT_FROM_WIN32(ERROR_CANCELLED)
            );
        }
        return surfaceReceipt(
            D3d11PreviewSurfaceState::ready,
            D3d11PreviewSurfaceOutcome::cleared,
            ++state_->presentSerial
        );
    }

    D3d11PreviewSurfaceReceipt present(
        const RenderedFrame&,
        std::stop_token cancellation
    ) override {
        ++state_->presentCalls;
        if (cancellation.stop_requested()) {
            return surfaceReceipt(
                D3d11PreviewSurfaceState::ready,
                D3d11PreviewSurfaceOutcome::cancelled,
                state_->presentSerial,
                HRESULT_FROM_WIN32(ERROR_CANCELLED)
            );
        }
        auto outcome = D3d11PreviewSurfaceOutcome::presented;
        if (!state_->presentOutcomes.empty()) {
            outcome = state_->presentOutcomes.front();
            state_->presentOutcomes.pop_front();
        }
        if (outcome == D3d11PreviewSurfaceOutcome::presented) {
            ++state_->presentSerial;
        }
        auto surfaceState = D3d11PreviewSurfaceState::ready;
        if (outcome == D3d11PreviewSurfaceOutcome::occluded) {
            surfaceState = D3d11PreviewSurfaceState::occluded;
        } else if (outcome == D3d11PreviewSurfaceOutcome::invalidated
            || outcome == D3d11PreviewSurfaceOutcome::unavailable) {
            surfaceState = D3d11PreviewSurfaceState::invalidated;
        } else if (outcome == D3d11PreviewSurfaceOutcome::failed) {
            surfaceState = D3d11PreviewSurfaceState::failed;
        }
        return surfaceReceipt(surfaceState, outcome, state_->presentSerial);
    }

    D3d11PreviewSurfaceReceipt clear(std::stop_token) override {
        ++state_->clearCalls;
        const auto outcome = state_->clearOutcome;
        auto surfaceState = D3d11PreviewSurfaceState::ready;
        if (outcome == D3d11PreviewSurfaceOutcome::invalidated
            || outcome == D3d11PreviewSurfaceOutcome::unavailable) {
            surfaceState = D3d11PreviewSurfaceState::invalidated;
        } else if (outcome == D3d11PreviewSurfaceOutcome::failed) {
            surfaceState = D3d11PreviewSurfaceState::failed;
        }
        return surfaceReceipt(
            surfaceState,
            outcome,
            outcome == D3d11PreviewSurfaceOutcome::cleared
                ? ++state_->presentSerial
                : state_->presentSerial,
            outcome == D3d11PreviewSurfaceOutcome::failed
                ? E_FAIL
                : outcome == D3d11PreviewSurfaceOutcome::invalidated
                    ? E_ABORT
                    : S_OK
        );
    }

    D3d11PreviewSurfaceReceipt snapshot() const override {
        return surfaceReceipt(
            D3d11PreviewSurfaceState::ready,
            D3d11PreviewSurfaceOutcome::noOp,
            state_->presentSerial
        );
    }

    D3d11PreviewSurfaceReceipt close() override {
        ++state_->closeCalls;
        const auto outcome = state_->closeOutcome;
        return surfaceReceipt(
            D3d11PreviewSurfaceState::closed,
            outcome,
            state_->presentSerial,
            outcome == D3d11PreviewSurfaceOutcome::failed ? E_FAIL : S_OK
        );
    }

private:
    std::shared_ptr<SurfaceState> state_;
};

struct Fixture final {
    std::shared_ptr<PlaybackState> playback = std::make_shared<PlaybackState>();
    std::shared_ptr<RenderState> renderer = std::make_shared<RenderState>();
    std::shared_ptr<SurfaceState> surface = std::make_shared<SurfaceState>();
    std::unique_ptr<PreviewPresentationSession> session =
        PreviewPresentationSessionTestAccess::make(
            std::make_unique<FakePlayback>(playback),
            std::make_unique<FakeRenderer>(renderer),
            std::make_unique<FakeSurface>(surface)
        );
};

PreviewPresentationSettings settings(float exposure = 0) {
    return {{
        2,
        1,
        10,
        "timeline",
        "video-track",
        "preview-layer",
        "preview-media",
        0,
        100,
        0,
        {},
        1,
        exposure,
    }};
}

void start(Fixture& fixture, PreviewPresentationSettings value = settings()) {
    const auto result = fixture.session->play("input.mov", 0, {10, 1}, std::move(value));
    require(result.outcome == PreviewPresentationOutcome::changed, "preview did not start");
    require(result.generation == 1, "preview generation changed");
}

void enqueueFrame(
    Fixture& fixture,
    std::int64_t sourceTimestamp = 0,
    std::int64_t targetTimelineFrame = 0
) {
    auto value = playbackReceipt(
        1,
        HeadlessAvPlaybackState::playing,
        HeadlessAvPlaybackOutcome::changed
    );
    value.hasTargetTimelineFrame = true;
    value.targetTimelineFrame = targetTimelineFrame;
    value.frame = frame(1, sourceTimestamp, 0.5F);
    fixture.playback->ticks.push_back(std::move(value));
}

void oneTickConsumesAndPresentsOneFrame() {
    Fixture fixture;
    start(fixture);
    enqueueFrame(fixture, 7, 3);

    const auto first = fixture.session->tick(1);
    require(first.outcome == PreviewPresentationOutcome::presented, "frame was not presented");
    require(first.renderSerial == 1 && first.presentSerial == 1, "serials differ");
    require(first.sourcePresentationTimestamp == 7, "source PTS changed");
    require(first.targetTimelineFrame == 3, "target timeline frame changed");
    require(fixture.playback->tickCalls == 1, "tick was consumed more than once");
    require(fixture.renderer->calls == 1, "frame was not rendered exactly once");
    require(fixture.surface->presentCalls == 1, "surface was not called exactly once");

    const auto held = fixture.session->tick(1);
    require(held.outcome == PreviewPresentationOutcome::noOp, "held frame was not coalesced");
    require(fixture.playback->tickCalls == 2, "second scheduler tick was not consumed once");
    require(fixture.renderer->calls == 1, "held frame was rendered again");
    require(fixture.surface->presentCalls == 1, "held frame was presented again");
}

void busySurfaceRetriesTheRenderedFrame() {
    Fixture fixture;
    fixture.surface->presentOutcomes = {
        D3d11PreviewSurfaceOutcome::noOp,
        D3d11PreviewSurfaceOutcome::presented,
    };
    start(fixture);
    enqueueFrame(fixture);

    require(
        fixture.session->tick(1).outcome == PreviewPresentationOutcome::noOp,
        "busy surface was not reported"
    );
    require(
        fixture.session->tick(1).outcome == PreviewPresentationOutcome::presented,
        "busy frame was not retried"
    );
    require(fixture.renderer->calls == 1, "busy retry rerendered identical content");
    require(fixture.surface->presentCalls == 2, "busy retry did not present once per tick");
}

void completedPlaybackRetriesAndResizesTheFinalFrame() {
    Fixture fixture;
    fixture.surface->presentOutcomes = {
        D3d11PreviewSurfaceOutcome::noOp,
        D3d11PreviewSurfaceOutcome::noOp,
        D3d11PreviewSurfaceOutcome::presented,
        D3d11PreviewSurfaceOutcome::presented,
    };
    start(fixture);
    enqueueFrame(fixture);
    auto completed = playbackReceipt(
        1,
        HeadlessAvPlaybackState::completed,
        HeadlessAvPlaybackOutcome::noOp
    );
    fixture.playback->ticks.push_back(completed);

    require(
        fixture.session->tick(1).outcome == PreviewPresentationOutcome::noOp,
        "first final-frame Present was not busy"
    );
    require(
        fixture.session->tick(1).state == PreviewPresentationState::completed,
        "audio completion was not retained"
    );
    require(
        fixture.session->tick(1).outcome == PreviewPresentationOutcome::presented,
        "completed final frame was not retried"
    );
    require(fixture.renderer->calls == 1, "completed retry rerendered the final frame");

    static_cast<void>(fixture.session->resize(800, 450));
    const auto resized = fixture.session->tick(1);
    require(resized.outcome == PreviewPresentationOutcome::presented, "completed resize froze");
    require(resized.state == PreviewPresentationState::completed, "resize revived playback");
    require(fixture.renderer->calls == 2, "completed resize did not rerender once");
}

void clipEndCompletesWithoutRenderingPastBoundary() {
    Fixture fixture;
    auto value = settings();
    value.renderLayer.durationFrames = 2;
    start(fixture, value);
    enqueueFrame(fixture, 7, 1);
    require(
        fixture.session->tick(1).outcome == PreviewPresentationOutcome::presented,
        "last active clip frame was not presented"
    );

    auto boundary = playbackReceipt(
        1,
        HeadlessAvPlaybackState::playing,
        HeadlessAvPlaybackOutcome::changed
    );
    boundary.hasTargetTimelineFrame = true;
    boundary.targetTimelineFrame = 2;
    boundary.frame = frame(2, 8, 0.75F);
    fixture.playback->ticks.push_back(std::move(boundary));
    const auto completed = fixture.session->tick(1);
    require(completed.state == PreviewPresentationState::completed, "clip end did not complete");
    require(completed.outcome == PreviewPresentationOutcome::changed, "clip end was hidden");
    require(completed.sourcePresentationTimestamp == 7, "clip end replaced the last valid frame");
    require(fixture.playback->cancelCalls == 1, "clip end did not stop source playback");
    require(fixture.renderer->calls == 1, "clip end rendered an inactive frame");
    require(fixture.surface->presentCalls == 1, "clip end presented an inactive frame");
    require(
        fixture.session->tick(1).outcome == PreviewPresentationOutcome::noOp,
        "completed clip consumed another playback tick"
    );
    require(fixture.playback->tickCalls == 2, "completed clip reached playback again");

    Fixture failed;
    start(failed, value);
    enqueueFrame(failed, 7, 1);
    static_cast<void>(failed.session->tick(1));
    failed.playback->failCancel = true;
    auto failedBoundary = playbackReceipt(
        1,
        HeadlessAvPlaybackState::playing,
        HeadlessAvPlaybackOutcome::changed
    );
    failedBoundary.hasTargetTimelineFrame = true;
    failedBoundary.targetTimelineFrame = 2;
    failedBoundary.frame = frame(2, 8, 0.75F);
    failed.playback->ticks.push_back(std::move(failedBoundary));
    const auto failure = failed.session->tick(1);
    require(failure.outcome == PreviewPresentationOutcome::failed, "clip-end stop failure hidden");
    require(
        failure.failure == PreviewPresentationFailureCode::playbackFailure,
        "clip-end stop failure lost its code"
    );
    require(failed.renderer->calls == 1, "clip-end stop failure rendered inactive content");
}

void resizeMarksTheCachedFrameDirty() {
    Fixture fixture;
    start(fixture);
    enqueueFrame(fixture);
    require(
        fixture.session->tick(1).outcome == PreviewPresentationOutcome::presented,
        "initial frame was not presented"
    );
    require(
        fixture.session->resize(800, 450).stage == PreviewPresentationStage::resize,
        "resize was not routed"
    );
    require(
        fixture.session->tick(1).outcome == PreviewPresentationOutcome::presented,
        "cached frame was not presented after resize"
    );
    require(fixture.renderer->calls == 2, "resize did not rerender cached source once");
    require(fixture.playback->tickCalls == 2, "resize path bypassed scheduler tick ownership");
}

void settingsCanChangeWithoutRestartingPlayback() {
    const auto verify = [](
        const std::string& name,
        const std::function<void(PreviewPresentationSettings&)>& mutate,
        std::int64_t timelineFrame
    ) {
        Fixture fixture;
        auto initial = settings();
        initial.renderLayer.timelineStartFrame = timelineFrame;
        require(
            fixture.session->play(
                "input.mov",
                timelineFrame,
                {static_cast<std::uint32_t>(initial.renderLayer.framesPerSecond), 1},
                initial
            ).outcome
                == PreviewPresentationOutcome::changed,
            name + " initial play failed"
        );
        enqueueFrame(fixture, 0, timelineFrame);
        static_cast<void>(fixture.session->tick(1));

        auto changedSettings = initial;
        mutate(changedSettings);
        const auto changed = fixture.session->play(
            "input.mov",
            timelineFrame,
            {static_cast<std::uint32_t>(changedSettings.renderLayer.framesPerSecond), 1},
            changedSettings
        );
        require(changed.outcome == PreviewPresentationOutcome::changed, name + " no-op'd");
        require(changed.generation == 1, name + " restarted playback");
        require(fixture.playback->playCalls == 2, name + " skipped playback validation");
        require(
            fixture.session->tick(1).outcome == PreviewPresentationOutcome::presented,
            name + " did not rerender cached frame"
        );
        require(fixture.renderer->calls == 2, name + " render count differs");
        require(
            fixture.session->play(
                "input.mov",
                timelineFrame,
                {static_cast<std::uint32_t>(changedSettings.renderLayer.framesPerSecond), 1},
                changedSettings
            ).outcome == PreviewPresentationOutcome::noOp,
            name + " identical settings changed twice"
        );
    };

    verify("canvas width", [](auto& value) { value.renderLayer.canvasWidth = 3; }, 0);
    verify("canvas height", [](auto& value) { value.renderLayer.canvasHeight = 2; }, 0);
    verify("timeline ID", [](auto& value) { value.renderLayer.timelineId = "other"; }, 0);
    verify("track ID", [](auto& value) { value.renderLayer.trackId = "other"; }, 0);
    verify("clip ID", [](auto& value) { value.renderLayer.clipId = "other"; }, 0);
    verify("media ID", [](auto& value) { value.renderLayer.mediaId = "other"; }, 0);
    verify("duration", [](auto& value) { value.renderLayer.durationFrames = 99; }, 0);
    verify("center X", [](auto& value) { value.renderLayer.transform.centerX = 0.25F; }, 0);
    verify("center Y", [](auto& value) { value.renderLayer.transform.centerY = 0.75F; }, 0);
    verify("width", [](auto& value) { value.renderLayer.transform.width = 0.5F; }, 0);
    verify("height", [](auto& value) { value.renderLayer.transform.height = 0.5F; }, 0);
    verify("rotation", [](auto& value) { value.renderLayer.transform.rotationDegrees = 15; }, 0);
    verify("opacity", [](auto& value) { value.renderLayer.opacity = 0.5F; }, 0);
    verify("exposure", [](auto& value) { value.renderLayer.exposureEv = 1.0F; }, 0);
}

void sourceMappingChangeRequiresPlaybackRestart() {
    Fixture fixture;
    auto initial = settings();
    initial.renderLayer.timelineStartFrame = 5;
    require(
        fixture.session->play("input.mov", 5, {10, 1}, initial).outcome
            == PreviewPresentationOutcome::changed,
        "source mapping baseline did not start"
    );
    auto changed = initial;
    changed.renderLayer.timelineStartFrame = 4;
    require(
        fixture.session->play("input.mov", 5, {10, 1}, changed).outcome
            == PreviewPresentationOutcome::refused,
        "source mapping changed without a playback restart"
    );

    fixture.playback->plays.push_back(playbackReceipt(
        2,
        HeadlessAvPlaybackState::playing,
        HeadlessAvPlaybackOutcome::changed
    ));
    const auto restarted = fixture.session->play("input.mov", 5, {10, 1}, changed);
    require(restarted.outcome == PreviewPresentationOutcome::changed, "restarted mapping refused");
    require(restarted.generation == 2, "restarted mapping kept the old generation");

    auto rateChanged = changed;
    rateChanged.renderLayer.framesPerSecond = 11;
    require(
        fixture.session->play("input.mov", 5, {11, 1}, rateChanged).outcome
            == PreviewPresentationOutcome::refused,
        "frame-rate mapping changed without a playback restart"
    );
}

void invalidAndStaleRequestsDoNotReachOwnedPorts() {
    Fixture fixture;
    auto invalid = settings();
    invalid.renderLayer.canvasWidth = 0;
    require(
        fixture.session->play("input.mov", 0, {10, 1}, invalid).outcome
            == PreviewPresentationOutcome::refused,
        "invalid settings were accepted"
    );
    require(fixture.playback->playCalls == 0, "invalid settings reached playback");
    auto seekable = settings();
    seekable.renderLayer.sourceStartFrame = 1;
    require(
        fixture.session->play("input.mov", 0, {10, 1}, seekable).outcome
            == PreviewPresentationOutcome::changed,
        "nonzero source start was not forwarded to seek support"
    );
    require(fixture.playback->playCalls == 1, "source seek skipped playback");
    require(
        fixture.playback->decodeStart.has_value()
            && fixture.playback->decodeStart->frameIndex == 1
            && fixture.playback->decodeStart->frameRate.numerator == 10
            && fixture.playback->decodeStart->frameRate.denominator == 1,
        "source seek mapping changed before playback"
    );

    Fixture staleFixture;
    start(staleFixture);
    require(
        staleFixture.session->tick(2).outcome == PreviewPresentationOutcome::stale,
        "stale generation was accepted"
    );
    require(staleFixture.playback->tickCalls == 0, "stale generation reached playback");
}

void pauseResumePreservesGenerationAndCachedFrame() {
    Fixture fixture;
    start(fixture);
    enqueueFrame(fixture, 3'072, 3);
    require(
        fixture.session->tick(1).outcome == PreviewPresentationOutcome::presented,
        "transport setup did not present"
    );
    const auto presentCalls = fixture.surface->presentCalls;
    const auto tickCalls = fixture.playback->tickCalls;

    const auto paused = fixture.session->pause(1);
    require(paused.outcome == PreviewPresentationOutcome::changed, "preview pause failed");
    require(paused.state == PreviewPresentationState::paused, "preview pause lost state");
    require(paused.generation == 1, "preview pause changed generation");
    require(paused.hasCachedFrame, "preview pause discarded cached frame");
    require(fixture.surface->presentCalls == presentCalls, "preview pause touched surface");
    require(
        fixture.session->tick(1).outcome == PreviewPresentationOutcome::noOp,
        "paused preview tick changed"
    );
    require(fixture.playback->tickCalls == tickCalls, "paused preview reached playback tick");
    require(fixture.session->pause(1).outcome == PreviewPresentationOutcome::noOp, "repeat preview pause changed");
    require(fixture.playback->pauseCalls == 1, "repeat preview pause reached playback");
    require(
        fixture.session->resume(2).outcome == PreviewPresentationOutcome::stale,
        "stale preview resume was accepted"
    );
    require(fixture.playback->resumeCalls == 0, "stale preview resume reached playback");

    const auto resumed = fixture.session->resume(1);
    require(resumed.outcome == PreviewPresentationOutcome::changed, "preview resume failed");
    require(resumed.state == PreviewPresentationState::playing, "preview resume lost state");
    require(resumed.generation == 1, "preview resume changed generation");
    require(resumed.hasCachedFrame, "preview resume discarded cached frame");
    require(fixture.session->resume(1).outcome == PreviewPresentationOutcome::noOp, "repeat preview resume changed");
    require(fixture.playback->resumeCalls == 1, "repeat preview resume reached playback");
}

void seekPresentsExactFramesAndEnforcesGenerationAndBounds() {
    Fixture fixture;
    start(fixture);
    enqueueFrame(fixture, 0, 0);
    require(
        fixture.session->tick(1).outcome == PreviewPresentationOutcome::presented,
        "seek setup did not present"
    );
    const auto renders = fixture.renderer->calls;
    const auto presents = fixture.surface->presentCalls;
    require(
        fixture.session->seek(
            1,
            2,
            static_cast<palmier::media::HeadlessAvPlaybackSeekMode>(99)
        ).outcome == PreviewPresentationOutcome::refused,
        "invalid preview seek mode was accepted"
    );
    require(fixture.playback->seekCalls == 0, "invalid seek mode reached playback");

    const auto paused = fixture.session->seek(
        1,
        3,
        palmier::media::HeadlessAvPlaybackSeekMode::paused
    );
    require(paused.outcome == PreviewPresentationOutcome::presented, "paused seek did not present");
    require(paused.state == PreviewPresentationState::paused, "paused seek started playback");
    require(paused.generation == 2, "paused seek did not replace generation");
    require(paused.hasTargetTimelineFrame && paused.targetTimelineFrame == 3, "paused seek target changed");
    require(paused.hasSourcePresentationTimestamp && paused.sourcePresentationTimestamp == 3, "paused seek source changed");
    require(paused.hasCachedFrame, "paused seek lost cached frame");
    require(fixture.renderer->calls == renders + 1, "paused seek did not render once");
    require(fixture.surface->presentCalls == presents + 1, "paused seek did not present once");
    require(
        fixture.playback->decodeStart.has_value()
            && fixture.playback->decodeStart->frameIndex == 3,
        "paused seek did not map the source frame"
    );
    require(
        fixture.playback->seekMode
            == palmier::media::HeadlessAvPlaybackSeekMode::paused,
        "paused seek changed mode"
    );
    require(
        fixture.session->seek(
            1,
            4,
            palmier::media::HeadlessAvPlaybackSeekMode::paused
        ).outcome == PreviewPresentationOutcome::stale,
        "stale preview seek was accepted"
    );
    require(fixture.playback->seekCalls == 1, "stale preview seek reached playback");
    require(
        fixture.session->seek(
            2,
            -1,
            palmier::media::HeadlessAvPlaybackSeekMode::paused
        ).outcome == PreviewPresentationOutcome::refused,
        "negative preview seek was accepted"
    );
    require(
        fixture.session->seek(
            2,
            100,
            palmier::media::HeadlessAvPlaybackSeekMode::paused
        ).outcome == PreviewPresentationOutcome::refused,
        "end-exclusive preview seek was accepted"
    );
    require(fixture.playback->seekCalls == 1, "out-of-range seek reached playback");

    require(fixture.session->resume(2).state == PreviewPresentationState::playing, "seek resume failed");
    const auto playing = fixture.session->seek(
        2,
        4,
        palmier::media::HeadlessAvPlaybackSeekMode::playing
    );
    require(playing.state == PreviewPresentationState::playing, "playing seek paused");
    require(playing.generation == 3, "playing seek did not replace generation");
    require(playing.targetTimelineFrame == 4, "playing seek target changed");

    std::stop_source cancelled;
    cancelled.request_stop();
    const auto cancelledSeek = fixture.session->seek(
        3,
        5,
        palmier::media::HeadlessAvPlaybackSeekMode::paused,
        cancelled.get_token()
    );
    require(cancelledSeek.outcome == PreviewPresentationOutcome::cancelled, "cancelled seek committed");
    require(fixture.session->snapshot().generation == 3, "cancelled seek changed generation");
    require(fixture.session->close().state == PreviewPresentationState::closed, "seek close failed");
}

void terminalSurfaceStopsPlayback() {
    Fixture fixture;
    fixture.surface->presentOutcomes = {D3d11PreviewSurfaceOutcome::invalidated};
    start(fixture);
    enqueueFrame(fixture);
    const auto result = fixture.session->tick(1);
    require(result.outcome == PreviewPresentationOutcome::invalidated, "surface loss was hidden");
    require(result.state == PreviewPresentationState::invalidated, "surface loss state differs");
    require(!result.hasCachedFrame, "surface loss receipt claimed released frame ownership");
    require(fixture.playback->cancelCalls == 1, "surface loss did not stop playback");
    require(!fixture.session->snapshot().hasCachedFrame, "surface loss retained frame memory");
    const auto refusedRestart = fixture.session->play("input.mov", 0, {10, 1}, settings());
    require(
        refusedRestart.outcome == PreviewPresentationOutcome::invalidated,
        "terminal surface accepted playback restart"
    );
    require(fixture.playback->playCalls == 1, "terminal surface restart reached playback");
    static_cast<void>(fixture.session->close());
    require(
        fixture.session->play("input.mov", 0, {10, 1}, settings()).outcome
            == PreviewPresentationOutcome::refused,
        "terminal surface bypassed closed play gate"
    );
    require(
        fixture.session->resize(800, 450).outcome == PreviewPresentationOutcome::refused,
        "terminal surface bypassed closed resize gate"
    );
}

void renderFailureStopsPlayback() {
    Fixture fixture;
    fixture.renderer->fail = true;
    start(fixture);
    enqueueFrame(fixture);
    const auto result = fixture.session->tick(1);
    require(result.outcome == PreviewPresentationOutcome::failed, "render failure was hidden");
    require(result.stage == PreviewPresentationStage::render, "render failure stage differs");
    require(!result.hasCachedFrame, "render failure receipt claimed released frame ownership");
    require(fixture.playback->cancelCalls == 1, "render failure did not stop playback");
    require(fixture.surface->presentCalls == 0, "failed render reached the surface");
    require(!fixture.session->snapshot().hasCachedFrame, "render failure retained frame memory");
}

void cancellationAfterPlaybackStopsBeforeRender() {
    Fixture fixture;
    start(fixture);
    enqueueFrame(fixture);
    std::stop_source stopped;
    fixture.playback->afterTick = [&stopped] { stopped.request_stop(); };
    const auto result = fixture.session->tick(1, stopped.get_token());
    require(result.outcome == PreviewPresentationOutcome::cancelled, "late cancel was hidden");
    require(result.stage == PreviewPresentationStage::render, "late cancel crossed render");
    require(fixture.renderer->calls == 0, "late cancel reached renderer");
    require(result.hasCachedFrame, "late cancel lost the selected source frame");
}

void postCommitPlaybackFailureAdvancesTheTerminalGeneration() {
    Fixture fixture;
    start(fixture);
    auto failed = playbackReceipt(
        2,
        HeadlessAvPlaybackState::failed,
        HeadlessAvPlaybackOutcome::failed
    );
    failed.failure = palmier::media::HeadlessAvPlaybackFailureCode::audioFailure;
    failed.audioFailure = palmier::media::AudioPlaybackFailureCode::deviceUnavailable;
    failed.hresult = E_FAIL;
    fixture.playback->plays.push_back(failed);

    const auto result = fixture.session->play("input.mov", 1, {10, 1}, settings());
    require(result.outcome == PreviewPresentationOutcome::failed, "playback failure was hidden");
    require(result.generation == 2, "post-commit failure kept the old generation");
    require(result.state == PreviewPresentationState::failed, "post-commit state differs");
    require(
        result.audioFailure
            == palmier::media::AudioPlaybackFailureCode::deviceUnavailable,
        "post-commit failure lost its structured audio failure"
    );
    require(!result.hasCachedFrame, "post-commit failure retained old video");
}

void invalidCancelCannotStopTheFirstPlay() {
    Fixture fixture;
    {
        std::scoped_lock lock(fixture.playback->mutex);
        fixture.playback->blockNextPlay = true;
    }
    std::optional<palmier::preview::PreviewPresentationReceipt> playResult;
    std::thread worker([&] {
        playResult = fixture.session->play("input.mov", 0, {10, 1}, settings());
    });
    {
        std::unique_lock lock(fixture.playback->mutex);
        fixture.playback->condition.wait(
            lock,
            [&fixture] { return fixture.playback->playEntered; }
        );
    }
    std::optional<palmier::preview::PreviewPresentationReceipt> cancelResult;
    std::thread cancelWorker([&] { cancelResult = fixture.session->cancel(0); });
    {
        std::scoped_lock lock(fixture.playback->mutex);
        require(
            !fixture.playback->playCancellationObserved,
            "cancel zero stopped the admitted first play"
        );
        fixture.playback->releasePlay = true;
        fixture.playback->condition.notify_all();
    }
    worker.join();
    cancelWorker.join();
    require(playResult.has_value(), "first play returned no receipt");
    require(playResult->outcome == PreviewPresentationOutcome::changed, "first play was cancelled");
    require(cancelResult.has_value(), "cancel zero returned no receipt");
    require(cancelResult->outcome == PreviewPresentationOutcome::stale, "cancel zero was accepted");
    require(cancelResult->generation == 1, "cancel zero receipt lost current generation");
    require(cancelResult->state == PreviewPresentationState::playing, "cancel zero receipt lost state");
}

void cancellationAndCloseSurfaceFailuresStayObservable() {
    Fixture cancelFixture;
    start(cancelFixture);
    cancelFixture.playback->failCancel = true;
    cancelFixture.surface->clearOutcome = D3d11PreviewSurfaceOutcome::invalidated;
    const auto cancelled = cancelFixture.session->cancel(1);
    require(
        cancelled.outcome == PreviewPresentationOutcome::invalidated,
        "cancel hid surface invalidation"
    );
    require(
        cancelled.failure == palmier::preview::PreviewPresentationFailureCode::surfaceFailure,
        "cancel omitted surface failure code"
    );
    require(cancelled.hresult == E_ABORT, "cancel mixed playback and surface HRESULTs");
    require(cancelled.mediaFailureCode == -1, "cancel mixed playback media failure details");

    Fixture closeFixture;
    start(closeFixture);
    closeFixture.playback->failClose = true;
    const auto closed = closeFixture.session->close();
    require(closed.state == PreviewPresentationState::closed, "failed close lost terminal state");
    require(closed.outcome == PreviewPresentationOutcome::failed, "close failure was success-shaped");
    require(
        closed.failure == palmier::preview::PreviewPresentationFailureCode::playbackFailure,
        "close failure omitted playback code"
    );
    const auto cancelAfterClose = closeFixture.session->cancel(1);
    require(
        cancelAfterClose.outcome == PreviewPresentationOutcome::refused,
        "cancel crossed the close admission gate"
    );
    require(closeFixture.playback->cancelCalls == 0, "closed playback received cancel");
    require(closeFixture.surface->clearCalls == 0, "closed surface received clear");
    const auto tickAfterClose = closeFixture.session->tick(1);
    require(
        tickAfterClose.outcome == PreviewPresentationOutcome::refused,
        "tick crossed the close admission gate"
    );
}

void closeInterruptsOneAdmittedTick() {
    Fixture fixture;
    start(fixture);
    {
        std::scoped_lock lock(fixture.playback->mutex);
        fixture.playback->blockNextTick = true;
    }
    std::optional<palmier::preview::PreviewPresentationReceipt> tickResult;
    std::thread worker([&] { tickResult = fixture.session->tick(1); });
    {
        std::unique_lock lock(fixture.playback->mutex);
        fixture.playback->condition.wait(
            lock,
            [&fixture] { return fixture.playback->tickEntered; }
        );
    }
    const auto closed = fixture.session->close();
    worker.join();

    require(tickResult.has_value(), "interrupted tick returned no receipt");
    require(
        tickResult->outcome == PreviewPresentationOutcome::cancelled,
        "close did not cancel admitted tick"
    );
    require(closed.state == PreviewPresentationState::closed, "close state differs");
    require(fixture.playback->closeCalls == 1, "playback closed more than once");
    require(fixture.surface->closeCalls == 1, "surface closed more than once");
    require(
        fixture.session->close().presentSerial == closed.presentSerial,
        "repeated close receipt changed"
    );
    require(fixture.playback->closeCalls == 1, "repeated close reached playback");
}

}

int main() {
    try {
        oneTickConsumesAndPresentsOneFrame();
        busySurfaceRetriesTheRenderedFrame();
        completedPlaybackRetriesAndResizesTheFinalFrame();
        clipEndCompletesWithoutRenderingPastBoundary();
        resizeMarksTheCachedFrameDirty();
        settingsCanChangeWithoutRestartingPlayback();
        sourceMappingChangeRequiresPlaybackRestart();
        invalidAndStaleRequestsDoNotReachOwnedPorts();
        pauseResumePreservesGenerationAndCachedFrame();
        seekPresentsExactFramesAndEnforcesGenerationAndBounds();
        terminalSurfaceStopsPlayback();
        renderFailureStopsPlayback();
        cancellationAfterPlaybackStopsBeforeRender();
        postCommitPlaybackFailureAdvancesTheTerminalGeneration();
        invalidCancelCannotStopTheFirstPlay();
        cancellationAndCloseSurfaceFailuresStayObservable();
        closeInterruptsOneAdmittedTick();
        std::cout << "PASS preview presentation session tests\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
