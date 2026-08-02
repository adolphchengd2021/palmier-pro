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

    void lockForVerification() {
        verificationHandle_ = CreateFileW(
            path_.c_str(),
            GENERIC_READ,
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
    }

    void install(
        const std::filesystem::path& destination,
        bool replaceExisting
    ) {
        HandleOwner commit(ReOpenFile(
            identityHandle_.get(),
            GENERIC_READ | DELETE,
            FILE_SHARE_READ | FILE_SHARE_DELETE,
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
        const std::size_t nameBytes = destinationName.size() * sizeof(wchar_t);
        const std::size_t infoSize = offsetof(FILE_RENAME_INFO, FileName) + nameBytes;
        if (nameBytes > (std::numeric_limits<DWORD>::max)()
            || infoSize > (std::numeric_limits<DWORD>::max)()) {
            fail(
                H264ExportFailureCode::installFailed,
                "installDestination",
                "destination path exceeds the rename contract"
            );
        }
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
    const project_render::StaticVideoLayer& layer,
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
        || stream->width != static_cast<std::int32_t>(layer.canvasWidth)
        || stream->height != static_cast<std::int32_t>(layer.canvasHeight)
        || !isExactRate(stream->averageFrameRate, layer.framesPerSecond)) {
        fail(
            H264ExportFailureCode::verificationFailed,
            "verifyProbe",
            "encoded stream contract differs"
        );
    }

    media::FfmpegVideoFrameReader reader(staging, {}, cancellation);
    for (std::int64_t index = 0; index < layer.durationFrames; ++index) {
        checkCancellation(cancellation, "verifyDecode");
        const auto decoded = reader.nextFrame(cancellation);
        if (!decoded.has_value()) {
            fail(
                H264ExportFailureCode::verificationFailed,
                "verifyDecode",
                "encoded stream ended early"
            );
        }
        if (decoded->width != static_cast<std::int32_t>(layer.canvasWidth)
            || decoded->height != static_cast<std::int32_t>(layer.canvasHeight)) {
            fail(
                H264ExportFailureCode::verificationFailed,
                "verifyDecode",
                "decoded output dimensions differ"
            );
        }
        requireExactTimestamp(
            *decoded,
            index,
            layer.framesPerSecond,
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
    return static_cast<std::uint64_t>(layer.durationFrames);
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

H264ProjectExportReceipt exportStaticProjectH264Impl(
    const project::ProjectDocument& document,
    const H264ProjectExportRequest& request,
    const H264ExportLimits& limits,
    std::stop_token cancellation,
    const detail::H264ExportTestHooks* hooks
) {
    const ExportLease lease(cancellation);
    checkCancellation(cancellation, "validateRequest");
    if (limits.maximumFrames == 0
        || limits.minimumBitRate <= 0
        || limits.maximumBitRate < limits.minimumBitRate
        || request.timelineId.empty()
        || request.trackId.empty()
        || request.clipId.empty()
        || !request.input.is_absolute()
        || !request.destination.is_absolute()
        || !hasMp4Extension(request.destination)
        || pathsEqual(request.input, request.destination)
        || request.bitRate < limits.minimumBitRate
        || request.bitRate > limits.maximumBitRate) {
        fail(
            H264ExportFailureCode::invalidRequest,
            "validateRequest",
            "export request or limits are invalid"
        );
    }
    requireDirectory(request.destination.parent_path());
    if (!request.replaceExisting
        && pathExists(request.destination, "validateDestination")) {
        fail(
            H264ExportFailureCode::destinationExists,
            "validateDestination",
            "destination already exists"
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
    if (layer.sourceStartFrame != 0
        || layer.canvasWidth < 2
        || layer.canvasHeight < 2
        || (layer.canvasWidth % 2) != 0
        || (layer.canvasHeight % 2) != 0) {
        fail(
            H264ExportFailureCode::unsupportedProject,
            "validateCompiledLayer",
            "compiled layer exceeds the static H.264 export contract"
        );
    }
    if (layer.durationFrames <= 0
        || static_cast<std::uint64_t>(layer.durationFrames) > limits.maximumFrames
        || layer.framesPerSecond <= 0
        || layer.framesPerSecond > 240) {
        fail(
            H264ExportFailureCode::resourceLimitExceeded,
            "validateCompiledLayer",
            "compiled layer exceeds the export resource limits"
        );
    }

    try {
        requireExactSourceProbe(
            request.input,
            layer.framesPerSecond,
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

    StagingFile staging(request.destination);
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
    codec->width = static_cast<int>(layer.canvasWidth);
    codec->height = static_cast<int>(layer.canvasHeight);
    codec->time_base = {1, layer.framesPerSecond};
    codec->framerate = {layer.framesPerSecond, 1};
    codec->pix_fmt = requireEncoderPixelFormat(encoder, codec.get());
    codec->bit_rate = request.bitRate;
    codec->gop_size = layer.framesPerSecond * 2;
    codec->max_b_frames = 0;
    codec->color_primaries = AVCOL_PRI_BT709;
    codec->color_trc = AVCOL_TRC_BT709;
    codec->colorspace = AVCOL_SPC_BT709;
    codec->color_range = AVCOL_RANGE_MPEG;
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

    const std::uint64_t pixelCount = static_cast<std::uint64_t>(layer.canvasWidth)
        * static_cast<std::uint64_t>(layer.canvasHeight);
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(pixelCount * 4));
    std::unique_ptr<media::FfmpegVideoFrameReader> reader;
    try {
        reader = std::make_unique<media::FfmpegVideoFrameReader>(
            request.input,
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
    render::CpuRenderer renderer;
    for (std::int64_t index = 0; index < layer.durationFrames; ++index) {
        checkCancellation(cancellation, "decodeSource");
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
                "source ended before the compiled clip"
            );
        }
        requireExactTimestamp(
            *decoded,
            index,
            layer.framesPerSecond,
            "decodeSource",
            H264ExportFailureCode::unsupportedSourceTiming
        );
        const auto source = [&] {
            try {
                return media::makeRenderSourceFrame(*decoded, cancellation);
            } catch (const media::RenderSourceError& error) {
                fail(
                    error.code == "cancelled"
                        ? H264ExportFailureCode::cancelled
                        : H264ExportFailureCode::encodeFailed,
                    "adaptSourceFrame",
                    error.code + " at " + error.pointer
                );
            }
        }();
        const std::int64_t timelineFrame = layer.timelineStartFrame + index;
        const auto plan = project_render::makeRenderPlan(layer, timelineFrame);
        const auto resolver = [&](std::string_view mediaId, std::int64_t sourceFrame)
            -> const render::SourceFrame* {
            return mediaId == layer.mediaId && sourceFrame == index ? &source : nullptr;
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
    staging.lockForVerification();

    std::uint64_t verifiedFrames = 0;
    try {
        verifiedFrames = verifyOutput(staging.path(), layer, cancellation);
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
        request.destination,
        encoder->name,
        static_cast<std::uint64_t>(layer.durationFrames),
        verifiedFrames,
        layer.canvasWidth,
        layer.canvasHeight,
        layer.framesPerSecond,
    };
    runCheckpoint(hooks, "beforeInstall", cancellation);
    staging.install(request.destination, request.replaceExisting);
    return receipt;
    } catch (...) {
        staging.cleanup();
        throw;
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

namespace detail {

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

}

}
