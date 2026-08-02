#pragma once

#include "palmier/media/ffmpeg_media_reader.hpp"
#include "palmier/media/presentation_video_buffer.hpp"
#include "palmier/render/render_plan.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>

namespace palmier::media {

namespace detail {
class PresentationVideoDecodePumpTestAccess;
}

struct PresentationVideoDecodeLimits final {
    PresentationVideoBufferLimits buffer;
    DecodeLimits decode{
        render::maximumRenderFramePixels,
        4'096,
        4'096,
        5 * 1024 * 1024,
        5'000'000,
        65'536,
    };
    std::size_t maximumFramesPerFill{4};
};

enum class PresentationVideoDecodeState {
    idle,
    ready,
    blocked,
    endOfStream,
    cancelled,
    failed,
};

enum class PresentationVideoDecodeErrorCode {
    decodeLimitExceedsRenderBudget,
    notStarted,
    terminalState,
    invariantViolation,
    changedInputWithinGeneration,
    invalidFillBudget,
};

class PresentationVideoDecodeError final : public std::runtime_error {
public:
    PresentationVideoDecodeError(
        PresentationVideoDecodeErrorCode code,
        const char* message
    );

    PresentationVideoDecodeErrorCode code;
};

struct PresentationVideoFillReceipt final {
    std::uint64_t generation{};
    PresentationVideoDecodeState state{PresentationVideoDecodeState::idle};
    PresentationVideoOutcome outcome{PresentationVideoOutcome::noOp};
    std::size_t admittedFrames{};
    bool hasPendingFrame{};
    std::size_t queuedFrames{};
    std::uint64_t queuedBytes{};
};

class PresentationVideoDecodePump final {
public:
    explicit PresentationVideoDecodePump(
        PresentationVideoDecodeLimits limits = {}
    );
    PresentationVideoDecodePump(const PresentationVideoDecodePump&) = delete;
    PresentationVideoDecodePump& operator=(const PresentationVideoDecodePump&) = delete;
    PresentationVideoDecodePump(PresentationVideoDecodePump&&) = delete;
    PresentationVideoDecodePump& operator=(PresentationVideoDecodePump&&) = delete;

    PresentationVideoReceipt start(
        std::uint64_t generation,
        const std::filesystem::path& input,
        std::stop_token cancellation = {}
    );
    PresentationVideoReceipt start(
        std::uint64_t generation,
        const std::filesystem::path& input,
        DecodeFrameStart start,
        std::stop_token cancellation = {}
    );
    PresentationVideoFillReceipt fill(
        std::uint64_t generation,
        std::stop_token cancellation = {}
    );
    PresentationVideoTake dequeue(std::uint64_t generation);
    PresentationVideoSelection select(
        std::uint64_t generation,
        std::uint64_t expectedRevision,
        const PresentationVideoClockPosition& clock
    );
    PresentationVideoReceipt cancel(std::uint64_t generation);

    std::uint64_t generation() const noexcept;
    std::uint64_t revision() const noexcept;
    PresentationVideoDecodeState state() const noexcept;

private:
    friend class detail::PresentationVideoDecodePumpTestAccess;

    using StartCommitCheckpoint = std::function<void()>;

    PresentationVideoDecodePump(
        PresentationVideoDecodeLimits limits,
        StartCommitCheckpoint startCommitCheckpoint
    );
    PresentationVideoFillReceipt fillReceipt(
        PresentationVideoOutcome outcome,
        std::size_t admittedFrames
    ) const noexcept;
    PresentationVideoReceipt startInternal(
        std::uint64_t generation,
        const std::filesystem::path& input,
        std::optional<DecodeFrameStart> start,
        std::stop_token cancellation
    );
    void terminate(PresentationVideoDecodeState state);

    PresentationVideoDecodeLimits limits_;
    PresentationVideoBuffer buffer_;
    std::unique_ptr<FfmpegVideoFrameReader> reader_;
    std::optional<DecodedVideoFrame> pendingFrame_;
    std::filesystem::path inputIdentity_;
    std::optional<DecodeFrameStart> decodeStart_;
    StartCommitCheckpoint startCommitCheckpoint_;
    PresentationVideoDecodeState state_{PresentationVideoDecodeState::idle};
};

}
