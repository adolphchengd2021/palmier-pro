#include "palmier/exporting/h264_project_exporter.hpp"

#include "internal/h264_project_exporter_testing.hpp"

#include "palmier/media/ffmpeg_media_reader.hpp"
#include "palmier/media/render_source_adapter.hpp"
#include "palmier/project_render/project_render_compiler.hpp"
#include "palmier/render/cpu_renderer.hpp"

#define NOMINMAX
#include <Windows.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
#include <libavutil/rational.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace palmier::exporting {
namespace {

std::atomic<std::uint64_t> stagingSerial{1};
std::condition_variable_any exportGateCondition;
std::mutex exportGateMutex;
bool exportActive{};

static_assert(std::is_nothrow_move_constructible_v<H264ProjectExportReceipt>);

[[noreturn]] void fail(
    H264ExportFailureCode code,
    std::string stage,
    std::string detail,
    int nativeCode = 0
) {
    throw H264ExportError(code, std::move(stage), std::move(detail), nativeCode);
}

void checkCancellation(std::stop_token cancellation, std::string_view stage) {
    if (cancellation.stop_requested()) {
        fail(H264ExportFailureCode::cancelled, std::string(stage), "export cancelled");
    }
}

H264ExportFailureCode compileFailureCode(std::string_view code) noexcept {
    if (code == "cancelled") {
        return H264ExportFailureCode::cancelled;
    }
    if (code == "entityUnavailable") {
        return H264ExportFailureCode::invalidRequest;
    }
    if (code == "resourceLimitExceeded") {
        return H264ExportFailureCode::resourceLimitExceeded;
    }
    return H264ExportFailureCode::unsupportedProject;
}

void runCheckpoint(
    const detail::H264ExportTestHooks* hooks,
    std::string_view name,
    std::stop_token cancellation
) {
    if (hooks != nullptr && hooks->checkpoint) {
        hooks->checkpoint(name);
    }
    checkCancellation(cancellation, name);
}

class ExportLease final {
public:
    explicit ExportLease(std::stop_token cancellation) {
        std::unique_lock lock(exportGateMutex);
        const bool acquired = exportGateCondition.wait(
            lock,
            cancellation,
            [] { return !exportActive; }
        );
        if (!acquired) {
            fail(
                H264ExportFailureCode::cancelled,
                "waitForExportSlot",
                "export cancelled while waiting for the encoder"
            );
        }
        exportActive = true;
    }

    ~ExportLease() {
        {
            const std::lock_guard lock(exportGateMutex);
            exportActive = false;
        }
        exportGateCondition.notify_one();
    }

    ExportLease(const ExportLease&) = delete;
    ExportLease& operator=(const ExportLease&) = delete;
};

std::string ffmpegMessage(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    if (av_strerror(code, buffer, sizeof(buffer)) < 0) {
        return "unknown FFmpeg failure";
    }
    return buffer;
}

[[noreturn]] void failFfmpeg(
    H264ExportFailureCode code,
    std::string stage,
    int ffmpegCode
) {
    fail(code, std::move(stage), ffmpegMessage(ffmpegCode), ffmpegCode);
}

std::string pathBytes(const std::filesystem::path& path) {
    const auto encoded = path.u8string();
    return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

bool pathsEqual(
    const std::filesystem::path& lhs,
    const std::filesystem::path& rhs
) {
    const auto left = lhs.lexically_normal().native();
    const auto right = rhs.lexically_normal().native();
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE)
        == CSTR_EQUAL;
}

bool hasMp4Extension(const std::filesystem::path& path) {
    const auto extension = path.extension().native();
    return CompareStringOrdinal(
        extension.c_str(),
        -1,
        L".mp4",
        -1,
        TRUE
    ) == CSTR_EQUAL;
}

bool pathExists(const std::filesystem::path& path, std::string_view stage) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        return true;
    }
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
        return false;
    }
    fail(
        H264ExportFailureCode::stagingFailed,
        std::string(stage),
        "path metadata query failed",
        static_cast<int>(error)
    );
}

void requireDirectory(const std::filesystem::path& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES
        || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        const DWORD error = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : 0;
        fail(
            H264ExportFailureCode::invalidRequest,
            "validateDestination",
            "destination directory is unavailable",
            static_cast<int>(error)
        );
    }
}

class HandleOwner final {
public:
    explicit HandleOwner(HANDLE value = INVALID_HANDLE_VALUE) noexcept
        : value_(value) {}

    ~HandleOwner() {
        if (value_ != INVALID_HANDLE_VALUE) {
            CloseHandle(value_);
        }
    }

    HandleOwner(const HandleOwner&) = delete;
    HandleOwner& operator=(const HandleOwner&) = delete;

    HANDLE get() const noexcept { return value_; }
    bool valid() const noexcept { return value_ != INVALID_HANDLE_VALUE; }

    void reset(HANDLE value = INVALID_HANDLE_VALUE) noexcept {
        if (value_ != INVALID_HANDLE_VALUE) {
            CloseHandle(value_);
        }
        value_ = value;
    }

private:
    HANDLE value_;
};

class StagingFile final {
public:
    explicit StagingFile(const std::filesystem::path& destination) {
        const auto parent = destination.parent_path();
        const auto base = destination.filename().native();
        for (std::uint32_t attempt = 0; attempt < 64; ++attempt) {
            const auto serial = stagingSerial.fetch_add(1, std::memory_order_relaxed);
            path_ = parent / (
                base
                + L".palmier-"
                + std::to_wstring(GetCurrentProcessId())
                + L"-"
                + std::to_wstring(serial)
                + L".partial.mp4"
            );
            HandleOwner file(CreateFileW(
                path_.c_str(),
                GENERIC_READ | GENERIC_WRITE | DELETE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr,
                CREATE_NEW,
                FILE_ATTRIBUTE_NORMAL,
                nullptr
            ));
            if (file.valid()) {
                FILE_ID_INFO identity{};
                if (!GetFileInformationByHandleEx(
                        file.get(),
                        FileIdInfo,
                        &identity,
                        static_cast<DWORD>(sizeof(identity))
                    )) {
                    const DWORD identityError = GetLastError();
                    FILE_DISPOSITION_INFO disposition{static_cast<BOOLEAN>(TRUE)};
                    if (!SetFileInformationByHandle(
                            file.get(),
                            FileDispositionInfo,
                            &disposition,
                            static_cast<DWORD>(sizeof(disposition))
                        )) {
                        fail(
                            H264ExportFailureCode::cleanupFailed,
                            "cleanupStaging",
                            "unidentified staging file could not be removed",
                            static_cast<int>(GetLastError())
                        );
                    }
                    fail(
                        H264ExportFailureCode::stagingFailed,
                        "createStaging",
                        "staging file identity query failed",
                        static_cast<int>(identityError)
                    );
                }
                identity_ = identity;
                const HANDLE tracking = ReOpenFile(
                    file.get(),
                    FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    0
                );
                if (tracking == INVALID_HANDLE_VALUE) {
                    const DWORD trackingError = GetLastError();
                    FILE_DISPOSITION_INFO disposition{static_cast<BOOLEAN>(TRUE)};
                    if (!SetFileInformationByHandle(
                            file.get(),
                            FileDispositionInfo,
                            &disposition,
                            static_cast<DWORD>(sizeof(disposition))
                        )) {
                        fail(
                            H264ExportFailureCode::cleanupFailed,
                            "cleanupStaging",
                            "untracked staging file could not be removed",
                            static_cast<int>(GetLastError())
                        );
                    }
                    fail(
                        H264ExportFailureCode::stagingFailed,
                        "createStaging",
                        "staging identity handle could not be retained",
                        static_cast<int>(trackingError)
                    );
                }
                identityHandle_.reset(tracking);
                return;
            }
            const DWORD error = GetLastError();
            if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) {
                fail(
                    H264ExportFailureCode::stagingFailed,
                    "createStaging",
                    "staging file creation failed",
                    static_cast<int>(error)
                );
            }
        }
        fail(
            H264ExportFailureCode::stagingFailed,
            "createStaging",
            "unique staging file limit exceeded"
        );
    }

    ~StagingFile() { static_cast<void>(cleanupImpl()); }

    StagingFile(const StagingFile&) = delete;
    StagingFile& operator=(const StagingFile&) = delete;

    const std::filesystem::path& path() const noexcept { return path_; }

    void lockForVerification(
        const detail::H264ExportTestHooks* hooks,
        std::stop_token cancellation
    ) {
        verificationHandle_ = CreateFileW(
            path_.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );
        if (verificationHandle_ == INVALID_HANDLE_VALUE) {
            fail(
                H264ExportFailureCode::stagingFailed,
                "lockStaging",
                "staging file could not be locked for verification",
                static_cast<int>(GetLastError())
            );
        }
        const auto identity = requireIdentity(verificationHandle_, "lockStaging");
        if (!sameIdentity(identity_, identity)) {
            fail(
                H264ExportFailureCode::stagingFailed,
                "lockStaging",
                "staging file identity changed before verification"
            );
        }
        runCheckpoint(hooks, "beforeFlush", cancellation);
        const auto flushResult = [&] {
            if (hooks != nullptr && hooks->flushFileBuffers) {
                return hooks->flushFileBuffers(
                    reinterpret_cast<std::uintptr_t>(verificationHandle_),
                    path_
                );
            }
            const bool flushed = FlushFileBuffers(verificationHandle_) != FALSE;
            return detail::FileFlushResult{
                flushed,
                static_cast<int>(flushed ? ERROR_SUCCESS : GetLastError()),
            };
        }();
        if (!flushResult.flushed) {
            fail(
                H264ExportFailureCode::stagingFailed,
                "flushStaging",
                "staging file could not be durably flushed",
                flushResult.nativeCode
            );
        }
        runCheckpoint(hooks, "afterFlush", cancellation);
    }

    void install(
        const std::filesystem::path& destination,
        bool replaceExisting
    ) {
        HandleOwner commit(ReOpenFile(
            identityHandle_.get(),
            GENERIC_READ | DELETE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            0
        ));
        if (!commit.valid()) {
            fail(
                H264ExportFailureCode::installFailed,
                "installDestination",
                "verified staging file could not be opened for commit",
                static_cast<int>(GetLastError())
            );
        }
        const auto identity = requireIdentity(commit.get(), "installDestination");
        if (!sameIdentity(identity_, identity)) {
            fail(
                H264ExportFailureCode::installFailed,
                "installDestination",
                "verified staging file identity changed before commit"
            );
        }

        const auto destinationName = destination.native();
        constexpr auto maximumInfoBytes = (std::numeric_limits<DWORD>::max)();
        if (destinationName.size() > maximumInfoBytes / sizeof(wchar_t)) {
            fail(
                H264ExportFailureCode::installFailed,
                "installDestination",
                "destination path exceeds the rename contract"
            );
        }
        const std::size_t nameBytes = destinationName.size() * sizeof(wchar_t);
        if (nameBytes > maximumInfoBytes - sizeof(FILE_RENAME_INFO)) {
            fail(
                H264ExportFailureCode::installFailed,
                "installDestination",
                "destination path exceeds the rename contract"
            );
        }
        const std::size_t infoSize = sizeof(FILE_RENAME_INFO) + nameBytes;
        std::vector<std::byte> storage(infoSize);
        auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
        rename->ReplaceIfExists = static_cast<BOOLEAN>(replaceExisting);
        rename->RootDirectory = nullptr;
        rename->FileNameLength = static_cast<DWORD>(nameBytes);
        std::memcpy(rename->FileName, destinationName.data(), nameBytes);
        const BOOL renamed = SetFileInformationByHandle(
            commit.get(),
            FileRenameInfo,
            rename,
            static_cast<DWORD>(storage.size())
        );
        const DWORD error = renamed ? ERROR_SUCCESS : GetLastError();
        if (!renamed) {
            fail(
                H264ExportFailureCode::installFailed,
                "installDestination",
                "handle-based destination install failed",
                static_cast<int>(error)
            );
        }
        closeVerificationHandle();
        remove_ = false;
        identityHandle_.reset();
    }

    void cleanup() {
        if (!cleanupImpl()) {
            fail(
                H264ExportFailureCode::cleanupFailed,
                "cleanupStaging",
                "staging cleanup failed or its identity changed",
                cleanupCode_
            );
        }
    }

private:
    static FILE_ID_INFO requireIdentity(HANDLE file, std::string_view stage) {
        FILE_ID_INFO identity{};
        if (!GetFileInformationByHandleEx(
                file,
                FileIdInfo,
                &identity,
                static_cast<DWORD>(sizeof(identity))
            )) {
            fail(
                H264ExportFailureCode::stagingFailed,
                std::string(stage),
                "staging file identity query failed",
                static_cast<int>(GetLastError())
            );
        }
        return identity;
    }

    static bool sameIdentity(
        const FILE_ID_INFO& lhs,
        const FILE_ID_INFO& rhs
    ) noexcept {
        return lhs.VolumeSerialNumber == rhs.VolumeSerialNumber
            && std::memcmp(
                lhs.FileId.Identifier,
                rhs.FileId.Identifier,
                sizeof(lhs.FileId.Identifier)
            ) == 0;
    }

    void closeVerificationHandle() noexcept {
        if (verificationHandle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(verificationHandle_);
            verificationHandle_ = INVALID_HANDLE_VALUE;
        }
    }

    bool cleanupImpl() noexcept {
        if (!remove_) {
            return true;
        }
        closeVerificationHandle();
        HandleOwner file(ReOpenFile(
            identityHandle_.get(),
            DELETE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            0
        ));
        if (!file.valid()) {
            cleanupCode_ = static_cast<int>(GetLastError());
            return false;
        }
        FILE_DISPOSITION_INFO disposition{static_cast<BOOLEAN>(TRUE)};
        const BOOL removed = SetFileInformationByHandle(
            file.get(),
            FileDispositionInfo,
            &disposition,
            static_cast<DWORD>(sizeof(disposition))
        );
        const DWORD error = removed ? ERROR_SUCCESS : GetLastError();
        if (!removed) {
            cleanupCode_ = static_cast<int>(error);
            return false;
        }
        remove_ = false;
        identityHandle_.reset();
        return true;
    }

    std::filesystem::path path_;
    FILE_ID_INFO identity_{};
    HandleOwner identityHandle_;
    HANDLE verificationHandle_{INVALID_HANDLE_VALUE};
    bool remove_{true};
    int cleanupCode_{};
};

struct CodecDeleter final {
    void operator()(AVCodecContext* value) const noexcept {
        avcodec_free_context(&value);
    }
};

struct FrameDeleter final {
    void operator()(AVFrame* value) const noexcept { av_frame_free(&value); }
};

struct PacketDeleter final {
    void operator()(AVPacket* value) const noexcept { av_packet_free(&value); }
};

struct ScaleDeleter final {
    void operator()(SwsContext* value) const noexcept { sws_freeContext(value); }
};

using CodecOwner = std::unique_ptr<AVCodecContext, CodecDeleter>;
using FrameOwner = std::unique_ptr<AVFrame, FrameDeleter>;
using PacketOwner = std::unique_ptr<AVPacket, PacketDeleter>;
using ScaleOwner = std::unique_ptr<SwsContext, ScaleDeleter>;

class OutputOwner final {
public:
    ~OutputOwner() {
        closeFile();
        avformat_free_context(value_);
    }

    AVFormatContext** address() noexcept { return &value_; }
    AVFormatContext* get() const noexcept { return value_; }

    void closeFile() noexcept {
        if (value_ != nullptr && value_->pb != nullptr) {
            avio_closep(&value_->pb);
        }
    }

private:
    AVFormatContext* value_{};
};

bool isExactRate(const media::Rational& rate, std::int32_t fps) {
    if (rate.numerator <= 0 || rate.denominator <= 0 || fps <= 0) {
        return false;
    }
    return av_cmp_q(
        {rate.numerator, rate.denominator},
        {fps, 1}
    ) == 0;
}

std::string rationalText(const media::Rational& value) {
    return std::to_string(value.numerator) + "/" + std::to_string(value.denominator);
}

void requireExactSourceProbe(
    const std::filesystem::path& input,
    std::int32_t fps,
    std::stop_token cancellation
) {
    const media::MediaProbe probe = media::FfmpegMediaReader::probe(
        input,
        {},
        cancellation
    );
    const auto stream = std::find_if(
        probe.streams.begin(),
        probe.streams.end(),
        [](const auto& value) { return value.kind == media::StreamKind::video; }
    );
    if (stream == probe.streams.end()) {
        fail(
            H264ExportFailureCode::unsupportedSourceTiming,
            "probeSource",
            "source has no video stream"
        );
    }
    if (!isExactRate(stream->averageFrameRate, fps)
        || !isExactRate(stream->realFrameRate, fps)) {
        fail(
            H264ExportFailureCode::unsupportedSourceTiming,
            "probeSource",
            "source frame rate does not exactly match the project"
        );
    }
}

void requireExactTimestamp(
    const media::DecodedVideoFrame& frame,
    std::int64_t expectedFrame,
    std::int32_t fps,
    std::string_view stage,
    H264ExportFailureCode failureCode
) {
    if (!frame.presentationTimestamp.has_value()
        || frame.timeBase.numerator <= 0
        || frame.timeBase.denominator <= 0
        || av_compare_ts(
            *frame.presentationTimestamp,
            {frame.timeBase.numerator, frame.timeBase.denominator},
            expectedFrame,
            {1, fps}
        ) != 0) {
        fail(
            failureCode,
            std::string(stage),
            "decoded timestamp does not map to the expected integer frame"
        );
    }
}

AVPixelFormat requireEncoderPixelFormat(
    const AVCodec* encoder,
    const AVCodecContext* context
) {
    const void* configurations = nullptr;
    int count = 0;
    const int result = avcodec_get_supported_config(
        context,
        encoder,
        AV_CODEC_CONFIG_PIX_FORMAT,
        0,
        &configurations,
        &count
    );
    if (result < 0) {
        failFfmpeg(H264ExportFailureCode::unsupportedEncoder, "queryEncoder", result);
    }
    const auto* formats = static_cast<const AVPixelFormat*>(configurations);
    if (formats == nullptr) {
        return AV_PIX_FMT_NV12;
    }
    for (int index = 0; index < count; ++index) {
        if (formats[index] == AV_PIX_FMT_NV12) {
            return AV_PIX_FMT_NV12;
        }
    }
    fail(
        H264ExportFailureCode::unsupportedEncoder,
        "queryEncoder",
        "h264_mf does not expose NV12 input"
    );
}

void writePackets(
    AVCodecContext* codec,
    AVFormatContext* output,
    AVStream* stream,
    AVPacket* packet,
    AVFrame* frame,
    std::stop_token cancellation
) {
    checkCancellation(cancellation, "encodeFrame");
    const int sendResult = avcodec_send_frame(codec, frame);
    if (sendResult < 0) {
        failFfmpeg(H264ExportFailureCode::encodeFailed, "sendFrame", sendResult);
    }
    for (;;) {
        checkCancellation(cancellation, "receivePacket");
        const int receiveResult = avcodec_receive_packet(codec, packet);
        if (receiveResult == AVERROR(EAGAIN) || receiveResult == AVERROR_EOF) {
            return;
        }
        if (receiveResult < 0) {
            failFfmpeg(
                H264ExportFailureCode::encodeFailed,
                "receivePacket",
                receiveResult
            );
        }
        av_packet_rescale_ts(packet, codec->time_base, stream->time_base);
        packet->duration = av_rescale_q(1, codec->time_base, stream->time_base);
        if (packet->duration <= 0) {
            av_packet_unref(packet);
            fail(
                H264ExportFailureCode::encodeFailed,
                "writePacket",
                "encoded packet duration is not representable"
            );
        }
        packet->stream_index = stream->index;
        const int writeResult = av_interleaved_write_frame(output, packet);
        av_packet_unref(packet);
        if (writeResult < 0) {
            failFfmpeg(
                H264ExportFailureCode::encodeFailed,
                "writePacket",
                writeResult
            );
        }
    }
}

std::uint8_t byteChannel(float value) {
    const float bounded = std::clamp(value, 0.0F, 1.0F);
    return static_cast<std::uint8_t>(std::lround(bounded * 255.0F));
}

void copyRgba8(
    const render::RenderedFrame& rendered,
    std::vector<std::uint8_t>& rgba
) {
    for (std::size_t index = 0; index < rendered.pixels.size(); ++index) {
        const auto& pixel = rendered.pixels[index];
        rgba[index * 4] = byteChannel(pixel.red);
        rgba[index * 4 + 1] = byteChannel(pixel.green);
        rgba[index * 4 + 2] = byteChannel(pixel.blue);
        rgba[index * 4 + 3] = byteChannel(pixel.alpha);
    }
}

std::uint64_t verifyOutput(
    const std::filesystem::path& staging,
    std::uint32_t canvasWidth,
    std::uint32_t canvasHeight,
    std::int32_t framesPerSecond,
    std::int64_t frameCount,
    std::stop_token cancellation
) {
    checkCancellation(cancellation, "verifyOutput");
    const auto probe = media::FfmpegMediaReader::probe(staging, {}, cancellation);
    const auto stream = std::find_if(
        probe.streams.begin(),
        probe.streams.end(),
        [](const auto& value) { return value.kind == media::StreamKind::video; }
    );
    if (stream == probe.streams.end()
        || stream->codecName != "h264"
        || stream->width != static_cast<std::int32_t>(canvasWidth)
        || stream->height != static_cast<std::int32_t>(canvasHeight)
        || !isExactRate(stream->averageFrameRate, framesPerSecond)
        || !isExactRate(stream->realFrameRate, framesPerSecond)
        || stream->timeBase.numerator <= 0
        || stream->timeBase.denominator <= 0
        || !stream->duration.has_value()
        || *stream->duration <= 0
        || av_compare_ts(
            *stream->duration,
            {stream->timeBase.numerator, stream->timeBase.denominator},
            frameCount,
            {1, framesPerSecond}
        ) != 0) {
        const std::string detail = stream == probe.streams.end()
            ? "encoded output has no video stream"
            : "encoded stream contract differs: codec="
                + stream->codecName
                + ", size="
                + std::to_string(stream->width)
                + "x"
                + std::to_string(stream->height)
                + ", averageRate="
                + rationalText(stream->averageFrameRate)
                + ", realRate="
                + rationalText(stream->realFrameRate)
                + ", timeBase="
                + rationalText(stream->timeBase)
                + ", duration="
                + (stream->duration.has_value()
                    ? std::to_string(*stream->duration)
                    : "unknown");
        fail(
            H264ExportFailureCode::verificationFailed,
            "verifyProbe",
            detail
        );
    }

    media::FfmpegVideoFrameReader reader(staging, {}, cancellation);
    for (std::int64_t index = 0; index < frameCount; ++index) {
        checkCancellation(cancellation, "verifyDecode");
        const auto decoded = reader.nextFrame(cancellation);
        if (!decoded.has_value()) {
            fail(
                H264ExportFailureCode::verificationFailed,
                "verifyDecode",
                "encoded stream ended early"
            );
        }
        if (decoded->width != static_cast<std::int32_t>(canvasWidth)
            || decoded->height != static_cast<std::int32_t>(canvasHeight)) {
            fail(
                H264ExportFailureCode::verificationFailed,
                "verifyDecode",
                "decoded output dimensions differ"
            );
        }
        requireExactTimestamp(
            *decoded,
            index,
            framesPerSecond,
            "verifyDecode",
            H264ExportFailureCode::verificationFailed
        );
    }
    if (reader.nextFrame(cancellation).has_value()) {
        fail(
            H264ExportFailureCode::verificationFailed,
            "verifyDecode",
            "encoded stream has extra frames"
        );
    }
    return static_cast<std::uint64_t>(frameCount);
}

}

H264ExportError::H264ExportError(
    H264ExportFailureCode errorCode,
    std::string errorStage,
    std::string detail,
    int nativeCodeValue
)
    : std::runtime_error(errorStage + ": " + detail),
      code(errorCode),
      stage(std::move(errorStage)),
      nativeCode(nativeCodeValue) {}

namespace {

H264ProjectExportReceipt exportCompiledStaticTimelineH264(
    const project_render::StaticVideoTimeline& timeline,
    std::int64_t firstTimelineFrame,
    std::int64_t frameCount,
    const std::vector<H264ProjectExportSource>& sources,
    const std::filesystem::path& destination,
    std::int64_t bitRate,
    bool replaceExisting,
    const H264ExportLimits& limits,
    std::stop_token cancellation,
    const detail::H264ExportTestHooks* hooks
) {
    const ExportLease lease(cancellation);
    checkCancellation(cancellation, "validateRequest");
    if (limits.maximumFrames == 0
        || limits.minimumBitRate <= 0
        || limits.maximumBitRate < limits.minimumBitRate
        || timeline.timelineId.empty()
        || timeline.segments.empty()
        || sources.size() != timeline.segments.size()
        || !destination.is_absolute()
        || !hasMp4Extension(destination)
        || firstTimelineFrame < 0
        || frameCount <= 0
        || firstTimelineFrame > timeline.durationFrames - frameCount
        || bitRate < limits.minimumBitRate
        || bitRate > limits.maximumBitRate) {
        fail(
            H264ExportFailureCode::invalidRequest,
            "validateRequest",
            "export request or limits are invalid"
        );
    }
    requireDirectory(destination.parent_path());
    if (!replaceExisting && pathExists(destination, "validateDestination")) {
        fail(
            H264ExportFailureCode::destinationExists,
            "validateDestination",
            "destination already exists"
        );
    }
    if (timeline.canvasWidth < 2
        || timeline.canvasHeight < 2
        || (timeline.canvasWidth % 2) != 0
        || (timeline.canvasHeight % 2) != 0) {
        fail(
            H264ExportFailureCode::unsupportedProject,
            "validateCompiledTimeline",
            "compiled timeline exceeds the static H.264 export contract"
        );
    }
    if (static_cast<std::uint64_t>(frameCount) > limits.maximumFrames
        || timeline.framesPerSecond <= 0
        || timeline.framesPerSecond > 240) {
        fail(
            H264ExportFailureCode::resourceLimitExceeded,
            "validateCompiledTimeline",
            "compiled timeline exceeds the export resource limits"
        );
    }

    std::vector<const std::filesystem::path*> segmentInputs;
    segmentInputs.reserve(timeline.segments.size());
    std::vector<std::filesystem::path> probedInputs;
    probedInputs.reserve(timeline.segments.size());
    for (const auto& segment : timeline.segments) {
        checkCancellation(cancellation, "resolveSources");
        const H264ProjectExportSource* matched = nullptr;
        for (const auto& source : sources) {
            if (source.clipId != segment.clipId) continue;
            if (matched != nullptr) {
                fail(
                    H264ExportFailureCode::invalidRequest,
                    "resolveSources",
                    "scheduled clip source identity is ambiguous"
                );
            }
            matched = &source;
        }
        if (matched == nullptr || matched->input.empty()
            || !matched->input.is_absolute()
            || pathsEqual(matched->input, destination)) {
            fail(
                H264ExportFailureCode::invalidRequest,
                "resolveSources",
                "every scheduled clip requires one distinct local input mapping"
            );
        }
        segmentInputs.push_back(&matched->input);
        const bool alreadyProbed = std::any_of(
            probedInputs.begin(),
            probedInputs.end(),
            [&](const auto& input) { return pathsEqual(input, matched->input); }
        );
        if (alreadyProbed) continue;
        try {
            requireExactSourceProbe(
                matched->input,
                timeline.framesPerSecond,
                cancellation
            );
        } catch (const media::MediaError& error) {
            fail(
                error.code == media::MediaFailureCode::cancelled
                    ? H264ExportFailureCode::cancelled
                    : H264ExportFailureCode::unsupportedSourceTiming,
                "probeSource",
                error.what(),
                error.ffmpegCode
            );
        }
        probedInputs.push_back(matched->input);
    }

    StagingFile staging(destination);
    try {
    runCheckpoint(hooks, "afterStaging", cancellation);
    const std::string stagingBytes = pathBytes(staging.path());
    OutputOwner output;
    const int outputResult = avformat_alloc_output_context2(
        output.address(),
        nullptr,
        "mp4",
        stagingBytes.c_str()
    );
    if (outputResult < 0 || output.get() == nullptr) {
        failFfmpeg(
            H264ExportFailureCode::encodeFailed,
            "createContainer",
            outputResult < 0 ? outputResult : AVERROR_UNKNOWN
        );
    }

    const AVCodec* encoder = avcodec_find_encoder_by_name("h264_mf");
    if (encoder == nullptr) {
        fail(
            H264ExportFailureCode::unsupportedEncoder,
            "findEncoder",
            "locked h264_mf encoder is unavailable"
        );
    }
    CodecOwner codec(avcodec_alloc_context3(encoder));
    if (!codec) {
        fail(
            H264ExportFailureCode::encodeFailed,
            "createEncoder",
            "encoder context allocation failed"
        );
    }
    codec->codec_id = AV_CODEC_ID_H264;
    codec->codec_type = AVMEDIA_TYPE_VIDEO;
    codec->width = static_cast<int>(timeline.canvasWidth);
    codec->height = static_cast<int>(timeline.canvasHeight);
    codec->time_base = {1, timeline.framesPerSecond};
    codec->framerate = {timeline.framesPerSecond, 1};
    codec->pix_fmt = requireEncoderPixelFormat(encoder, codec.get());
    codec->bit_rate = bitRate;
    codec->gop_size = timeline.framesPerSecond * 2;
    codec->max_b_frames = 0;
    codec->color_primaries = AVCOL_PRI_BT709;
    codec->color_trc = AVCOL_TRC_BT709;
    codec->colorspace = AVCOL_SPC_BT709;
    codec->color_range = AVCOL_RANGE_MPEG;
    codec->chroma_sample_location = AVCHROMA_LOC_LEFT;
    if ((output.get()->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
        codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    const int openEncoderResult = avcodec_open2(codec.get(), encoder, nullptr);
    if (openEncoderResult < 0) {
        failFfmpeg(
            H264ExportFailureCode::unsupportedEncoder,
            "openEncoder",
            openEncoderResult
        );
    }

    AVStream* stream = avformat_new_stream(output.get(), nullptr);
    if (stream == nullptr) {
        fail(
            H264ExportFailureCode::encodeFailed,
            "createStream",
            "output stream allocation failed"
        );
    }
    stream->time_base = codec->time_base;
    const int parameterResult = avcodec_parameters_from_context(
        stream->codecpar,
        codec.get()
    );
    if (parameterResult < 0) {
        failFfmpeg(
            H264ExportFailureCode::encodeFailed,
            "copyEncoderParameters",
            parameterResult
        );
    }
    const int fileResult = avio_open(&output.get()->pb, stagingBytes.c_str(), AVIO_FLAG_WRITE);
    if (fileResult < 0) {
        failFfmpeg(H264ExportFailureCode::stagingFailed, "openStaging", fileResult);
    }
    const int headerResult = avformat_write_header(output.get(), nullptr);
    if (headerResult < 0) {
        failFfmpeg(H264ExportFailureCode::encodeFailed, "writeHeader", headerResult);
    }

    FrameOwner frame(av_frame_alloc());
    PacketOwner packet(av_packet_alloc());
    if (!frame || !packet) {
        fail(
            H264ExportFailureCode::encodeFailed,
            "allocateEncodeBuffers",
            "frame or packet allocation failed"
        );
    }
    frame->format = codec->pix_fmt;
    frame->width = codec->width;
    frame->height = codec->height;
    frame->color_primaries = codec->color_primaries;
    frame->color_trc = codec->color_trc;
    frame->colorspace = codec->colorspace;
    frame->color_range = codec->color_range;
    frame->chroma_location = codec->chroma_sample_location;
    const int frameBufferResult = av_frame_get_buffer(frame.get(), 32);
    if (frameBufferResult < 0) {
        failFfmpeg(
            H264ExportFailureCode::encodeFailed,
            "allocateFrameBuffer",
            frameBufferResult
        );
    }
    ScaleOwner scale(sws_getContext(
        codec->width,
        codec->height,
        AV_PIX_FMT_RGBA,
        codec->width,
        codec->height,
        codec->pix_fmt,
        SWS_BICUBIC,
        nullptr,
        nullptr,
        nullptr
    ));
    if (!scale) {
        fail(
            H264ExportFailureCode::encodeFailed,
            "createPixelConverter",
            "RGBA to encoder format conversion is unavailable"
        );
    }
    const int* coefficients = sws_getCoefficients(SWS_CS_ITU709);
    const int colorResult = sws_setColorspaceDetails(
        scale.get(),
        coefficients,
        1,
        coefficients,
        0,
        0,
        1 << 16,
        1 << 16
    );
    if (colorResult < 0) {
        failFfmpeg(
            H264ExportFailureCode::encodeFailed,
            "configurePixelConverter",
            colorResult
        );
    }

    const std::uint64_t pixelCount = static_cast<std::uint64_t>(timeline.canvasWidth)
        * static_cast<std::uint64_t>(timeline.canvasHeight);
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(pixelCount * 4));
    std::unique_ptr<media::FfmpegVideoFrameReader> reader;
    const project_render::StaticVideoLayer* activeSegment = nullptr;
    render::CpuRenderer renderer;
    for (std::int64_t index = 0; index < frameCount; ++index) {
        checkCancellation(cancellation, "renderTimeline");
        const std::int64_t timelineFrame = firstTimelineFrame + index;
        const auto plan = project_render::makeRenderPlan(timeline, timelineFrame);
        const auto* segment = project_render::staticVideoLayerAt(
            timeline,
            timelineFrame
        );
        std::optional<render::SourceFrame> source;
        std::int64_t expectedSourceFrame{};
        if (segment != nullptr) {
            const auto segmentIndex = static_cast<std::size_t>(
                segment - timeline.segments.data()
            );
            if (activeSegment != segment) {
                try {
                    reader = std::make_unique<media::FfmpegVideoFrameReader>(
                        *segmentInputs[segmentIndex],
                        media::DecodeFrameStart{
                            segment->sourceStartFrame,
                            {segment->framesPerSecond, 1},
                        },
                        media::DecodeLimits{},
                        cancellation
                    );
                } catch (const media::MediaError& error) {
                    fail(
                        error.code == media::MediaFailureCode::cancelled
                            ? H264ExportFailureCode::cancelled
                            : H264ExportFailureCode::encodeFailed,
                        "openSource",
                        error.what(),
                        error.ffmpegCode
                    );
                }
                activeSegment = segment;
            }
            std::optional<media::DecodedVideoFrame> decoded;
            try {
                decoded = reader->nextFrame(cancellation);
            } catch (const media::MediaError& error) {
                fail(
                    error.code == media::MediaFailureCode::cancelled
                        ? H264ExportFailureCode::cancelled
                        : H264ExportFailureCode::encodeFailed,
                    "decodeSource",
                    error.what(),
                    error.ffmpegCode
                );
            }
            if (!decoded.has_value()) {
                fail(
                    H264ExportFailureCode::sourceEndedEarly,
                    "decodeSource",
                    "source ended before the scheduled clip"
                );
            }
            expectedSourceFrame = plan.layers().front().sourceFrame;
            requireExactTimestamp(
                *decoded,
                expectedSourceFrame,
                timeline.framesPerSecond,
                "decodeSource",
                H264ExportFailureCode::unsupportedSourceTiming
            );
            try {
                source = media::makeRenderSourceFrame(*decoded, cancellation);
            } catch (const media::RenderSourceError& error) {
                fail(
                    error.code == "cancelled"
                        ? H264ExportFailureCode::cancelled
                        : H264ExportFailureCode::encodeFailed,
                    "adaptSourceFrame",
                    error.code + " at " + error.pointer
                );
            }
        } else {
            reader.reset();
            activeSegment = nullptr;
        }
        const auto resolver = [&](std::string_view mediaId, std::int64_t sourceFrame)
            -> const render::SourceFrame* {
            return segment != nullptr && source.has_value()
                && mediaId == segment->mediaId
                && sourceFrame == expectedSourceFrame
                ? &*source
                : nullptr;
        };
        const auto rendered = [&] {
            try {
                return render::renderExportFrame(
                    plan,
                    resolver,
                    renderer,
                    cancellation
                );
            } catch (const render::RenderError& error) {
                fail(
                    error.code == "cancelled"
                        ? H264ExportFailureCode::cancelled
                        : H264ExportFailureCode::encodeFailed,
                    "renderFrame",
                    error.code + " at " + error.pointer
                );
            }
        }();
        copyRgba8(rendered, rgba);
        const int writableResult = av_frame_make_writable(frame.get());
        if (writableResult < 0) {
            failFfmpeg(
                H264ExportFailureCode::encodeFailed,
                "makeFrameWritable",
                writableResult
            );
        }
        const std::uint8_t* sourceData[]{rgba.data(), nullptr, nullptr, nullptr};
        const int sourceLines[]{codec->width * 4, 0, 0, 0};
        const int scaled = sws_scale(
            scale.get(),
            sourceData,
            sourceLines,
            0,
            codec->height,
            frame->data,
            frame->linesize
        );
        if (scaled != codec->height) {
            fail(
                H264ExportFailureCode::encodeFailed,
                "convertFrame",
                "pixel conversion returned an incomplete frame"
            );
        }
        frame->pts = index;
        frame->duration = 1;
        writePackets(
            codec.get(),
            output.get(),
            stream,
            packet.get(),
            frame.get(),
            cancellation
        );
    }
    writePackets(
        codec.get(),
        output.get(),
        stream,
        packet.get(),
        nullptr,
        cancellation
    );
    const int trailerResult = av_write_trailer(output.get());
    if (trailerResult < 0) {
        failFfmpeg(H264ExportFailureCode::encodeFailed, "writeTrailer", trailerResult);
    }
    output.closeFile();
    staging.lockForVerification(hooks, cancellation);

    std::uint64_t verifiedFrames = 0;
    try {
        verifiedFrames = verifyOutput(
            staging.path(),
            timeline.canvasWidth,
            timeline.canvasHeight,
            timeline.framesPerSecond,
            frameCount,
            cancellation
        );
    } catch (const media::MediaError& error) {
        fail(
            error.code == media::MediaFailureCode::cancelled
                ? H264ExportFailureCode::cancelled
                : H264ExportFailureCode::verificationFailed,
            "verifyOutput",
            error.what(),
            error.ffmpegCode
        );
    }
    H264ProjectExportReceipt receipt{
        destination,
        encoder->name,
        static_cast<std::uint64_t>(frameCount),
        verifiedFrames,
        timeline.canvasWidth,
        timeline.canvasHeight,
        timeline.framesPerSecond,
    };
    runCheckpoint(hooks, "beforeInstall", cancellation);
    staging.install(destination, replaceExisting);
    return receipt;
    } catch (...) {
        staging.cleanup();
        throw;
    }
}

H264ProjectExportReceipt exportStaticProjectH264Impl(
    const project::ProjectDocument& document,
    const H264ProjectExportRequest& request,
    const H264ExportLimits& limits,
    std::stop_token cancellation,
    const detail::H264ExportTestHooks* hooks
) {
    checkCancellation(cancellation, "validateRequest");
    if (request.timelineId.empty() || request.trackId.empty()
        || request.clipId.empty() || request.input.empty()) {
        fail(
            H264ExportFailureCode::invalidRequest,
            "validateRequest",
            "timeline, track, clip, and input are required"
        );
    }
    project_render::StaticVideoLayer layer;
    try {
        layer = project_render::compileExclusiveStaticVideoLayer(
            document,
            request.timelineId,
            request.trackId,
            request.clipId,
            cancellation
        );
    } catch (const project_render::ProjectRenderCompileError& error) {
        fail(
            compileFailureCode(error.code),
            "compileProject",
            error.code + " at " + error.jsonPointer
        );
    }
    project_render::StaticVideoTimeline timeline{
        layer.canvasWidth,
        layer.canvasHeight,
        layer.framesPerSecond,
        layer.timelineId,
        layer.timelineStartFrame + layer.durationFrames,
        {layer},
    };
    return exportCompiledStaticTimelineH264(
        timeline,
        layer.timelineStartFrame,
        layer.durationFrames,
        {{layer.clipId, request.input}},
        request.destination,
        request.bitRate,
        request.replaceExisting,
        limits,
        cancellation,
        hooks
    );
}

H264ProjectExportReceipt exportStaticProjectTimelineH264Impl(
    const project::ProjectDocument& document,
    const H264ProjectTimelineExportRequest& request,
    const H264ExportLimits& limits,
    std::stop_token cancellation,
    const detail::H264ExportTestHooks* hooks
) {
    checkCancellation(cancellation, "validateRequest");
    if (request.timelineId.empty()) {
        fail(
            H264ExportFailureCode::invalidRequest,
            "validateRequest",
            "timeline is required"
        );
    }
    project_render::StaticVideoTimeline timeline;
    try {
        timeline = project_render::compileStaticVideoTimeline(
            document,
            request.timelineId,
            cancellation
        );
    } catch (const project_render::ProjectRenderCompileError& error) {
        fail(
            compileFailureCode(error.code),
            "compileProject",
            error.code + " at " + error.jsonPointer
        );
    }
    return exportCompiledStaticTimelineH264(
        timeline,
        0,
        timeline.durationFrames,
        request.sources,
        request.destination,
        request.bitRate,
        request.replaceExisting,
        limits,
        cancellation,
        hooks
    );
}

}

H264ProjectExportReceipt exportStaticProjectH264(
    const project::ProjectDocument& document,
    const H264ProjectExportRequest& request,
    const H264ExportLimits& limits,
    std::stop_token cancellation
) {
    return exportStaticProjectH264Impl(
        document,
        request,
        limits,
        cancellation,
        nullptr
    );
}

H264ProjectExportReceipt exportStaticProjectTimelineH264(
    const project::ProjectDocument& document,
    const H264ProjectTimelineExportRequest& request,
    const H264ExportLimits& limits,
    std::stop_token cancellation
) {
    return exportStaticProjectTimelineH264Impl(
        document,
        request,
        limits,
        cancellation,
        nullptr
    );
}

namespace detail {

void installStagingFileForTesting(
    const std::filesystem::path& destination,
    const std::vector<std::uint8_t>& payload,
    bool replaceExisting,
    std::stop_token cancellation,
    const H264ExportTestHooks& hooks
) {
    StagingFile staging(destination);
    try {
        checkCancellation(cancellation, "writeStaging");
        HandleOwner output(CreateFileW(
            staging.path().c_str(),
            GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        ));
        if (!output.valid()) {
            fail(
                H264ExportFailureCode::stagingFailed,
                "writeStaging",
                "staging test payload could not be opened",
                static_cast<int>(GetLastError())
            );
        }
        std::size_t offset{};
        while (offset < payload.size()) {
            checkCancellation(cancellation, "writeStaging");
            const auto remaining = payload.size() - offset;
            const DWORD chunk = static_cast<DWORD>((std::min)(
                remaining,
                static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())
            ));
            DWORD written{};
            if (!WriteFile(output.get(), payload.data() + offset, chunk, &written, nullptr)
                || written != chunk) {
                fail(
                    H264ExportFailureCode::stagingFailed,
                    "writeStaging",
                    "staging test payload could not be written",
                    static_cast<int>(GetLastError())
                );
            }
            offset += written;
        }
        output.reset();
        staging.lockForVerification(&hooks, cancellation);
        runCheckpoint(&hooks, "beforeInstall", cancellation);
        staging.install(destination, replaceExisting);
    } catch (...) {
        staging.cleanup();
        throw;
    }
}

H264ProjectExportReceipt exportStaticProjectH264ForTesting(
    const project::ProjectDocument& document,
    const H264ProjectExportRequest& request,
    const H264ExportLimits& limits,
    std::stop_token cancellation,
    const H264ExportTestHooks& hooks
) {
    return exportStaticProjectH264Impl(
        document,
        request,
        limits,
        cancellation,
        &hooks
    );
}

H264ProjectExportReceipt exportStaticProjectTimelineH264ForTesting(
    const project::ProjectDocument& document,
    const H264ProjectTimelineExportRequest& request,
    const H264ExportLimits& limits,
    std::stop_token cancellation,
    const H264ExportTestHooks& hooks
) {
    return exportStaticProjectTimelineH264Impl(
        document,
        request,
        limits,
        cancellation,
        &hooks
    );
}

}

}
