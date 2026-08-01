#pragma once

#include "palmier/media/headless_av_playback_session.hpp"
#include "palmier/render/d3d11_preview_surface.hpp"

#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>

namespace palmier::preview {

struct PreviewPresentationSettings final {
    std::uint32_t canvasWidth{};
    std::uint32_t canvasHeight{};
    std::int32_t framesPerSecond{};
    std::string layerId;
    std::string trackId;
    std::string mediaId;
    render::Transform2D transform;
    float opacity{1};
    std::optional<float> exposureEv;
};

struct PreviewPresentationLimits final {
    media::HeadlessAvPlaybackLimits playback;
    render::D3d11PreviewSurfaceLimits surface;
};

enum class PreviewPresentationState {
    idle,
    playing,
    completed,
    cancelled,
    invalidated,
    failed,
    closed,
};

enum class PreviewPresentationOutcome {
    changed,
    presented,
    noOp,
    stale,
    cancelled,
    refused,
    occluded,
    unavailable,
    invalidated,
    failed,
};

enum class PreviewPresentationStage {
    none,
    validate,
    startPlayback,
    tickPlayback,
    render,
    present,
    resize,
    cancel,
    close,
};

enum class PreviewPresentationFailureCode {
    none,
    invalidRequest,
    playbackFailure,
    renderFailure,
    surfaceFailure,
    invariantFailure,
};

struct PreviewPresentationReceipt final {
    std::uint64_t generation{};
    PreviewPresentationState state{PreviewPresentationState::idle};
    PreviewPresentationOutcome outcome{PreviewPresentationOutcome::noOp};
    PreviewPresentationStage stage{PreviewPresentationStage::none};
    PreviewPresentationFailureCode failure{PreviewPresentationFailureCode::none};
    HRESULT hresult{S_OK};
    std::int32_t mediaFailureCode{-1};
    media::AudioPlaybackFailureCode audioFailure{media::AudioPlaybackFailureCode::none};
    bool hasTargetTimelineFrame{};
    std::int64_t targetTimelineFrame{};
    bool hasSourcePresentationTimestamp{};
    std::int64_t sourcePresentationTimestamp{};
    bool hasCachedFrame{};
    std::uint64_t renderSerial{};
    std::uint64_t presentSerial{};
};

namespace detail {

class PreviewPlaybackPort {
public:
    virtual ~PreviewPlaybackPort() = default;

    virtual media::HeadlessAvPlaybackReceipt play(
        const std::filesystem::path& input,
        std::int64_t timelineFrame,
        audio::FrameRate timelineFrameRate,
        std::stop_token cancellation
    ) = 0;
    virtual media::HeadlessAvPlaybackReceipt tick(
        std::uint64_t expectedGeneration,
        std::stop_token cancellation
    ) = 0;
    virtual media::HeadlessAvPlaybackReceipt cancel(
        std::uint64_t expectedGeneration
    ) = 0;
    virtual media::HeadlessAvPlaybackReceipt snapshot() const = 0;
    virtual media::HeadlessAvPlaybackReceipt close() = 0;
};

class PreviewRenderPort {
public:
    virtual ~PreviewRenderPort() = default;

    virtual render::RenderedFrame render(
        const media::PresentedVideoFrame& frame,
        std::int64_t targetTimelineFrame,
        const PreviewPresentationSettings& settings,
        std::stop_token cancellation
    ) = 0;
};

class PreviewSurfacePort {
public:
    virtual ~PreviewSurfacePort() = default;

    virtual render::D3d11PreviewSurfaceReceipt resize(
        std::uint32_t width,
        std::uint32_t height,
        std::stop_token cancellation
    ) = 0;
    virtual render::D3d11PreviewSurfaceReceipt present(
        const render::RenderedFrame& frame,
        std::stop_token cancellation
    ) = 0;
    virtual render::D3d11PreviewSurfaceReceipt clear(
        std::stop_token cancellation
    ) = 0;
    virtual render::D3d11PreviewSurfaceReceipt snapshot() const = 0;
    virtual render::D3d11PreviewSurfaceReceipt close() = 0;
};

struct PreviewPresentationActiveOperation final {
    std::stop_source cancellation;
    std::uint64_t admittedGeneration{};
};

class PreviewPresentationSessionTestAccess;

}

class PreviewPresentationSession final {
public:
    PreviewPresentationSession(
        HWND window,
        render::D3d11PreviewDriver driver = render::D3d11PreviewDriver::hardware,
        PreviewPresentationLimits limits = {}
    );
    ~PreviewPresentationSession();

    PreviewPresentationSession(const PreviewPresentationSession&) = delete;
    PreviewPresentationSession& operator=(const PreviewPresentationSession&) = delete;
    PreviewPresentationSession(PreviewPresentationSession&&) = delete;
    PreviewPresentationSession& operator=(PreviewPresentationSession&&) = delete;

    PreviewPresentationReceipt play(
        const std::filesystem::path& input,
        std::int64_t timelineFrame,
        audio::FrameRate timelineFrameRate,
        PreviewPresentationSettings settings,
        std::stop_token cancellation = {}
    );
    PreviewPresentationReceipt tick(
        std::uint64_t expectedGeneration,
        std::stop_token cancellation = {}
    );
    PreviewPresentationReceipt resize(
        std::uint32_t width,
        std::uint32_t height,
        std::stop_token cancellation = {}
    );
    PreviewPresentationReceipt cancel(std::uint64_t expectedGeneration);
    PreviewPresentationReceipt snapshot() const;
    PreviewPresentationReceipt close();

private:
    friend class detail::PreviewPresentationSessionTestAccess;

    PreviewPresentationSession(
        std::unique_ptr<detail::PreviewPlaybackPort> playback,
        std::unique_ptr<detail::PreviewRenderPort> renderer,
        std::unique_ptr<detail::PreviewSurfacePort> surface
    );

    PreviewPresentationReceipt receipt(
        PreviewPresentationOutcome outcome,
        PreviewPresentationStage stage = PreviewPresentationStage::none
    ) const;
    PreviewPresentationReceipt refused(
        PreviewPresentationStage stage,
        PreviewPresentationFailureCode failure,
        HRESULT hresult
    ) const;
    std::shared_ptr<detail::PreviewPresentationActiveOperation> beginOperation(
        std::uint64_t admittedGeneration
    );
    void finishOperation(
        const std::shared_ptr<detail::PreviewPresentationActiveOperation>& operation
    );
    void clearFrameState();

    mutable std::mutex operationMutex_;
    mutable std::mutex lifecycleMutex_;
    std::unique_ptr<detail::PreviewPlaybackPort> playback_;
    std::unique_ptr<detail::PreviewRenderPort> renderer_;
    std::unique_ptr<detail::PreviewSurfacePort> surface_;
    std::uint64_t generation_{};
    PreviewPresentationState state_{PreviewPresentationState::idle};
    std::optional<PreviewPresentationSettings> settings_;
    std::optional<media::PresentedVideoFrame> cachedFrame_;
    std::optional<std::int64_t> targetTimelineFrame_;
    std::optional<render::RenderedFrame> pendingRenderedFrame_;
    std::optional<std::int64_t> pendingRenderedSourceTimestamp_;
    std::optional<std::int64_t> pendingRenderedTargetFrame_;
    std::optional<render::D3d11PreviewSurfaceReceipt> terminalSurfaceReceipt_;
    std::uint64_t renderSerial_{};
    std::uint64_t presentSerial_{};
    bool presentationDirty_{};
    bool closeRequested_{};
    std::optional<PreviewPresentationReceipt> closeReceipt_;
    std::shared_ptr<detail::PreviewPresentationActiveOperation> activeOperation_;
};

}
