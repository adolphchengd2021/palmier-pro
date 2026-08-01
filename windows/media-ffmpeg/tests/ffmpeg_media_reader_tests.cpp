#include "media_test_fixtures.hpp"
#include "palmier/media/ffmpeg_media_reader.hpp"
#include "palmier/media/render_source_adapter.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace {

using palmier::media::AlphaMode;
using palmier::media::DecodeLimits;
using palmier::media::FfmpegMediaReader;
using palmier::media::FfmpegAudioFrameReader;
using palmier::media::FfmpegVideoFrameReader;
using palmier::media::MediaError;
using palmier::media::MediaFailureCode;
using palmier::media::RenderSourceError;
using palmier::media::StreamKind;
using palmier::audio::PcmFormat;
using palmier::audio::PcmSampleEncoding;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::vector<std::uint8_t> decodeBase64(std::string_view input) {
    constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::array<std::int16_t, 256> table{};
    table.fill(-1);
    for (std::size_t index = 0; index < alphabet.size(); ++index) {
        table[static_cast<std::uint8_t>(alphabet[index])] =
            static_cast<std::int16_t>(index);
    }

    std::vector<std::uint8_t> output;
    output.reserve(input.size() * 3 / 4);
    std::uint32_t accumulator = 0;
    int bitCount = 0;
    for (const char character : input) {
        if (character == '=') {
            break;
        }
        const auto value = table[static_cast<std::uint8_t>(character)];
        require(value >= 0, "fixture contains invalid base64");
        accumulator = (accumulator << 6) | static_cast<std::uint32_t>(value);
        bitCount += 6;
        if (bitCount >= 8) {
            bitCount -= 8;
            output.push_back(static_cast<std::uint8_t>(
                (accumulator >> bitCount) & 0xFFU
            ));
        }
    }
    return output;
}

class TemporaryDirectory final {
public:
    TemporaryDirectory()
        : path_(std::filesystem::temp_directory_path()
            / ("palmier-media-" + std::to_string(GetCurrentProcessId()))) {
        std::filesystem::create_directory(path_);
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    std::filesystem::path write(
        std::string_view name,
        std::string_view base64
    ) const {
        return write(name, decodeBase64(base64));
    }

    std::filesystem::path write(
        std::string_view name,
        const std::vector<std::uint8_t>& bytes
    ) const {
        const auto destination = path_ / name;
        std::ofstream output(destination, std::ios::binary | std::ios::trunc);
        require(output.is_open(), "fixture file could not be opened");
        output.write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size())
        );
        require(output.good(), "fixture file could not be written");
        return destination;
    }

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

std::vector<std::uint8_t> replaceBytes(
    std::string_view base64,
    const std::vector<std::uint8_t>& pattern,
    const std::vector<std::uint8_t>& replacement
) {
    require(pattern.size() == replacement.size(), "replacement size differs");
    auto bytes = decodeBase64(base64);
    const auto position = std::search(
        bytes.begin(),
        bytes.end(),
        pattern.begin(),
        pattern.end()
    );
    require(position != bytes.end(), "fixture mutation pattern is missing");
    std::copy(replacement.begin(), replacement.end(), position);
    return bytes;
}

template<typename Operation>
void requireError(Operation operation, MediaFailureCode code) {
    try {
        operation();
    } catch (const MediaError& error) {
        require(error.code == code, "unexpected media error code");
        return;
    }
    throw std::runtime_error("expected media failure");
}

void dependencyContract() {
    const auto info = FfmpegMediaReader::runtimeInfo();
    require(info.version == "8.1.2", "unexpected FFmpeg runtime version");
    require(info.headersMatchRuntime, "FFmpeg headers and DLLs differ");
    require(info.license.find("LGPL") != std::string::npos, "FFmpeg is not LGPL");
    require(
        info.configuration.find("--enable-gpl") == std::string::npos,
        "FFmpeg GPL build is outside the prototype contract"
    );
    require(
        info.configuration.find("--enable-nonfree") == std::string::npos,
        "FFmpeg nonfree build is outside the prototype contract"
    );
}

void probesH264AndAac(const std::filesystem::path& input) {
    const auto probe = FfmpegMediaReader::probe(input);
    require(probe.containerName.find("mov") != std::string::npos, "wrong container");
    require(probe.durationMicroseconds == 500'000, "wrong container duration");
    require(probe.streams.size() == 2, "expected video and audio streams");

    const auto& video = probe.streams[0];
    require(video.kind == StreamKind::video, "first stream is not video");
    require(video.codecName == "h264", "video codec is not H.264");
    require(video.width == 16 && video.height == 16, "wrong video size");
    require(video.averageFrameRate.numerator == 10, "wrong average frame rate");

    const auto& audio = probe.streams[1];
    require(audio.kind == StreamKind::audio, "second stream is not audio");
    require(audio.codecName == "aac", "audio codec is not AAC");
    require(audio.sampleRate == 48'000, "wrong audio sample rate");
    require(audio.channelCount == 2, "wrong audio channel count");
}

void decodesStraightAlphaAndRotation(const std::filesystem::path& input) {
    const auto frame = FfmpegMediaReader::decodeFirstVideoFrame(input);
    require(frame.width == 4 && frame.height == 4, "wrong decoded dimensions");
    require(frame.rowBytes == 16, "wrong decoded stride");
    require(frame.presentationTimestamp == 0, "wrong presentation timestamp");
    require(frame.timeBase.numerator == 1, "wrong time-base numerator");
    require(frame.timeBase.denominator == 16'384, "wrong time-base denominator");
    require(frame.displayTransform.has_value(), "rotation metadata is missing");
    require(
        frame.displayTransform->counterClockwiseDegrees == 90,
        "wrong display rotation"
    );

    const std::array<std::uint8_t, 64> expected{
        255, 0, 0, 255, 255, 0, 0, 255, 0, 255, 0, 128, 0, 255, 0, 128,
        255, 0, 0, 255, 255, 0, 0, 255, 0, 255, 0, 128, 0, 255, 0, 128,
        0, 0, 255, 64, 0, 0, 255, 64, 255, 255, 0, 255, 255, 255, 0, 255,
        0, 0, 255, 64, 0, 0, 255, 64, 255, 255, 0, 255, 255, 255, 0, 255,
    };
    require(
        std::equal(
            frame.rgba8.begin(),
            frame.rgba8.end(),
            expected.begin(),
            expected.end()
        ),
        "decoded RGBA pixels differ"
    );
    require(frame.alphaMode == AlphaMode::unspecified, "wrong alpha mode");
    try {
        palmier::media::makeRenderSourceFrame(frame);
    } catch (const RenderSourceError& error) {
        require(error.code == "unsupportedAlphaMode", "wrong adapter refusal");
        return;
    }
    throw std::runtime_error("unknown alpha reached the render source");
}

void decodesPresentationOrderedFrames(const std::filesystem::path& input) {
    constexpr std::array<std::int64_t, 3> timestamps{0, 1'024, 2'048};
    constexpr std::array<std::uint8_t, 72> expected{
        255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255,
        255, 255, 255, 255, 0, 0, 0, 255, 255, 255, 0, 255,
        0, 255, 255, 255, 255, 0, 255, 255, 128, 128, 128, 255,
        255, 128, 0, 255, 0, 128, 128, 255, 128, 0, 128, 255,
        64, 0, 0, 255, 0, 64, 0, 255, 0, 0, 64, 255,
        32, 64, 96, 255, 96, 64, 32, 255, 200, 100, 50, 255,
    };

    FfmpegVideoFrameReader reader(input);
    for (std::size_t index = 0; index < timestamps.size(); ++index) {
        const auto frame = reader.nextFrame();
        require(frame.has_value(), "multi-frame fixture ended early");
        require(frame->width == 3 && frame->height == 2, "wrong multi-frame size");
        require(frame->rowBytes == 12, "wrong multi-frame stride");
        require(
            frame->presentationTimestamp == timestamps[index],
            "wrong multi-frame presentation timestamp"
        );
        require(
            frame->timeBase.numerator == 1
                && frame->timeBase.denominator == 10'240,
            "wrong multi-frame time base"
        );
        require(frame->alphaMode == AlphaMode::opaque, "multi-frame alpha is not opaque");
        const auto expectedStart = expected.begin() + static_cast<std::ptrdiff_t>(index * 24);
        require(
            std::equal(
                frame->rgba8.begin(),
                frame->rgba8.end(),
                expectedStart,
                expectedStart + 24
            ),
            "multi-frame RGBA pixels differ"
        );
    }
    require(!reader.nextFrame().has_value(), "multi-frame fixture has extra frames");
    require(!reader.nextFrame().has_value(), "end of stream is not stable");

    const auto first = FfmpegMediaReader::decodeFirstVideoFrame(input);
    require(first.presentationTimestamp == 0, "first-frame compatibility changed");
    require(
        std::equal(first.rgba8.begin(), first.rgba8.end(), expected.begin(), expected.begin() + 24),
        "first-frame compatibility pixels differ"
    );
}

void cancellationTerminatesCursor(const std::filesystem::path& input) {
    FfmpegVideoFrameReader reader(input);
    require(reader.nextFrame().has_value(), "cursor did not decode its first frame");
    std::stop_source source;
    source.request_stop();
    requireError(
        [&] { static_cast<void>(reader.nextFrame(source.get_token())); },
        MediaFailureCode::cancelled
    );
    requireError(
        [&] { static_cast<void>(reader.nextFrame()); },
        MediaFailureCode::cancelled
    );
}

std::int16_t patternedSample(std::size_t index) {
    constexpr std::array<std::array<std::int16_t, 8>, 3> patterns{{
        {0, 4096, -4096, 8192, -8192, 16384, -16384, 32767},
        {-32768, 24576, -24576, 12288, -12288, 2048, -2048, 1},
        {30000, -30000, 20000, -20000, 10000, -10000, 12345, -12345},
    }};
    return patterns[index / 256][index % 8];
}

std::int16_t sampleAt(
    const std::vector<std::byte>& bytes,
    std::size_t sampleIndex
) {
    std::int16_t value{};
    std::memcpy(
        &value,
        bytes.data() + sampleIndex * sizeof(value),
        sizeof(value)
    );
    return value;
}

void decodesExactCanonicalPcm(const std::filesystem::path& input) {
    const PcmFormat mono24k{
        24'000,
        1,
        16,
        16,
        2,
        0x4,
        PcmSampleEncoding::integer,
        true,
    };
    FfmpegAudioFrameReader reader(input, mono24k);
    std::vector<std::byte> decoded;
    std::int64_t nextSample = 0;
    for (;;) {
        const auto block = reader.nextBlock();
        if (!block.has_value()) {
            break;
        }
        require(block->format == mono24k, "canonical PCM format changed");
        require(block->startOutputSample == nextSample, "PCM sample cursor skipped");
        require(
            block->interleavedBytes.size()
                == static_cast<std::size_t>(block->frameCount) * mono24k.blockAlign,
            "PCM block byte count changed"
        );
        nextSample += block->frameCount;
        decoded.insert(
            decoded.end(),
            block->interleavedBytes.begin(),
            block->interleavedBytes.end()
        );
    }
    require(nextSample == 768, "canonical PCM frame count changed");
    for (std::size_t index = 0; index < 768; ++index) {
        require(sampleAt(decoded, index) == patternedSample(index), "PCM sample changed");
    }
    require(!reader.nextBlock().has_value(), "audio EOF is not stable");
}

void resamplesAndRemixesCanonicalPcm(const std::filesystem::path& input) {
    const PcmFormat stereo48k{
        48'000,
        2,
        16,
        16,
        4,
        0x3,
        PcmSampleEncoding::integer,
        true,
    };
    FfmpegAudioFrameReader reader(input, stereo48k);
    std::uint64_t frames = 0;
    std::int64_t nextSample = 0;
    for (;;) {
        const auto block = reader.nextBlock();
        if (!block.has_value()) {
            break;
        }
        require(block->startOutputSample == nextSample, "resampled cursor skipped");
        for (std::size_t frame = 0; frame < block->frameCount; ++frame) {
            require(
                sampleAt(block->interleavedBytes, frame * 2)
                    == sampleAt(block->interleavedBytes, frame * 2 + 1),
                "mono remix channels differ"
            );
        }
        frames += block->frameCount;
        nextSample += block->frameCount;
    }
    require(frames == 1'536, "resampled frame count changed");
}

void decodesPlayableAac(const std::filesystem::path& input) {
    const PcmFormat stereo48k{
        48'000,
        2,
        16,
        16,
        4,
        0x3,
        PcmSampleEncoding::integer,
        true,
    };
    FfmpegAudioFrameReader reader(input, stereo48k);
    std::uint64_t frames{};
    std::int64_t nextSample{};
    for (;;) {
        const auto block = reader.nextBlock();
        if (!block.has_value()) break;
        require(block->format == stereo48k, "AAC PCM format changed");
        require(block->startOutputSample == nextSample, "AAC sample cursor skipped");
        require(block->frameCount != 0, "AAC returned an empty block");
        require(
            block->interleavedBytes.size()
                == static_cast<std::size_t>(block->frameCount) * stereo48k.blockAlign,
            "AAC block byte count changed"
        );
        frames += block->frameCount;
        nextSample += block->frameCount;
    }
    require(frames == 24'576, "AAC decoded frame count changed");
    require(!reader.nextBlock().has_value(), "AAC EOF is not stable");
}

void audioCursorCancellationIsTerminal(const std::filesystem::path& input) {
    const PcmFormat format{
        24'000,
        1,
        16,
        16,
        2,
        0x4,
        PcmSampleEncoding::integer,
        true,
    };
    FfmpegAudioFrameReader reader(input, format);
    require(reader.nextBlock().has_value(), "audio cursor produced no block");
    std::stop_source source;
    source.request_stop();
    requireError(
        [&] { static_cast<void>(reader.nextBlock(source.get_token())); },
        MediaFailureCode::cancelled
    );
    requireError(
        [&] { static_cast<void>(reader.nextBlock()); },
        MediaFailureCode::cancelled
    );
}

void validatesAudioFailureBoundaries(const std::filesystem::path& videoOnly) {
    const PcmFormat valid{
        48'000,
        2,
        16,
        16,
        4,
        0x3,
        PcmSampleEncoding::integer,
        true,
    };
    requireError(
        [&] { static_cast<void>(FfmpegAudioFrameReader(videoOnly, valid)); },
        MediaFailureCode::noAudioStream
    );
    requireError(
        [&] { static_cast<void>(FfmpegAudioFrameReader(videoOnly, {})); },
        MediaFailureCode::unsupportedAudioFormat
    );
    auto unidentifiedMultichannel = valid;
    unidentifiedMultichannel.channelCount = 6;
    unidentifiedMultichannel.blockAlign = 12;
    unidentifiedMultichannel.channelMask = 0;
    requireError(
        [&] {
            static_cast<void>(FfmpegAudioFrameReader(
                videoOnly,
                unidentifiedMultichannel
            ));
        },
        MediaFailureCode::unsupportedAudioFormat
    );
    DecodeLimits invalidLimits;
    invalidLimits.maximumAudioFramesPerBlock = 0;
    requireError(
        [&] {
            static_cast<void>(FfmpegAudioFrameReader(videoOnly, valid, invalidLimits));
        },
        MediaFailureCode::invalidLimits
    );
    invalidLimits.maximumAudioFramesPerBlock = 65'537;
    requireError(
        [&] {
            static_cast<void>(FfmpegAudioFrameReader(videoOnly, valid, invalidLimits));
        },
        MediaFailureCode::invalidLimits
    );
}

void validatesFailureBoundaries(
    const TemporaryDirectory& directory,
    const std::filesystem::path& qtrle,
    const std::filesystem::path& audioOnly
) {
    requireError(
        [&] { FfmpegMediaReader::probe(directory.path() / "missing.mov"); },
        MediaFailureCode::openFailed
    );
    requireError(
        [&] { FfmpegMediaReader::probe("https://example.invalid/media.mp4"); },
        MediaFailureCode::unsupportedInputProtocol
    );
    requireError(
        [&] { FfmpegMediaReader::probe(R"(\\?\C:\palmier-missing\media.mov)"); },
        MediaFailureCode::openFailed
    );
    requireError(
        [&] { FfmpegMediaReader::probe(R"(\\.\PhysicalDrive0)"); },
        MediaFailureCode::unsupportedInputProtocol
    );

    DecodeLimits invalidLimits;
    invalidLimits.maximumProbeBytes = 0;
    requireError(
        [&] { FfmpegMediaReader::probe(qtrle, invalidLimits); },
        MediaFailureCode::invalidLimits
    );

    std::stop_source source;
    source.request_stop();
    requireError(
        [&] { FfmpegMediaReader::probe(qtrle, {}, source.get_token()); },
        MediaFailureCode::cancelled
    );

    DecodeLimits limits;
    limits.maximumPixels = 15;
    requireError(
        [&] { FfmpegMediaReader::decodeFirstVideoFrame(qtrle, limits); },
        MediaFailureCode::resourceLimitExceeded
    );

    requireError(
        [&] { FfmpegMediaReader::decodeFirstVideoFrame(audioOnly); },
        MediaFailureCode::noVideoStream
    );

    const auto unsupportedColor = directory.write(
        "unsupported-color.mov",
        replaceBytes(
            palmier::media::test_fixtures::qtrleAlphaRotated,
            {0x63, 0x6F, 0x6C, 0x72, 0x6E, 0x63, 0x6C, 0x63,
             0x00, 0x01, 0x00, 0x0D, 0x00, 0x00},
            {0x63, 0x6F, 0x6C, 0x72, 0x6E, 0x63, 0x6C, 0x63,
             0x00, 0x01, 0x00, 0x10, 0x00, 0x00}
        )
    );
    requireError(
        [&] { FfmpegMediaReader::decodeFirstVideoFrame(unsupportedColor); },
        MediaFailureCode::unsupportedColorMetadata
    );

    const auto unsupportedMatrix = directory.write(
        "unsupported-matrix.mov",
        replaceBytes(
            palmier::media::test_fixtures::qtrleAlphaRotated,
            {0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00,
             0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
             0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
             0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
             0x40, 0x00, 0x00, 0x00},
            {0x00, 0x00, 0x01, 0x00, 0xFF, 0xFF, 0x00, 0x00,
             0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
             0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
             0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
             0x40, 0x00, 0x00, 0x00}
        )
    );
    requireError(
        [&] { FfmpegMediaReader::decodeFirstVideoFrame(unsupportedMatrix); },
        MediaFailureCode::unsupportedDisplayTransform
    );
}

}

int main() {
    try {
        TemporaryDirectory directory;
        const auto qtrle = directory.write(
            "alpha-rotated.mov",
            palmier::media::test_fixtures::qtrleAlphaRotated
        );
        const auto h264Aac = directory.write(
            "h264-aac.mp4",
            palmier::media::test_fixtures::h264Aac
        );
        const auto audioOnly = directory.write(
            "audio-only.wav",
            palmier::media::test_fixtures::audioOnlyWav
        );
        const auto opaqueThreeFrames = directory.write(
            "opaque-three-frames.mov",
            palmier::media::test_fixtures::qtrleOpaqueThreeFrames
        );
        const auto patternedPcm = directory.write(
            "patterned-pcm.wav",
            palmier::media::test_fixtures::patternedPcmWav
        );
        dependencyContract();
        probesH264AndAac(h264Aac);
        decodesPlayableAac(h264Aac);
        decodesStraightAlphaAndRotation(qtrle);
        decodesPresentationOrderedFrames(opaqueThreeFrames);
        cancellationTerminatesCursor(opaqueThreeFrames);
        decodesExactCanonicalPcm(patternedPcm);
        resamplesAndRemixesCanonicalPcm(patternedPcm);
        audioCursorCancellationIsTerminal(patternedPcm);
        validatesAudioFailureBoundaries(qtrle);
        validatesFailureBoundaries(directory, qtrle, audioOnly);
        std::cout << "FFmpeg media reader tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
