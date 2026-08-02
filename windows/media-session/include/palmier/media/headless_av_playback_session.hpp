#pragma once

#include "palmier/media/audio_playback_session.hpp"
#include "palmier/media/presentation_video_decode_pump.hpp"

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>

namespace palmier::media {

namespace detail {

struct HeadlessAvPlaybackActiveOperation final {
    std::stop_source cancellation;
    std::uint64_t admittedGeneration{};
};

class HeadlessAvPlaybackAudioPort {
public:
    virtual ~HeadlessAvPlaybackAudioPort() = default;

    virtual AudioPlaybackReceipt playExactGeneration(
        const std::filesystem::path& input,
        std::int64_t timelineFrame,
        std::optional<DecodeFrameStart> decodeStart,
        std::uint64_t generation,
        std::stop_token cancellation
    ) = 0;
    virtual AudioPlaybackPositionReceipt position(
        std::uint64_t generation
    ) const = 0;
    virtual AudioPlaybackReceipt cancel(std::uint64_t generation) = 0;
    virtual AudioPlaybackReceipt snapshot() const = 0;
    virtual AudioPlaybackReceipt close() = 0;
};

class HeadlessAvPlaybackSessionTestAccess;

}

struct HeadlessAvPlaybackLimits final {
    PresentationVideoDecodeLimits video;
    std::size_t maximumVideoFillCallsPerTick{2};
};

enum class HeadlessAvPlaybackState {
    idle,
    playing,
    completed,
    cancelled,
    invalidated,
    failed,
    closed,
};

enum class HeadlessAvPlaybackOutcome {
    changed,
    noOp,
    stale,
    cancelled,
    refused,
    failed,
    invalidated,
};

enum class HeadlessAvPlaybackStage {
    none,
    prepareVideo,
    startAudio,
    commitVideo,
    audioPosition,
    fillVideo,
    selectVideo,
    cancel,
    close,
};

enum class HeadlessAvPlaybackFailureCode {
    none,
    invalidRequest,
    generationExhausted,
    audioFailure,
    videoFailure,
    invariantFailure,
};

struct HeadlessAvPlaybackReceipt final {
    std::uint64_t generation{};
    HeadlessAvPlaybackState state{HeadlessAvPlaybackState::idle};
    HeadlessAvPlaybackOutcome outcome{HeadlessAvPlaybackOutcome::noOp};
    HeadlessAvPlaybackStage stage{HeadlessAvPlaybackStage::none};
    HeadlessAvPlaybackFailureCode failure{HeadlessAvPlaybackFailureCode::none};
    HRESULT hresult{S_OK};
    std::int32_t mediaFailureCode{-1};
    AudioPlaybackFailureCode audioFailure{AudioPlaybackFailureCode::none};
    AudioPlaybackState audioState{AudioPlaybackState::idle};
    PresentationVideoDecodeState videoState{PresentationVideoDecodeState::idle};
    std::size_t fillCalls{};
    std::size_t admittedFrames{};
    std::size_t droppedFrames{};
    bool fillBudgetExhausted{};
    bool hasTargetTimelineFrame{};
    std::int64_t targetTimelineFrame{};
    std::optional<PresentedVideoFrame> frame;
};

class HeadlessAvPlaybackSession final {
public:
    explicit HeadlessAvPlaybackSession(HeadlessAvPlaybackLimits limits = {});
    ~HeadlessAvPlaybackSession();

    HeadlessAvPlaybackSession(const HeadlessAvPlaybackSession&) = delete;
    HeadlessAvPlaybackSession& operator=(const HeadlessAvPlaybackSession&) = delete;
    HeadlessAvPlaybackSession(HeadlessAvPlaybackSession&&) = delete;
    HeadlessAvPlaybackSession& operator=(HeadlessAvPlaybackSession&&) = delete;

    HeadlessAvPlaybackReceipt play(
        const std::filesystem::path& input,
        std::int64_t timelineFrame,
        audio::FrameRate timelineFrameRate,
        std::stop_token cancellation = {}
    );
    HeadlessAvPlaybackReceipt play(
        const std::filesystem::path& input,
        std::int64_t timelineFrame,
        audio::FrameRate timelineFrameRate,
        DecodeFrameStart start,
        std::stop_token cancellation = {}
    );
    HeadlessAvPlaybackReceipt tick(
        std::uint64_t expectedGeneration,
        std::stop_token cancellation = {}
    );
    HeadlessAvPlaybackReceipt cancel(std::uint64_t expectedGeneration);
    HeadlessAvPlaybackReceipt snapshot() const;
    HeadlessAvPlaybackReceipt close();

private:
    friend class detail::HeadlessAvPlaybackSessionTestAccess;

    HeadlessAvPlaybackSession(
        std::unique_ptr<detail::HeadlessAvPlaybackAudioPort> audio,
        HeadlessAvPlaybackLimits limits
    );

    HeadlessAvPlaybackReceipt baseReceipt(
        HeadlessAvPlaybackOutcome outcome,
        HeadlessAvPlaybackStage stage = HeadlessAvPlaybackStage::none
    ) const;
    HeadlessAvPlaybackReceipt terminateFromAudioPosition(
        const AudioPlaybackPositionReceipt& position,
        HeadlessAvPlaybackStage stage
    );
    HeadlessAvPlaybackReceipt failActive(
        HeadlessAvPlaybackStage stage,
        HeadlessAvPlaybackFailureCode failure,
        HRESULT hresult,
        std::int32_t mediaFailureCode = -1
    );
    HeadlessAvPlaybackReceipt cancelCurrent();
    HeadlessAvPlaybackReceipt playInternal(
        const std::filesystem::path& input,
        std::int64_t timelineFrame,
        audio::FrameRate timelineFrameRate,
        std::optional<DecodeFrameStart> decodeStart,
        std::stop_token cancellation
    );

    mutable std::mutex mutex_;
    mutable std::mutex operationMutex_;
    std::mutex lifecycleMutex_;
    std::unique_ptr<detail::HeadlessAvPlaybackAudioPort> audio_;
    HeadlessAvPlaybackLimits limits_;
    std::unique_ptr<PresentationVideoDecodePump> video_;
    std::filesystem::path input_;
    std::uint64_t generation_{};
    std::int64_t timelineFrame_{};
    audio::FrameRate timelineFrameRate_;
    std::optional<DecodeFrameStart> decodeStart_;
    HeadlessAvPlaybackState state_{HeadlessAvPlaybackState::idle};
    std::optional<HeadlessAvPlaybackReceipt> closeReceipt_;
    bool closeRequested_{};
    std::optional<std::uint64_t> pendingCancellation_;
    std::uint64_t publishedGeneration_{};
    std::shared_ptr<detail::HeadlessAvPlaybackActiveOperation> activeOperation_;
};

}
