#include "media_test_fixtures.hpp"
#include "palmier/media/presentation_audio_decode_pump.hpp"
#include "wasapi_output_backend.hpp"

#include <Windows.h>
#include <audioclient.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace {

using palmier::audio::PcmFormat;
using palmier::audio::PcmSampleEncoding;
using palmier::audio::WasapiClockReading;
using palmier::audio::WasapiOutputBackend;
using palmier::audio::WasapiOutputConfig;
using palmier::audio::WasapiOutputState;
using palmier::audio::WasapiOutputStateMachine;
using palmier::audio::WasapiPcmQueue;
using palmier::media::FfmpegAudioFrameReader;
using palmier::media::MediaError;
using palmier::media::MediaFailureCode;
using palmier::media::PresentationAudioDecodeLimits;
using palmier::media::PresentationAudioDecodePump;
using palmier::media::PresentationAudioDecodeState;
using palmier::media::PresentationAudioOutcome;

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

class TemporaryMedia final {
public:
    TemporaryMedia()
        : path_(std::filesystem::temp_directory_path()
            / ("palmier-audio-pipeline-" + std::to_string(GetCurrentProcessId()))) {
        std::filesystem::create_directory(path_);
        const auto bytes = decodeBase64(
            palmier::media::test_fixtures::patternedPcmWav
        );
        input_ = path_ / "patterned-pcm.wav";
        std::ofstream output(input_, std::ios::binary | std::ios::trunc);
        require(output.is_open(), "audio fixture could not be opened");
        output.write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size())
        );
        require(output.good(), "audio fixture could not be written");
    }

    ~TemporaryMedia() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& input() const noexcept { return input_; }
    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
    std::filesystem::path input_;
};

class CapturingBackend final : public WasapiOutputBackend {
public:
    explicit CapturingBackend(std::uint16_t blockAlign)
        : lease(static_cast<std::size_t>(100) * blockAlign),
        blockAlign_(blockAlign) {
        captured.reserve(static_cast<std::size_t>(1'536) * blockAlign);
        acquiredFrames.reserve(32);
        releasedFrames.reserve(32);
    }

    HRESULT waitForRenderEvent(
        std::stop_token,
        std::uint32_t
    ) noexcept override {
        return S_OK;
    }

    HRESULT loadCurrentPadding(std::uint32_t& paddingFrames) noexcept override {
        paddingFrames = padding;
        return S_OK;
    }

    HRESULT acquireBuffer(
        std::uint32_t frameCount,
        std::byte*& data
    ) noexcept override {
        acquiredFrames.push_back(frameCount);
        activeFrames_ = frameCount;
        const std::size_t byteCount = static_cast<std::size_t>(frameCount)
            * blockAlign_;
        if (byteCount > lease.size()) {
            data = nullptr;
            return E_UNEXPECTED;
        }
        std::fill(lease.begin(), lease.begin() + static_cast<std::ptrdiff_t>(byteCount), std::byte{0});
        data = lease.data();
        return S_OK;
    }

    HRESULT releaseBuffer(std::uint32_t frameCount, DWORD flags) noexcept override {
        releasedFrames.push_back(frameCount);
        if (frameCount == 0) {
            activeFrames_ = 0;
            return S_OK;
        }
        if (frameCount != activeFrames_) {
            return AUDCLNT_E_INVALID_SIZE;
        }
        if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) == 0) {
            const auto byteCount = static_cast<std::size_t>(frameCount) * blockAlign_;
            captured.insert(
                captured.end(),
                lease.begin(),
                lease.begin() + static_cast<std::ptrdiff_t>(byteCount)
            );
        }
        activeFrames_ = 0;
        return S_OK;
    }

    HRESULT start() noexcept override {
        started = true;
        return S_OK;
    }

    HRESULT loadClockPosition(WasapiClockReading& reading) noexcept override {
        reading = {clockPosition, clockPosition * 100, false};
        return S_OK;
    }

    HRESULT stop() noexcept override {
        stopped = true;
        return S_OK;
    }

    HRESULT reset() noexcept override { return S_OK; }
    HRESULT close() noexcept override { return S_OK; }

    std::uint32_t padding{};
    std::uint64_t clockPosition{};
    std::vector<std::byte> lease;
    std::vector<std::byte> captured;
    std::vector<std::uint32_t> acquiredFrames;
    std::vector<std::uint32_t> releasedFrames;
    bool started{};
    bool stopped{};

private:
    std::uint16_t blockAlign_{};
    std::uint32_t activeFrames_{};
};

PcmFormat stereo48k() {
    return {
        48'000,
        2,
        16,
        16,
        4,
        0x3,
        PcmSampleEncoding::integer,
        true,
    };
}

std::vector<std::byte> decodeExpected(
    const std::filesystem::path& input,
    const PcmFormat& format
) {
    FfmpegAudioFrameReader reader(input, format);
    std::vector<std::byte> bytes;
    std::int64_t nextSample = 0;
    for (;;) {
        auto block = reader.nextBlock();
        if (!block.has_value()) {
            break;
        }
        require(block->startOutputSample == nextSample, "direct PCM cursor skipped");
        nextSample += block->frameCount;
        bytes.insert(
            bytes.end(),
            block->interleavedBytes.begin(),
            block->interleavedBytes.end()
        );
    }
    require(nextSample == 1'536, "direct resampled frame count changed");
    return bytes;
}

void boundedPipelinePreservesEveryMediaSample(const TemporaryMedia& media) {
    const auto format = stereo48k();
    const auto expected = decodeExpected(media.input(), format);
    PresentationAudioDecodePump pump({format, 150, 50, {}});
    require(
        pump.start(1, media.input()).outcome == PresentationAudioOutcome::changed,
        "audio pump did not start"
    );

    WasapiPcmQueue pcmQueue(250, format);
    CapturingBackend backend(format.blockAlign);
    WasapiOutputStateMachine output(
        WasapiOutputConfig{100, format, 48'000, 1},
        backend,
        pcmQueue
    );

    bool eosMarked = false;
    auto feed = [&] {
        for (std::size_t attempt = 0; attempt < 100; ++attempt) {
            if (pcmQueue.freeFrames() < 50) {
                return;
            }
            auto take = pump.dequeue(1);
            if (take.outcome == PresentationAudioOutcome::changed) {
                require(take.block.has_value(), "changed take has no block");
                if (take.block->frameCount > pcmQueue.freeFrames()) {
                    throw std::runtime_error("test handoff dequeued beyond free capacity");
                }
                require(
                    pcmQueue.enqueue(take.block->interleavedBytes),
                    "bounded PCM enqueue failed"
                );
                continue;
            }
            if (pump.state() != PresentationAudioDecodeState::endOfStream) {
                pump.fill(1);
                continue;
            }
            if (!eosMarked) {
                pcmQueue.markEndOfStream();
                eosMarked = true;
            }
            return;
        }
        throw std::runtime_error("audio handoff did not converge");
    };

    feed();
    require(output.start().currentState == WasapiOutputState::running, "output did not start");
    for (std::size_t iteration = 0; iteration < 100; ++iteration) {
        feed();
        const auto receipt = output.renderOnce(10);
        backend.clockPosition += receipt.releasedFrames;
        if (output.state() == WasapiOutputState::completed) {
            break;
        }
    }

    require(output.state() == WasapiOutputState::completed, "EOS did not complete");
    require(backend.started && backend.stopped, "output lifecycle did not close EOS");
    require(backend.captured == expected, "bounded WASAPI lease changed PCM bytes");
    require(
        !backend.acquiredFrames.empty() && backend.acquiredFrames.back() == 36,
        "EOS did not request the exact final packet"
    );
    require(
        backend.releasedFrames.back() == backend.acquiredFrames.back(),
        "EOS release size differs from its lease"
    );
}

void pumpBackpressureCancellationAndReplacement(const TemporaryMedia& media) {
    const auto format = stereo48k();
    PresentationAudioDecodePump pump({format, 100, 50, {}});
    require(pump.start(7, media.input()).outcome == PresentationAudioOutcome::changed, "start failed");
    require(pump.fill(7).admittedFrames == 50, "first bounded fill changed");
    require(pump.fill(7).admittedFrames == 50, "second bounded fill changed");
    const auto blocked = pump.fill(7);
    require(blocked.outcome == PresentationAudioOutcome::refused, "full pump did not block");
    require(blocked.hasPendingBlock, "backpressure lost pending audio");

    const auto beforeFailedReplacement = blocked.queuedFrames;
    try {
        pump.start(8, media.path() / "missing.wav");
        throw std::runtime_error("missing replacement unexpectedly opened");
    } catch (const MediaError& error) {
        require(error.code == MediaFailureCode::openFailed, "wrong replacement failure");
    }
    require(pump.generation() == 7, "failed replacement changed generation");
    require(
        pump.fill(7).queuedFrames == beforeFailedReplacement,
        "failed replacement changed queued audio"
    );

    require(pump.start(8, media.input()).outcome == PresentationAudioOutcome::changed, "replacement failed");
    require(pump.fill(8).admittedFrames == 50, "replacement did not decode");
    std::stop_source source;
    source.request_stop();
    try {
        pump.fill(8, source.get_token());
        throw std::runtime_error("cancelled fill unexpectedly succeeded");
    } catch (const MediaError& error) {
        require(error.code == MediaFailureCode::cancelled, "wrong cancellation error");
    }
    require(pump.state() == PresentationAudioDecodeState::cancelled, "cancel not terminal");
    require(pump.dequeue(8).outcome == PresentationAudioOutcome::noOp, "cancel kept PCM");
}

void rejectsUnboundedPumpLimits() {
    const auto format = stereo48k();
    try {
        static_cast<void>(PresentationAudioDecodePump({
            format,
            4'194'305,
            1'024,
            {},
        }));
        throw std::runtime_error("excessive audio capacity was accepted");
    } catch (const palmier::media::PresentationAudioDecodeError& error) {
        require(
            error.code
                == palmier::media::PresentationAudioDecodeErrorCode::invalidLimits,
            "wrong excessive capacity error"
        );
    }

    try {
        static_cast<void>(PresentationAudioDecodePump({
            format,
            65'537,
            65'537,
            {},
        }));
        throw std::runtime_error("excessive audio fill budget was accepted");
    } catch (const palmier::media::PresentationAudioDecodeError& error) {
        require(
            error.code
                == palmier::media::PresentationAudioDecodeErrorCode::invalidLimits,
            "wrong excessive fill error"
        );
    }

    auto excessiveDecode = PresentationAudioDecodeLimits{
        format,
        4'096,
        1'024,
        {},
    };
    excessiveDecode.decode.maximumAudioFramesPerBlock = 65'537;
    try {
        static_cast<void>(PresentationAudioDecodePump(excessiveDecode));
        throw std::runtime_error("excessive decoded audio block was accepted");
    } catch (const palmier::media::PresentationAudioDecodeError& error) {
        require(
            error.code
                == palmier::media::PresentationAudioDecodeErrorCode::invalidLimits,
            "wrong excessive decode block error"
        );
    }
}

}

int main() {
    try {
        TemporaryMedia media;
        boundedPipelinePreservesEveryMediaSample(media);
        pumpBackpressureCancellationAndReplacement(media);
        rejectsUnboundedPumpLimits();
        std::cout << "FFmpeg to bounded WASAPI audio pipeline tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
