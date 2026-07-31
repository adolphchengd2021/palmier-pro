#include "palmier/media/ffmpeg_media_reader.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <string_view>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/version.h>
#include <libavformat/avformat.h>
#include <libavformat/version.h>
#include <libavutil/display.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <libavutil/version.h>
#include <libswscale/swscale.h>
#include <libswscale/version.h>
}

namespace palmier::media {
namespace {

template<typename Value, void (*Release)(Value**)>
class FfmpegOwner final {
public:
    explicit FfmpegOwner(Value* value = nullptr) : value_(value) {}
    ~FfmpegOwner() { reset(); }

    FfmpegOwner(const FfmpegOwner&) = delete;
    FfmpegOwner& operator=(const FfmpegOwner&) = delete;

    FfmpegOwner(FfmpegOwner&& other) noexcept
        : value_(std::exchange(other.value_, nullptr)) {}

    FfmpegOwner& operator=(FfmpegOwner&& other) noexcept {
        if (this != &other) {
            reset();
            value_ = std::exchange(other.value_, nullptr);
        }
        return *this;
    }

    Value* get() const { return value_; }
    Value** address() { return &value_; }

    void reset(Value* value = nullptr) {
        if (value_ != nullptr) {
            Release(&value_);
        }
        value_ = value;
    }

private:
    Value* value_{};
};

using CodecOwner = FfmpegOwner<AVCodecContext, avcodec_free_context>;
using FrameOwner = FfmpegOwner<AVFrame, av_frame_free>;
using PacketOwner = FfmpegOwner<AVPacket, av_packet_free>;

class FormatOwner final {
public:
    explicit FormatOwner(AVFormatContext* value) : value_(value) {}
    ~FormatOwner() {
        if (value_ != nullptr) {
            avformat_close_input(&value_);
        }
    }

    FormatOwner(const FormatOwner&) = delete;
    FormatOwner& operator=(const FormatOwner&) = delete;

    FormatOwner(FormatOwner&& other) noexcept
        : value_(std::exchange(other.value_, nullptr)) {}

    FormatOwner& operator=(FormatOwner&& other) noexcept {
        if (this != &other) {
            if (value_ != nullptr) {
                avformat_close_input(&value_);
            }
            value_ = std::exchange(other.value_, nullptr);
        }
        return *this;
    }

    AVFormatContext* get() const { return value_; }
    AVFormatContext** address() { return &value_; }

private:
    AVFormatContext* value_{};
};

class ScaleOwner final {
public:
    explicit ScaleOwner(SwsContext* value) : value_(value) {}
    ~ScaleOwner() { sws_freeContext(value_); }

    ScaleOwner(const ScaleOwner&) = delete;
    ScaleOwner& operator=(const ScaleOwner&) = delete;

    SwsContext* get() const { return value_; }

private:
    SwsContext* value_{};
};

struct InterruptState final {
    std::stop_token cancellation;
};

int interruptRead(void* opaque) noexcept {
    const auto* state = static_cast<const InterruptState*>(opaque);
    return state != nullptr && state->cancellation.stop_requested() ? 1 : 0;
}

std::string ffmpegMessage(int code) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
    if (av_strerror(code, buffer.data(), buffer.size()) < 0) {
        return "unknown FFmpeg error";
    }
    return buffer.data();
}

[[noreturn]] void fail(
    MediaFailureCode code,
    std::string stage,
    int ffmpegCode = 0
) {
    throw MediaError(code, std::move(stage), ffmpegCode);
}

void requireNotCancelled(
    std::stop_token cancellation,
    std::string_view stage
) {
    if (cancellation.stop_requested()) {
        fail(MediaFailureCode::cancelled, std::string(stage), AVERROR_EXIT);
    }
}

std::string pathBytes(const std::filesystem::path& path) {
    const auto encoded = path.u8string();
    return {encoded.begin(), encoded.end()};
}

bool hasUnsupportedProtocol(const std::string& path) {
    const auto startsWithAsciiInsensitive = [&](std::string_view prefix) {
        if (path.size() < prefix.size()) {
            return false;
        }
        for (std::size_t index = 0; index < prefix.size(); ++index) {
            const char lhs = path[index] >= 'a' && path[index] <= 'z'
                ? static_cast<char>(path[index] - ('a' - 'A'))
                : path[index];
            const char rhs = prefix[index] >= 'a' && prefix[index] <= 'z'
                ? static_cast<char>(prefix[index] - ('a' - 'A'))
                : prefix[index];
            if (lhs != rhs) {
                return false;
            }
        }
        return true;
    };
    if (path.starts_with(R"(\\.\)")) {
        return true;
    }
    if (path.starts_with(R"(\\?\)")) {
        const bool extendedDrive = path.size() >= 7
            && ((path[4] >= 'A' && path[4] <= 'Z')
                || (path[4] >= 'a' && path[4] <= 'z'))
            && path[5] == ':'
            && (path[6] == '\\' || path[6] == '/');
        return !extendedDrive
            && !startsWithAsciiInsensitive(R"(\\?\UNC\)");
    }
    const auto colon = path.find(':');
    if (colon == std::string::npos) {
        return false;
    }
    const bool driveLetter = colon == 1
        && ((path[0] >= 'A' && path[0] <= 'Z')
            || (path[0] >= 'a' && path[0] <= 'z'));
    return !driveLetter;
}

Rational rational(AVRational value) {
    return {value.num, value.den};
}

ColorMetadata colorMetadata(const AVCodecParameters& parameters) {
    return {
        static_cast<std::int32_t>(parameters.color_primaries),
        static_cast<std::int32_t>(parameters.color_trc),
        static_cast<std::int32_t>(parameters.color_space),
        static_cast<std::int32_t>(parameters.color_range),
        static_cast<std::int32_t>(parameters.chroma_location),
    };
}

ColorMetadata colorMetadata(const AVFrame& frame) {
    return {
        static_cast<std::int32_t>(frame.color_primaries),
        static_cast<std::int32_t>(frame.color_trc),
        static_cast<std::int32_t>(frame.colorspace),
        static_cast<std::int32_t>(frame.color_range),
        static_cast<std::int32_t>(frame.chroma_location),
    };
}

AlphaMode alphaMode(const AVFrame& frame, const AVCodecParameters& parameters) {
    const auto* descriptor = av_pix_fmt_desc_get(
        static_cast<AVPixelFormat>(frame.format)
    );
    if (descriptor == nullptr || (descriptor->flags & AV_PIX_FMT_FLAG_ALPHA) == 0) {
        return AlphaMode::opaque;
    }
    const AVAlphaMode mode = frame.alpha_mode != AVALPHA_MODE_UNSPECIFIED
        ? frame.alpha_mode
        : parameters.alpha_mode;
    if (mode == AVALPHA_MODE_STRAIGHT) {
        return AlphaMode::straight;
    }
    if (mode == AVALPHA_MODE_PREMULTIPLIED) {
        return AlphaMode::premultiplied;
    }
    return AlphaMode::unspecified;
}

bool matrixNear(const std::int32_t* lhs, const std::int32_t* rhs) {
    for (std::size_t index = 0; index < 9; ++index) {
        const auto difference = static_cast<std::int64_t>(lhs[index]) - rhs[index];
        if (std::abs(difference) > 1) {
            return false;
        }
    }
    return true;
}

std::optional<DisplayTransform> displayTransform(const std::int32_t* matrix) {
    if (matrix == nullptr) {
        return std::nullopt;
    }

    const double angle = av_display_rotation_get(matrix);
    if (!std::isfinite(angle)) {
        fail(MediaFailureCode::unsupportedDisplayTransform, "display-matrix");
    }

    const auto rounded = static_cast<int>(std::lround(angle));
    const std::array<int, 5> candidates{0, 90, -90, 180, -180};
    for (const int candidate : candidates) {
        if (rounded != candidate) {
            continue;
        }
        std::array<std::int32_t, 9> expected{};
        av_display_rotation_set(expected.data(), -static_cast<double>(candidate));
        if (!matrixNear(matrix, expected.data())) {
            fail(
                MediaFailureCode::unsupportedDisplayTransform,
                "display-matrix"
            );
        }
        const int normalized = candidate == -180 ? 180 : candidate;
        return DisplayTransform{static_cast<std::int16_t>(normalized)};
    }

    fail(MediaFailureCode::unsupportedDisplayTransform, "display-matrix");
}

std::optional<DisplayTransform> displayTransform(
    const AVCodecParameters& parameters
) {
    const auto* sideData = av_packet_side_data_get(
        parameters.coded_side_data,
        parameters.nb_coded_side_data,
        AV_PKT_DATA_DISPLAYMATRIX
    );
    if (sideData == nullptr || sideData->size < 9 * sizeof(std::int32_t)) {
        return std::nullopt;
    }
    return displayTransform(reinterpret_cast<const std::int32_t*>(sideData->data));
}

std::optional<DisplayTransform> displayTransform(
    AVFrame& frame,
    const AVCodecParameters& parameters
) {
    if (const auto* sideData = av_frame_get_side_data(
            &frame,
            AV_FRAME_DATA_DISPLAYMATRIX
        ); sideData != nullptr && sideData->size >= 9 * sizeof(std::int32_t)) {
        return displayTransform(
            reinterpret_cast<const std::int32_t*>(sideData->data)
        );
    }
    return displayTransform(parameters);
}

AVPixelFormat chooseSoftwarePixelFormat(
    AVCodecContext*,
    const AVPixelFormat* formats
) {
    for (const AVPixelFormat* format = formats;
         format != nullptr && *format != AV_PIX_FMT_NONE;
         ++format) {
        const auto* descriptor = av_pix_fmt_desc_get(*format);
        if (descriptor != nullptr
            && (descriptor->flags & AV_PIX_FMT_FLAG_HWACCEL) == 0) {
            return *format;
        }
    }
    return AV_PIX_FMT_NONE;
}

FormatOwner openInput(
    const std::filesystem::path& input,
    const DecodeLimits& limits,
    InterruptState& interrupt
) {
    if (limits.maximumPixels == 0
        || limits.maximumPixels > static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max()
        )
        || limits.maximumPacketsBeforeFrame == 0
        || limits.maximumProbeBytes <= 0
        || limits.maximumAnalyzeMicroseconds <= 0) {
        fail(MediaFailureCode::invalidLimits, "validate-limits");
    }
    requireNotCancelled(interrupt.cancellation, "before-open");
    AVFormatContext* raw = avformat_alloc_context();
    if (raw == nullptr) {
        fail(MediaFailureCode::resourceLimitExceeded, "allocate-format");
    }
    FormatOwner format(raw);
    raw->interrupt_callback = AVIOInterruptCB{interruptRead, &interrupt};
    raw->probesize = limits.maximumProbeBytes;
    raw->max_analyze_duration = limits.maximumAnalyzeMicroseconds;

    const std::string inputBytes = pathBytes(input);
    if (hasUnsupportedProtocol(inputBytes)) {
        fail(MediaFailureCode::unsupportedInputProtocol, "validate-input");
    }
    const int openResult = avformat_open_input(
        format.address(),
        inputBytes.c_str(),
        nullptr,
        nullptr
    );
    if (openResult < 0) {
        if (interrupt.cancellation.stop_requested() || openResult == AVERROR_EXIT) {
            fail(MediaFailureCode::cancelled, "open", openResult);
        }
        fail(MediaFailureCode::openFailed, "open", openResult);
    }

    const int streamResult = avformat_find_stream_info(format.get(), nullptr);
    if (streamResult < 0) {
        if (interrupt.cancellation.stop_requested() || streamResult == AVERROR_EXIT) {
            fail(MediaFailureCode::cancelled, "stream-info", streamResult);
        }
        fail(MediaFailureCode::streamInfoFailed, "stream-info", streamResult);
    }
    requireNotCancelled(interrupt.cancellation, "after-stream-info");
    return format;
}

StreamKind streamKind(AVMediaType type) {
    if (type == AVMEDIA_TYPE_VIDEO) {
        return StreamKind::video;
    }
    if (type == AVMEDIA_TYPE_AUDIO) {
        return StreamKind::audio;
    }
    return StreamKind::other;
}

StreamProbe probeStream(const AVStream& stream, std::int32_t index) {
    const AVCodecParameters& parameters = *stream.codecpar;
    StreamProbe result;
    result.index = index;
    result.kind = streamKind(parameters.codec_type);
    result.codecName = avcodec_get_name(parameters.codec_id);
    result.timeBase = rational(stream.time_base);
    if (stream.duration != AV_NOPTS_VALUE) {
        result.duration = stream.duration;
    }
    result.width = parameters.width;
    result.height = parameters.height;
    result.averageFrameRate = rational(stream.avg_frame_rate);
    result.realFrameRate = rational(stream.r_frame_rate);
    result.sampleRate = parameters.sample_rate;
    result.channelCount = parameters.ch_layout.nb_channels;
    const auto format = static_cast<AVPixelFormat>(parameters.format);
    if (parameters.codec_type == AVMEDIA_TYPE_VIDEO) {
        if (const char* name = av_get_pix_fmt_name(format); name != nullptr) {
            result.mediaFormatName = name;
        }
        result.displayTransform = displayTransform(parameters);
    } else if (parameters.codec_type == AVMEDIA_TYPE_AUDIO) {
        if (const char* name = av_get_sample_fmt_name(
                static_cast<AVSampleFormat>(parameters.format)
            ); name != nullptr) {
            result.mediaFormatName = name;
        }
    }
    result.color = colorMetadata(parameters);
    return result;
}

bool supportedPrototypeColor(const AVFrame& frame, const ColorMetadata& color) {
    const auto* descriptor = av_pix_fmt_desc_get(
        static_cast<AVPixelFormat>(frame.format)
    );
    return isPrototypeSrgbColor(color)
        && descriptor != nullptr
        && (descriptor->flags & AV_PIX_FMT_FLAG_RGB) != 0;
}

std::size_t checkedFrameBytes(
    std::int32_t width,
    std::int32_t height,
    std::uint64_t maximumPixels
) {
    if (width <= 0 || height <= 0) {
        fail(MediaFailureCode::corruptInput, "frame-dimensions");
    }
    const auto pixels = static_cast<std::uint64_t>(width)
        * static_cast<std::uint64_t>(height);
    if (pixels > maximumPixels
        || width > std::numeric_limits<int>::max() / 4
        || pixels > std::numeric_limits<std::size_t>::max() / 4) {
        fail(MediaFailureCode::resourceLimitExceeded, "frame-pixels");
    }
    return static_cast<std::size_t>(pixels) * 4;
}

DecodedVideoFrame convertFrame(
    AVFrame& frame,
    const AVStream& stream,
    const DecodeLimits& limits,
    std::stop_token cancellation
) {
    requireNotCancelled(cancellation, "before-convert");
    const ColorMetadata color = colorMetadata(frame);
    if (!supportedPrototypeColor(frame, color)) {
        fail(MediaFailureCode::unsupportedColorMetadata, "frame-color");
    }

    const std::size_t byteCount = checkedFrameBytes(
        frame.width,
        frame.height,
        limits.maximumPixels
    );
    const auto rowBytes = static_cast<std::size_t>(frame.width) * 4;
    std::vector<std::uint8_t> pixels(byteCount);

    ScaleOwner scale(sws_getContext(
        frame.width,
        frame.height,
        static_cast<AVPixelFormat>(frame.format),
        frame.width,
        frame.height,
        AV_PIX_FMT_RGBA,
        SWS_POINT,
        nullptr,
        nullptr,
        nullptr
    ));
    if (scale.get() == nullptr) {
        fail(MediaFailureCode::conversionFailed, "create-converter");
    }

    std::array<std::uint8_t*, 4> destination{pixels.data(), nullptr, nullptr, nullptr};
    std::array<int, 4> destinationStride{
        static_cast<int>(rowBytes),
        0,
        0,
        0,
    };
    const int converted = sws_scale(
        scale.get(),
        frame.data,
        frame.linesize,
        0,
        frame.height,
        destination.data(),
        destinationStride.data()
    );
    if (converted != frame.height) {
        fail(MediaFailureCode::conversionFailed, "convert-frame", converted);
    }

    requireNotCancelled(cancellation, "after-convert");
    DecodedVideoFrame result;
    result.width = frame.width;
    result.height = frame.height;
    result.rowBytes = rowBytes;
    result.rgba8 = std::move(pixels);
    if (frame.best_effort_timestamp != AV_NOPTS_VALUE) {
        result.presentationTimestamp = frame.best_effort_timestamp;
    }
    result.timeBase = rational(stream.time_base);
    result.displayTransform = displayTransform(frame, *stream.codecpar);
    result.color = color;
    result.alphaMode = alphaMode(frame, *stream.codecpar);
    return result;
}

}

bool isPrototypeSrgbColor(const ColorMetadata& color) noexcept {
    return color.primaries == AVCOL_PRI_BT709
        && color.transfer == AVCOL_TRC_IEC61966_2_1
        && color.matrix == AVCOL_SPC_RGB
        && (color.range == AVCOL_RANGE_JPEG
            || color.range == AVCOL_RANGE_UNSPECIFIED);
}

MediaError::MediaError(
    MediaFailureCode codeValue,
    std::string stageValue,
    int ffmpegCodeValue
)
    : std::runtime_error(
          stageValue
          + (ffmpegCodeValue < 0 ? ": " + ffmpegMessage(ffmpegCodeValue) : "")
      ),
      code(codeValue),
      stage(std::move(stageValue)),
      ffmpegCode(ffmpegCodeValue) {}

FfmpegRuntimeInfo FfmpegMediaReader::runtimeInfo() {
    const bool headersMatch = avutil_version() == LIBAVUTIL_VERSION_INT
        && avcodec_version() == LIBAVCODEC_VERSION_INT
        && avformat_version() == LIBAVFORMAT_VERSION_INT
        && swscale_version() == LIBSWSCALE_VERSION_INT;
    return {
        av_version_info(),
        avcodec_license(),
        avcodec_configuration(),
        headersMatch,
    };
}

MediaProbe FfmpegMediaReader::probe(
    const std::filesystem::path& input,
    const DecodeLimits& limits,
    std::stop_token cancellation
) {
    InterruptState interrupt{cancellation};
    auto format = openInput(input, limits, interrupt);
    MediaProbe result;
    result.containerName = format.get()->iformat->name;
    if (format.get()->duration != AV_NOPTS_VALUE) {
        result.durationMicroseconds = format.get()->duration;
    }
    result.streams.reserve(format.get()->nb_streams);
    for (unsigned int index = 0; index < format.get()->nb_streams; ++index) {
        requireNotCancelled(cancellation, "probe-stream");
        result.streams.push_back(probeStream(
            *format.get()->streams[index],
            static_cast<std::int32_t>(index)
        ));
    }
    requireNotCancelled(cancellation, "before-probe-return");
    return result;
}

DecodedVideoFrame FfmpegMediaReader::decodeFirstVideoFrame(
    const std::filesystem::path& input,
    const DecodeLimits& limits,
    std::stop_token cancellation
) {
    InterruptState interrupt{cancellation};
    auto format = openInput(input, limits, interrupt);
    const int videoIndex = av_find_best_stream(
        format.get(),
        AVMEDIA_TYPE_VIDEO,
        -1,
        -1,
        nullptr,
        0
    );
    if (videoIndex < 0) {
        fail(MediaFailureCode::noVideoStream, "find-video", videoIndex);
    }

    AVStream& stream = *format.get()->streams[videoIndex];
    const AVCodec* decoder = avcodec_find_decoder(stream.codecpar->codec_id);
    if (decoder == nullptr) {
        fail(MediaFailureCode::decoderUnavailable, "find-decoder");
    }
    CodecOwner codec(avcodec_alloc_context3(decoder));
    if (codec.get() == nullptr) {
        fail(MediaFailureCode::resourceLimitExceeded, "allocate-decoder");
    }
    int result = avcodec_parameters_to_context(codec.get(), stream.codecpar);
    if (result < 0) {
        fail(MediaFailureCode::corruptInput, "copy-codec-parameters", result);
    }
    checkedFrameBytes(
        stream.codecpar->width,
        stream.codecpar->height,
        limits.maximumPixels
    );
    codec.get()->max_pixels = static_cast<std::int64_t>(limits.maximumPixels);
    codec.get()->get_format = chooseSoftwarePixelFormat;
    codec.get()->thread_count = 1;
    result = avcodec_open2(codec.get(), decoder, nullptr);
    if (result < 0) {
        fail(MediaFailureCode::decoderUnavailable, "open-decoder", result);
    }

    PacketOwner packet(av_packet_alloc());
    FrameOwner frame(av_frame_alloc());
    if (packet.get() == nullptr || frame.get() == nullptr) {
        fail(MediaFailureCode::resourceLimitExceeded, "allocate-decode-buffer");
    }

    const auto receive = [&]() -> std::optional<DecodedVideoFrame> {
        for (;;) {
            requireNotCancelled(cancellation, "receive-frame");
            const int receiveResult = avcodec_receive_frame(codec.get(), frame.get());
            if (receiveResult == AVERROR(EAGAIN) || receiveResult == AVERROR_EOF) {
                return std::nullopt;
            }
            if (receiveResult < 0) {
                fail(MediaFailureCode::corruptInput, "receive-frame", receiveResult);
            }
            if ((frame.get()->flags & AV_FRAME_FLAG_CORRUPT) != 0
                || frame.get()->decode_error_flags != 0) {
                fail(MediaFailureCode::corruptInput, "corrupt-frame");
            }
            return convertFrame(*frame.get(), stream, limits, cancellation);
        }
    };

    std::uint32_t packetCount = 0;
    for (;;) {
        requireNotCancelled(cancellation, "read-packet");
        const int readResult = av_read_frame(format.get(), packet.get());
        if (readResult == AVERROR_EOF) {
            break;
        }
        if (readResult < 0) {
            if (cancellation.stop_requested() || readResult == AVERROR_EXIT) {
                fail(MediaFailureCode::cancelled, "read-packet", readResult);
            }
            fail(MediaFailureCode::corruptInput, "read-packet", readResult);
        }
        ++packetCount;
        if (packetCount > limits.maximumPacketsBeforeFrame) {
            fail(MediaFailureCode::resourceLimitExceeded, "packet-budget");
        }
        if (packet.get()->stream_index == videoIndex) {
            const int sendResult = avcodec_send_packet(codec.get(), packet.get());
            av_packet_unref(packet.get());
            if (sendResult < 0) {
                fail(MediaFailureCode::corruptInput, "send-packet", sendResult);
            }
            if (auto decoded = receive()) {
                return std::move(*decoded);
            }
        } else {
            av_packet_unref(packet.get());
        }
    }

    result = avcodec_send_packet(codec.get(), nullptr);
    if (result < 0 && result != AVERROR_EOF) {
        fail(MediaFailureCode::corruptInput, "drain-decoder", result);
    }
    if (auto decoded = receive()) {
        return std::move(*decoded);
    }
    fail(MediaFailureCode::corruptInput, "missing-decoded-frame");
}

}
