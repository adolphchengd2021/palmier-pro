#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <vector>

namespace palmier::media {

struct Rational final {
    std::int32_t numerator{};
    std::int32_t denominator{1};
};

struct DisplayTransform final {
    std::int16_t counterClockwiseDegrees{};
};

struct ColorMetadata final {
    std::int32_t primaries{};
    std::int32_t transfer{};
    std::int32_t matrix{};
    std::int32_t range{};
    std::int32_t chromaLocation{};
};

bool isPrototypeSrgbColor(const ColorMetadata& color) noexcept;

enum class AlphaMode {
    opaque,
    unspecified,
    straight,
    premultiplied,
};

enum class StreamKind {
    video,
    audio,
    other,
};

struct StreamProbe final {
    std::int32_t index{};
    StreamKind kind{StreamKind::other};
    std::string codecName;
    Rational timeBase;
    std::optional<std::int64_t> duration;
    std::int32_t width{};
    std::int32_t height{};
    Rational averageFrameRate;
    Rational realFrameRate;
    std::int32_t sampleRate{};
    std::int32_t channelCount{};
    std::string mediaFormatName;
    std::optional<DisplayTransform> displayTransform;
    ColorMetadata color;
};

struct MediaProbe final {
    std::string containerName;
    std::optional<std::int64_t> durationMicroseconds;
    std::vector<StreamProbe> streams;
};

struct DecodedVideoFrame final {
    std::int32_t width{};
    std::int32_t height{};
    std::size_t rowBytes{};
    std::vector<std::uint8_t> rgba8;
    std::optional<std::int64_t> presentationTimestamp;
    Rational timeBase;
    std::optional<DisplayTransform> displayTransform;
    ColorMetadata color;
    AlphaMode alphaMode{AlphaMode::opaque};
};

struct DecodeLimits final {
    std::uint64_t maximumPixels{67'108'864};
    std::uint32_t maximumPacketsBeforeFrame{4'096};
    std::int64_t maximumProbeBytes{5 * 1024 * 1024};
    std::int64_t maximumAnalyzeMicroseconds{5'000'000};
};

enum class MediaFailureCode {
    invalidLimits,
    unsupportedInputProtocol,
    cancelled,
    openFailed,
    streamInfoFailed,
    noVideoStream,
    decoderUnavailable,
    corruptInput,
    resourceLimitExceeded,
    unsupportedColorMetadata,
    unsupportedDisplayTransform,
    conversionFailed,
};

class MediaError final : public std::runtime_error {
public:
    MediaError(MediaFailureCode code, std::string stage, int ffmpegCode);

    MediaFailureCode code;
    std::string stage;
    int ffmpegCode;
};

struct FfmpegRuntimeInfo final {
    std::string version;
    std::string license;
    std::string configuration;
    bool headersMatchRuntime{};
};

class FfmpegVideoFrameReader final {
public:
    explicit FfmpegVideoFrameReader(
        const std::filesystem::path& input,
        const DecodeLimits& limits = {},
        std::stop_token cancellation = {}
    );
    ~FfmpegVideoFrameReader();

    FfmpegVideoFrameReader(const FfmpegVideoFrameReader&) = delete;
    FfmpegVideoFrameReader& operator=(const FfmpegVideoFrameReader&) = delete;
    FfmpegVideoFrameReader(FfmpegVideoFrameReader&&) = delete;
    FfmpegVideoFrameReader& operator=(FfmpegVideoFrameReader&&) = delete;

    std::optional<DecodedVideoFrame> nextFrame(
        std::stop_token cancellation = {}
    );

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

class FfmpegMediaReader final {
public:
    static FfmpegRuntimeInfo runtimeInfo();

    static MediaProbe probe(
        const std::filesystem::path& input,
        const DecodeLimits& limits = {},
        std::stop_token cancellation = {}
    );

    static DecodedVideoFrame decodeFirstVideoFrame(
        const std::filesystem::path& input,
        const DecodeLimits& limits = {},
        std::stop_token cancellation = {}
    );
};

}
