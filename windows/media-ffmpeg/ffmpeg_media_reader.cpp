#include "palmier/media/ffmpeg_media_reader.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
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
#include <libavutil/mathematics.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
#include <libavutil/version.h>
#include <libswresample/swresample.h>
#include <libswresample/version.h>
#include <libswscale/swscale.h>
#include <libswscale/version.h>
}

namespace palmier::media {
namespace {

constexpr std::uint32_t maximumConfigurableAudioFramesPerBlock = 65'536;

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
    explicit ScaleOwner(SwsContext* value = nullptr) : value_(value) {}
    ~ScaleOwner() { sws_freeContext(value_); }

    ScaleOwner(const ScaleOwner&) = delete;
    ScaleOwner& operator=(const ScaleOwner&) = delete;

    SwsContext* get() const { return value_; }
    void replaceCached(SwsContext* value) noexcept { value_ = value; }

    bool configurationMatches(
        std::int32_t width,
        std::int32_t height,
        AVPixelFormat format,
        const ColorMetadata& color
    ) const noexcept {
        return configured_
            && width_ == width
            && height_ == height
            && format_ == format
            && color_.primaries == color.primaries
            && color_.transfer == color.transfer
            && color_.matrix == color.matrix
            && color_.range == color.range
            && color_.chromaLocation == color.chromaLocation;
    }

    void recordConfiguration(
        std::int32_t width,
        std::int32_t height,
        AVPixelFormat format,
        const ColorMetadata& color
    ) noexcept {
        configured_ = true;
        width_ = width;
        height_ = height;
        format_ = format;
        color_ = color;
    }

private:
    SwsContext* value_{};
    bool configured_{};
    std::int32_t width_{};
    std::int32_t height_{};
    AVPixelFormat format_{AV_PIX_FMT_NONE};
    ColorMetadata color_{};
};

using ResampleOwner = FfmpegOwner<SwrContext, swr_free>;

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
        || limits.maximumAnalyzeMicroseconds <= 0
        || limits.maximumAudioFramesPerBlock == 0
        || limits.maximumAudioFramesPerBlock
            > maximumConfigurableAudioFramesPerBlock) {
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

enum class DecodeColorMode {
    srgbRgb,
    bt709Video,
};

std::optional<DecodeColorMode> decodeColorMode(
    const AVFrame& frame,
    const ColorMetadata& color
) {
    const auto* descriptor = av_pix_fmt_desc_get(
        static_cast<AVPixelFormat>(frame.format)
    );
    if (descriptor == nullptr) {
        return std::nullopt;
    }
    if ((descriptor->flags & AV_PIX_FMT_FLAG_RGB) != 0) {
        return isPrototypeSrgbColor(color)
            ? std::optional{DecodeColorMode::srgbRgb}
            : std::nullopt;
    }
    return isPrototypeBt709VideoColor(color)
        ? std::optional{DecodeColorMode::bt709Video}
        : std::nullopt;
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
    ScaleOwner& scale,
    std::stop_token cancellation
) {
    requireNotCancelled(cancellation, "before-convert");
    const ColorMetadata color = colorMetadata(frame);
    const auto colorMode = decodeColorMode(frame, color);
    if (!colorMode.has_value()) {
        fail(MediaFailureCode::unsupportedColorMetadata, "frame-color");
    }

    const std::size_t byteCount = checkedFrameBytes(
        frame.width,
        frame.height,
        limits.maximumPixels
    );
    const auto rowBytes = static_cast<std::size_t>(frame.width) * 4;
    std::vector<std::uint8_t> pixels(byteCount);

    const auto sourceFormat = static_cast<AVPixelFormat>(frame.format);
    const bool configureScale = !scale.configurationMatches(
        frame.width,
        frame.height,
        sourceFormat,
        color
    );
    auto* cachedScale = sws_getCachedContext(
        scale.get(),
        frame.width,
        frame.height,
        sourceFormat,
        frame.width,
        frame.height,
        AV_PIX_FMT_RGBA,
        SWS_POINT,
        nullptr,
        nullptr,
        nullptr
    );
    scale.replaceCached(cachedScale);
    if (scale.get() == nullptr) {
        fail(MediaFailureCode::conversionFailed, "create-converter");
    }
    if (configureScale && *colorMode == DecodeColorMode::bt709Video) {
        const int* coefficients = sws_getCoefficients(SWS_CS_ITU709);
        const int colorResult = sws_setColorspaceDetails(
            scale.get(),
            coefficients,
            0,
            coefficients,
            1,
            0,
            1 << 16,
            1 << 16
        );
        if (colorResult < 0) {
            fail(
                MediaFailureCode::conversionFailed,
                "configure-converter",
                colorResult
            );
        }
    }
    if (configureScale) {
        scale.recordConfiguration(frame.width, frame.height, sourceFormat, color);
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

namespace {

class SoftwareFrameReader final {
public:
    SoftwareFrameReader(
        const std::filesystem::path& input,
        DecodeLimits limits,
        AVMediaType mediaType,
        MediaFailureCode missingStreamCode,
        std::stop_token cancellation
    ) : limits_(limits), interrupt_{cancellation}, format_(nullptr) {
        format_ = openInput(input, limits_, interrupt_);
        streamIndex_ = av_find_best_stream(
            format_.get(),
            mediaType,
            -1,
            -1,
            nullptr,
            0
        );
        if (streamIndex_ < 0) {
            fail(missingStreamCode, "find-stream", streamIndex_);
        }

        stream_ = format_.get()->streams[streamIndex_];
        const AVCodec* decoder = avcodec_find_decoder(stream_->codecpar->codec_id);
        if (decoder == nullptr) {
            fail(MediaFailureCode::decoderUnavailable, "find-decoder");
        }
        codec_.reset(avcodec_alloc_context3(decoder));
        if (codec_.get() == nullptr) {
            fail(MediaFailureCode::resourceLimitExceeded, "allocate-decoder");
        }
        int result = avcodec_parameters_to_context(codec_.get(), stream_->codecpar);
        if (result < 0) {
            fail(MediaFailureCode::corruptInput, "copy-codec-parameters", result);
        }
        if (mediaType == AVMEDIA_TYPE_VIDEO) {
            checkedFrameBytes(
                stream_->codecpar->width,
                stream_->codecpar->height,
                limits_.maximumPixels
            );
            codec_.get()->max_pixels = static_cast<std::int64_t>(
                limits_.maximumPixels
            );
            codec_.get()->get_format = chooseSoftwarePixelFormat;
        }
        codec_.get()->thread_count = 1;
        result = avcodec_open2(codec_.get(), decoder, nullptr);
        if (result < 0) {
            fail(MediaFailureCode::decoderUnavailable, "open-decoder", result);
        }

        packet_.reset(av_packet_alloc());
        frame_.reset(av_frame_alloc());
        if (packet_.get() == nullptr || frame_.get() == nullptr) {
            fail(MediaFailureCode::resourceLimitExceeded, "allocate-decode-buffer");
        }
        requireNotCancelled(cancellation, "after-decoder-setup");
    }

    AVFrame* nextFrame(std::stop_token cancellation) {
        if (terminalError_ != nullptr) {
            std::rethrow_exception(terminalError_);
        }
        if (exhausted_) {
            return nullptr;
        }
        interrupt_.cancellation = cancellation;
        av_frame_unref(frame_.get());
        try {
            return decodeNext(cancellation);
        } catch (...) {
            terminalError_ = std::current_exception();
            throw;
        }
    }

    AVStream& stream() const noexcept { return *stream_; }

private:
    AVFrame* decodeNext(std::stop_token cancellation) {
        for (;;) {
            requireNotCancelled(cancellation, "receive-frame");
            const int receiveResult = avcodec_receive_frame(codec_.get(), frame_.get());
            if (receiveResult >= 0) {
                receiveMustProgress_ = false;
                if ((frame_.get()->flags & AV_FRAME_FLAG_CORRUPT) != 0
                    || frame_.get()->decode_error_flags != 0) {
                    fail(MediaFailureCode::corruptInput, "corrupt-frame");
                }
                packetCount_ = 0;
                return frame_.get();
            }
            if (receiveResult == AVERROR_EOF) {
                exhausted_ = true;
                return nullptr;
            }
            if (receiveResult != AVERROR(EAGAIN)) {
                fail(MediaFailureCode::corruptInput, "receive-frame", receiveResult);
            }
            if (receiveMustProgress_) {
                fail(MediaFailureCode::corruptInput, "decoder-no-progress");
            }
            if (drainSent_) {
                fail(MediaFailureCode::corruptInput, "drain-stalled");
            }
            if (packetPending_) {
                sendPendingPacket();
            } else if (drainPending_) {
                sendDrainPacket();
            } else {
                readNextPacket(cancellation);
            }
        }
    }

    void sendPendingPacket() {
        const int result = avcodec_send_packet(codec_.get(), packet_.get());
        if (result >= 0) {
            av_packet_unref(packet_.get());
            packetPending_ = false;
            return;
        }
        if (result == AVERROR(EAGAIN)) {
            receiveMustProgress_ = true;
            return;
        }
        av_packet_unref(packet_.get());
        packetPending_ = false;
        fail(MediaFailureCode::corruptInput, "send-packet", result);
    }

    void sendDrainPacket() {
        const int result = avcodec_send_packet(codec_.get(), nullptr);
        if (result >= 0 || result == AVERROR_EOF) {
            drainPending_ = false;
            drainSent_ = true;
            return;
        }
        if (result == AVERROR(EAGAIN)) {
            receiveMustProgress_ = true;
            return;
        }
        fail(MediaFailureCode::corruptInput, "drain-decoder", result);
    }

    void readNextPacket(std::stop_token cancellation) {
        for (;;) {
            requireNotCancelled(cancellation, "read-packet");
            const int result = av_read_frame(format_.get(), packet_.get());
            if (result == AVERROR_EOF) {
                drainPending_ = true;
                return;
            }
            if (result < 0) {
                if (cancellation.stop_requested() || result == AVERROR_EXIT) {
                    fail(MediaFailureCode::cancelled, "read-packet", result);
                }
                fail(MediaFailureCode::corruptInput, "read-packet", result);
            }
            if (packetCount_ >= limits_.maximumPacketsBeforeFrame) {
                av_packet_unref(packet_.get());
                fail(MediaFailureCode::resourceLimitExceeded, "packet-budget");
            }
            ++packetCount_;
            if (packet_.get()->stream_index == streamIndex_) {
                packetPending_ = true;
                return;
            }
            av_packet_unref(packet_.get());
        }
    }

    DecodeLimits limits_;
    InterruptState interrupt_;
    FormatOwner format_;
    AVStream* stream_{};
    CodecOwner codec_;
    PacketOwner packet_;
    FrameOwner frame_;
    std::exception_ptr terminalError_;
    std::uint32_t packetCount_{};
    int streamIndex_{-1};
    bool packetPending_{};
    bool drainPending_{};
    bool drainSent_{};
    bool receiveMustProgress_{};
    bool exhausted_{};
};

AVSampleFormat sampleFormat(const audio::PcmFormat& format) {
    if (!audio::isValidPcmFormat(format) || !format.interleaved) {
        fail(MediaFailureCode::unsupportedAudioFormat, "target-format");
    }
    if (format.encoding == audio::PcmSampleEncoding::integer
        && format.containerBitsPerSample == 16
        && format.validBitsPerSample == 16) {
        return AV_SAMPLE_FMT_S16;
    }
    if (format.encoding == audio::PcmSampleEncoding::ieeeFloat
        && format.containerBitsPerSample == 32
        && format.validBitsPerSample == 32) {
        return AV_SAMPLE_FMT_FLT;
    }
    fail(MediaFailureCode::unsupportedAudioFormat, "target-format");
}

AVChannelLayout channelLayout(const audio::PcmFormat& format) {
    AVChannelLayout layout{};
    int result = 0;
    if (format.channelMask != 0) {
        result = av_channel_layout_from_mask(&layout, format.channelMask);
    } else {
        av_channel_layout_default(&layout, format.channelCount);
    }
    if (result < 0 || layout.nb_channels != format.channelCount
        || av_channel_layout_check(&layout) == 0) {
        av_channel_layout_uninit(&layout);
        fail(MediaFailureCode::unsupportedAudioFormat, "target-channel-layout", result);
    }
    return layout;
}

std::size_t checkedAudioBytes(
    std::uint32_t frameCount,
    const audio::PcmFormat& format,
    const DecodeLimits& limits
) {
    if (frameCount == 0 || frameCount > limits.maximumAudioFramesPerBlock
        || frameCount > std::numeric_limits<std::size_t>::max()
            / format.blockAlign) {
        fail(MediaFailureCode::resourceLimitExceeded, "audio-block-size");
    }
    return static_cast<std::size_t>(frameCount) * format.blockAlign;
}

std::int64_t checkedAddSamples(
    std::int64_t value,
    std::int64_t delta,
    std::string_view stage
) {
    if (delta < 0 || value > std::numeric_limits<std::int64_t>::max() - delta) {
        fail(MediaFailureCode::resourceLimitExceeded, std::string(stage));
    }
    return value + delta;
}

bool differsByMoreThanOne(std::int64_t lhs, std::int64_t rhs) noexcept {
    if (lhs > rhs) {
        return rhs < std::numeric_limits<std::int64_t>::max()
            && lhs > rhs + 1;
    }
    return lhs < rhs
        && lhs < std::numeric_limits<std::int64_t>::max()
        && rhs > lhs + 1;
}

}

class FfmpegVideoFrameReader::Impl final {
public:
    Impl(
        const std::filesystem::path& input,
        DecodeLimits limits,
        std::stop_token cancellation
    ) : limits_(limits), cursor_(
        input,
        limits,
        AVMEDIA_TYPE_VIDEO,
        MediaFailureCode::noVideoStream,
        cancellation
    ) {}

    std::optional<DecodedVideoFrame> nextFrame(std::stop_token cancellation) {
        if (terminalError_ != nullptr) {
            std::rethrow_exception(terminalError_);
        }
        try {
            AVFrame* frame = cursor_.nextFrame(cancellation);
            if (frame == nullptr) {
                return std::nullopt;
            }
            return convertFrame(
                *frame,
                cursor_.stream(),
                limits_,
                scale_,
                cancellation
            );
        } catch (...) {
            terminalError_ = std::current_exception();
            throw;
        }
    }

private:
    DecodeLimits limits_;
    SoftwareFrameReader cursor_;
    ScaleOwner scale_;
    std::exception_ptr terminalError_;
};

class FfmpegAudioFrameReader::Impl final {
public:
    Impl(
        const std::filesystem::path& input,
        audio::PcmFormat targetFormat,
        DecodeLimits limits,
        std::stop_token cancellation
    ) : targetFormat_(targetFormat),
        targetSampleFormat_(sampleFormat(targetFormat_)),
        limits_(limits),
        cursor_(
            input,
            limits,
            AVMEDIA_TYPE_AUDIO,
            MediaFailureCode::noAudioStream,
            cancellation
        ) {}

    ~Impl() { av_channel_layout_uninit(&sourceLayout_); }

    std::optional<DecodedAudioBlock> nextBlock(std::stop_token cancellation) {
        if (terminalError_ != nullptr) {
            std::rethrow_exception(terminalError_);
        }
        if (exhausted_) {
            return std::nullopt;
        }
        try {
            for (;;) {
                requireNotCancelled(cancellation, "before-audio-decode");
                AVFrame* frame = cursor_.nextFrame(cancellation);
                if (frame == nullptr) {
                    return drain(cancellation);
                }
                auto converted = convert(*frame, cancellation);
                if (converted.frameCount != 0) {
                    return converted;
                }
            }
        } catch (...) {
            terminalError_ = std::current_exception();
            throw;
        }
    }

private:
    void configure(const AVFrame& frame) {
        if (frame.sample_rate <= 0 || frame.nb_samples <= 0
            || frame.format < 0 || frame.ch_layout.nb_channels > 32
            || av_channel_layout_check(&frame.ch_layout) == 0) {
            fail(MediaFailureCode::corruptInput, "audio-frame-format");
        }
        if (sourceConfigured_) {
            if (frame.sample_rate != sourceSampleRate_
                || frame.format != sourceSampleFormat_
                || av_channel_layout_compare(&frame.ch_layout, &sourceLayout_) != 0) {
                fail(MediaFailureCode::unsupportedAudioFormat, "changed-source-format");
            }
            return;
        }

        AVChannelLayout outputLayout = channelLayout(targetFormat_);
        SwrContext* raw = nullptr;
        int result = swr_alloc_set_opts2(
            &raw,
            &outputLayout,
            targetSampleFormat_,
            static_cast<int>(targetFormat_.sampleRate),
            &frame.ch_layout,
            static_cast<AVSampleFormat>(frame.format),
            frame.sample_rate,
            0,
            nullptr
        );
        av_channel_layout_uninit(&outputLayout);
        resampler_.reset(raw);
        if (result < 0 || resampler_.get() == nullptr) {
            fail(MediaFailureCode::conversionFailed, "create-resampler", result);
        }
        result = swr_init(resampler_.get());
        if (result < 0) {
            fail(MediaFailureCode::conversionFailed, "initialize-resampler", result);
        }
        result = av_channel_layout_copy(&sourceLayout_, &frame.ch_layout);
        if (result < 0) {
            fail(MediaFailureCode::resourceLimitExceeded, "copy-source-layout", result);
        }
        sourceSampleRate_ = frame.sample_rate;
        sourceSampleFormat_ = frame.format;
        sourceConfigured_ = true;
    }

    DecodedAudioBlock convert(const AVFrame& frame, std::stop_token cancellation) {
        requireNotCancelled(cancellation, "before-audio-convert");
        configure(frame);
        if (frame.best_effort_timestamp == AV_NOPTS_VALUE) {
            fail(MediaFailureCode::discontinuousAudioTimestamp, "missing-audio-pts");
        }
        if (cursor_.stream().time_base.num <= 0
            || cursor_.stream().time_base.den <= 0) {
            fail(MediaFailureCode::corruptInput, "audio-time-base");
        }
        if (!sourceAnchorTimestamp_.has_value()) {
            sourceAnchorTimestamp_ = frame.best_effort_timestamp;
            nextOutputSample_ = av_rescale_q_rnd(
                frame.best_effort_timestamp,
                cursor_.stream().time_base,
                AVRational{1, static_cast<int>(targetFormat_.sampleRate)},
                static_cast<AVRounding>(AV_ROUND_DOWN | AV_ROUND_PASS_MINMAX)
            );
        } else {
            const auto expectedOffset = av_rescale_q_rnd(
                sourceFramesRead_,
                AVRational{1, sourceSampleRate_},
                cursor_.stream().time_base,
                AV_ROUND_NEAR_INF
            );
            const auto expectedTimestamp = checkedAddSamples(
                *sourceAnchorTimestamp_,
                expectedOffset,
                "source-timestamp-overflow"
            );
            if (differsByMoreThanOne(
                    frame.best_effort_timestamp,
                    expectedTimestamp
                )) {
                fail(
                    MediaFailureCode::discontinuousAudioTimestamp,
                    "audio-pts-discontinuity"
                );
            }
        }
        sourceFramesRead_ = checkedAddSamples(
            sourceFramesRead_,
            frame.nb_samples,
            "source-sample-overflow"
        );

        const int outputCapacity = swr_get_out_samples(
            resampler_.get(),
            frame.nb_samples
        );
        if (outputCapacity <= 0
            || static_cast<std::uint64_t>(outputCapacity)
                > limits_.maximumAudioFramesPerBlock) {
            fail(
                outputCapacity < 0
                    ? MediaFailureCode::conversionFailed
                    : MediaFailureCode::resourceLimitExceeded,
                "audio-output-capacity",
                outputCapacity
            );
        }
        std::vector<std::byte> bytes(checkedAudioBytes(
            static_cast<std::uint32_t>(outputCapacity),
            targetFormat_,
            limits_
        ));
        std::uint8_t* output[] = {
            reinterpret_cast<std::uint8_t*>(bytes.data()),
        };
        std::array<const std::uint8_t*, 32> input{};
        const int pointerCount = av_sample_fmt_is_planar(
            static_cast<AVSampleFormat>(frame.format)
        ) != 0 ? frame.ch_layout.nb_channels : 1;
        for (int index = 0; index < pointerCount; ++index) {
            input[static_cast<std::size_t>(index)] = frame.extended_data[index];
        }
        const int converted = swr_convert(
            resampler_.get(),
            output,
            outputCapacity,
            input.data(),
            frame.nb_samples
        );
        if (converted < 0) {
            fail(MediaFailureCode::conversionFailed, "convert-audio", converted);
        }
        requireNotCancelled(cancellation, "after-audio-convert");
        bytes.resize(static_cast<std::size_t>(converted) * targetFormat_.blockAlign);
        DecodedAudioBlock block{
            frame.best_effort_timestamp,
            rational(cursor_.stream().time_base),
            *nextOutputSample_,
            static_cast<std::uint32_t>(converted),
            targetFormat_,
            std::move(bytes),
        };
        *nextOutputSample_ = checkedAddSamples(
            *nextOutputSample_,
            converted,
            "output-sample-overflow"
        );
        return block;
    }

    std::optional<DecodedAudioBlock> drain(std::stop_token cancellation) {
        if (!sourceConfigured_) {
            exhausted_ = true;
            return std::nullopt;
        }
        for (;;) {
            requireNotCancelled(cancellation, "before-resampler-drain");
            const int outputCapacity = swr_get_out_samples(resampler_.get(), 0);
            if (outputCapacity < 0) {
                fail(
                    MediaFailureCode::conversionFailed,
                    "drain-audio-capacity",
                    outputCapacity
                );
            }
            if (outputCapacity == 0) {
                exhausted_ = true;
                return std::nullopt;
            }
            if (static_cast<std::uint64_t>(outputCapacity)
                > limits_.maximumAudioFramesPerBlock) {
                fail(MediaFailureCode::resourceLimitExceeded, "drain-audio-capacity");
            }
            std::vector<std::byte> bytes(checkedAudioBytes(
                static_cast<std::uint32_t>(outputCapacity),
                targetFormat_,
                limits_
            ));
            std::uint8_t* output[] = {
                reinterpret_cast<std::uint8_t*>(bytes.data()),
            };
            const int converted = swr_convert(
                resampler_.get(),
                output,
                outputCapacity,
                nullptr,
                0
            );
            if (converted < 0) {
                fail(MediaFailureCode::conversionFailed, "drain-audio", converted);
            }
            if (converted == 0) {
                exhausted_ = true;
                return std::nullopt;
            }
            requireNotCancelled(cancellation, "after-resampler-drain");
            bytes.resize(static_cast<std::size_t>(converted) * targetFormat_.blockAlign);
            DecodedAudioBlock block{
                checkedAddSamples(
                    *sourceAnchorTimestamp_,
                    av_rescale_q_rnd(
                        sourceFramesRead_,
                        AVRational{1, sourceSampleRate_},
                        cursor_.stream().time_base,
                        AV_ROUND_NEAR_INF
                    ),
                    "drain-source-timestamp-overflow"
                ),
                rational(cursor_.stream().time_base),
                *nextOutputSample_,
                static_cast<std::uint32_t>(converted),
                targetFormat_,
                std::move(bytes),
            };
            *nextOutputSample_ = checkedAddSamples(
                *nextOutputSample_,
                converted,
                "drain-output-sample-overflow"
            );
            return block;
        }
    }

    audio::PcmFormat targetFormat_;
    AVSampleFormat targetSampleFormat_{AV_SAMPLE_FMT_NONE};
    DecodeLimits limits_;
    SoftwareFrameReader cursor_;
    ResampleOwner resampler_;
    AVChannelLayout sourceLayout_{};
    std::exception_ptr terminalError_;
    std::optional<std::int64_t> sourceAnchorTimestamp_;
    std::optional<std::int64_t> nextOutputSample_;
    std::int64_t sourceFramesRead_{};
    int sourceSampleRate_{};
    int sourceSampleFormat_{-1};
    bool sourceConfigured_{};
    bool exhausted_{};
};

bool isPrototypeSrgbColor(const ColorMetadata& color) noexcept {
    return color.primaries == AVCOL_PRI_BT709
        && color.transfer == AVCOL_TRC_IEC61966_2_1
        && color.matrix == AVCOL_SPC_RGB
        && (color.range == AVCOL_RANGE_JPEG
            || color.range == AVCOL_RANGE_UNSPECIFIED);
}

bool isPrototypeBt709VideoColor(const ColorMetadata& color) noexcept {
    return color.primaries == AVCOL_PRI_BT709
        && color.transfer == AVCOL_TRC_BT709
        && color.matrix == AVCOL_SPC_BT709
        && color.range == AVCOL_RANGE_MPEG;
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
        && swresample_version() == LIBSWRESAMPLE_VERSION_INT
        && swscale_version() == LIBSWSCALE_VERSION_INT;
    return {
        av_version_info(),
        avcodec_license(),
        avcodec_configuration(),
        headersMatch,
    };
}

FfmpegVideoFrameReader::FfmpegVideoFrameReader(
    const std::filesystem::path& input,
    const DecodeLimits& limits,
    std::stop_token cancellation
) : impl_(std::make_unique<Impl>(input, limits, cancellation)) {}

FfmpegVideoFrameReader::~FfmpegVideoFrameReader() = default;

std::optional<DecodedVideoFrame> FfmpegVideoFrameReader::nextFrame(
    std::stop_token cancellation
) {
    return impl_->nextFrame(cancellation);
}

FfmpegAudioFrameReader::FfmpegAudioFrameReader(
    const std::filesystem::path& input,
    audio::PcmFormat targetFormat,
    const DecodeLimits& limits,
    std::stop_token cancellation
) : impl_(std::make_unique<Impl>(
    input,
    targetFormat,
    limits,
    cancellation
)) {}

FfmpegAudioFrameReader::~FfmpegAudioFrameReader() = default;

std::optional<DecodedAudioBlock> FfmpegAudioFrameReader::nextBlock(
    std::stop_token cancellation
) {
    return impl_->nextBlock(cancellation);
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
    FfmpegVideoFrameReader reader(input, limits, cancellation);
    if (auto decoded = reader.nextFrame(cancellation)) {
        return std::move(*decoded);
    }
    fail(MediaFailureCode::corruptInput, "missing-decoded-frame");
}

}
