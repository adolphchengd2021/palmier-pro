#pragma once

#include "palmier/media/ffmpeg_media_reader.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>

namespace palmier::media {

struct PresentationAudioDecodeLimits final {
    audio::PcmFormat targetFormat;
    std::uint32_t capacityFrames{4'096};
    std::uint32_t maximumFramesPerFill{1'024};
    DecodeLimits decode;
};

enum class PresentationAudioDecodeState {
    idle,
    ready,
    blocked,
    endOfStream,
    cancelled,
    failed,
};

enum class PresentationAudioOutcome {
    changed,
    noOp,
    stale,
    refused,
};

enum class PresentationAudioDecodeErrorCode {
    invalidLimits,
    notStarted,
    terminalState,
    changedInputWithinGeneration,
    invariantViolation,
};

class PresentationAudioDecodeError final : public std::runtime_error {
public:
    PresentationAudioDecodeError(
        PresentationAudioDecodeErrorCode code,
        const char* message
    );

    PresentationAudioDecodeErrorCode code;
};

struct PresentationAudioReceipt final {
    std::uint64_t generation{};
    PresentationAudioDecodeState state{PresentationAudioDecodeState::idle};
    PresentationAudioOutcome outcome{PresentationAudioOutcome::noOp};
    std::uint32_t admittedFrames{};
    std::uint32_t queuedFrames{};
    bool hasPendingBlock{};
};

struct PresentationAudioTake final {
    PresentationAudioOutcome outcome{PresentationAudioOutcome::noOp};
    std::optional<DecodedAudioBlock> block;
};

class PresentationAudioDecodePump final {
public:
    explicit PresentationAudioDecodePump(PresentationAudioDecodeLimits limits);
    PresentationAudioDecodePump(const PresentationAudioDecodePump&) = delete;
    PresentationAudioDecodePump& operator=(const PresentationAudioDecodePump&) = delete;
    PresentationAudioDecodePump(PresentationAudioDecodePump&&) = delete;
    PresentationAudioDecodePump& operator=(PresentationAudioDecodePump&&) = delete;

    PresentationAudioReceipt start(
        std::uint64_t generation,
        const std::filesystem::path& input,
        std::stop_token cancellation = {}
    );
    PresentationAudioReceipt start(
        std::uint64_t generation,
        const std::filesystem::path& input,
        DecodeFrameStart start,
        std::stop_token cancellation = {}
    );
    PresentationAudioReceipt fill(
        std::uint64_t generation,
        std::stop_token cancellation = {}
    );
    PresentationAudioTake dequeue(std::uint64_t generation);
    PresentationAudioReceipt cancel(std::uint64_t generation);

    std::uint64_t generation() const noexcept;
    PresentationAudioDecodeState state() const noexcept;

private:
    PresentationAudioReceipt receipt(
        PresentationAudioOutcome outcome,
        std::uint32_t admittedFrames = 0
    ) const noexcept;
    PresentationAudioReceipt startInternal(
        std::uint64_t generation,
        const std::filesystem::path& input,
        std::optional<DecodeFrameStart> start,
        std::stop_token cancellation
    );
    void terminate(PresentationAudioDecodeState state);

    PresentationAudioDecodeLimits limits_;
    std::unique_ptr<FfmpegAudioFrameReader> reader_;
    std::optional<DecodedAudioBlock> pendingBlock_;
    std::size_t pendingFrameOffset_{};
    std::deque<DecodedAudioBlock> queue_;
    std::filesystem::path inputIdentity_;
    std::optional<DecodeFrameStart> decodeStart_;
    std::uint64_t generation_{};
    std::uint32_t queuedFrames_{};
    PresentationAudioDecodeState state_{PresentationAudioDecodeState::idle};
};

}
