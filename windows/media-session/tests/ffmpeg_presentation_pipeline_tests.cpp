#include "media_test_fixtures.hpp"
#include "media_test_support.hpp"
#include "internal/presentation_video_decode_pump_testing.hpp"
#include "palmier/media/presentation_video_decode_pump.hpp"
#include "palmier/render/cpu_renderer.hpp"
#include "palmier/render/d3d11_warp_renderer.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

namespace {

using palmier::media::MediaError;
using palmier::media::MediaFailureCode;
using palmier::media::PresentationVideoDecodeLimits;
using palmier::media::PresentationVideoDecodeError;
using palmier::media::PresentationVideoDecodeErrorCode;
using palmier::media::PresentationVideoDecodePump;
using palmier::media::PresentationVideoDecodeState;
using palmier::media::PresentationVideoOutcome;
using palmier::media::PresentationVideoTake;
using palmier::media::test_support::TemporaryDirectory;
using palmier::media::test_support::require;
using palmier::media::test_support::replaceBytes;
using palmier::render::RenderedFrame;
using palmier::render::Rgba32Float;
using palmier::render::SourceFrame;

template<typename Operation>
void requireMediaError(Operation operation, MediaFailureCode code) {
    try {
        operation();
    } catch (const MediaError& error) {
        require(error.code == code, "unexpected pipeline media error code");
        return;
    }
    throw std::runtime_error("expected pipeline media failure");
}

template<typename Operation>
void requirePumpError(
    Operation operation,
    PresentationVideoDecodeErrorCode code
) {
    try {
        operation();
    } catch (const PresentationVideoDecodeError& error) {
        require(error.code == code, "unexpected pipeline error code");
        return;
    }
    throw std::runtime_error("expected pipeline failure");
}

PresentationVideoDecodeLimits limits(std::size_t maximumFrames) {
    PresentationVideoDecodeLimits result;
    result.buffer.maximumFrames = maximumFrames;
    result.buffer.maximumBytes = maximumFrames * 3ULL * 2ULL
        * sizeof(palmier::render::Rgba32Float);
    return result;
}

PresentationVideoTake requireFrame(
    PresentationVideoTake take,
    const std::string& message
) {
    require(take.frame.has_value(), message);
    return take;
}

Rgba32Float pixel(
    std::uint8_t red,
    std::uint8_t green,
    std::uint8_t blue
) {
    constexpr float scale = 1.0F / 255.0F;
    return {red * scale, green * scale, blue * scale, 1};
}

void requirePixel(
    const Rgba32Float& actual,
    const Rgba32Float& expected,
    float tolerance,
    const std::string& message
) {
    const float actualChannels[]{actual.red, actual.green, actual.blue, actual.alpha};
    const float expectedChannels[]{expected.red, expected.green, expected.blue, expected.alpha};
    for (std::size_t channel = 0; channel < 4; ++channel) {
        if (std::abs(actualChannels[channel] - expectedChannels[channel]) > tolerance) {
            throw std::runtime_error(message);
        }
    }
}

void compareFrames(
    const RenderedFrame& expected,
    const RenderedFrame& actual,
    float tolerance
) {
    require(expected.width == actual.width && expected.height == actual.height, "frame size drift");
    require(expected.pixels.size() == actual.pixels.size(), "frame storage drift");
    for (std::size_t index = 0; index < expected.pixels.size(); ++index) {
        const float expectedChannels[]{
            expected.pixels[index].red,
            expected.pixels[index].green,
            expected.pixels[index].blue,
            expected.pixels[index].alpha,
        };
        const float actualChannels[]{
            actual.pixels[index].red,
            actual.pixels[index].green,
            actual.pixels[index].blue,
            actual.pixels[index].alpha,
        };
        for (std::size_t channel = 0; channel < 4; ++channel) {
            if (std::abs(expectedChannels[channel] - actualChannels[channel]) > tolerance) {
                throw std::runtime_error("pipeline CPU/WARP drift");
            }
        }
    }
}

void renderThroughSharedEntryPoints(
    const PresentationVideoTake& take,
    const Rgba32Float& expectedFirst,
    const Rgba32Float& expectedLast
) {
    require(take.frame.has_value(), "pipeline dequeue did not return a frame");
    const auto& source = take.frame->source;
    requirePixel(source.pixels.front(), expectedFirst, 1e-6F, "pipeline source first pixel differs");
    requirePixel(source.pixels.back(), expectedLast, 1e-6F, "pipeline source last pixel differs");
    const auto plan = palmier::render::RenderPlan::create(
        source.width,
        source.height,
        10,
        0,
        {{
            "decoded",
            "video-track",
            "decoded-media",
            0,
            {},
            1,
            palmier::render::BlendMode::normal,
            std::nullopt,
        }}
    );
    const auto resolver = [&](std::string_view mediaId, std::int64_t sourceFrame)
        -> const SourceFrame* {
        return mediaId == "decoded-media" && sourceFrame == 0 ? &source : nullptr;
    };

    palmier::render::CpuRenderer cpu;
    const auto cpuPreview = palmier::render::renderPreviewFrame(plan, resolver, cpu);
    const auto cpuExport = palmier::render::renderExportFrame(plan, resolver, cpu);
    require(cpuPreview.pixels == cpuExport.pixels, "pipeline CPU preview/export differ");

    palmier::render::D3d11WarpRenderer warp;
    const auto warpPreview = palmier::render::renderPreviewFrame(plan, resolver, warp);
    const auto warpExport = palmier::render::renderExportFrame(plan, resolver, warp);
    require(warpPreview.pixels == warpExport.pixels, "pipeline WARP preview/export differ");
    compareFrames(cpuPreview, warpPreview, 2e-5F);
    requirePixel(warpPreview.pixels.front(), expectedFirst, 2e-5F, "pipeline WARP first pixel differs");
    requirePixel(warpPreview.pixels.back(), expectedLast, 2e-5F, "pipeline WARP last pixel differs");
}

void realFramesReachSharedRenderers(const std::filesystem::path& input) {
    PresentationVideoDecodePump pump(limits(3));
    const auto start = pump.start(1, input);
    require(start.outcome == PresentationVideoOutcome::changed, "pipeline did not start");
    const auto fill = pump.fill(1);
    require(fill.state == PresentationVideoDecodeState::endOfStream, "pipeline did not reach EOS");
    require(fill.admittedFrames == 3, "pipeline did not admit three frames");
    require(fill.queuedFrames == 3 && !fill.hasPendingFrame, "pipeline queue receipt differs");
    const auto repeatedEnd = pump.fill(1);
    require(
        repeatedEnd.state == PresentationVideoDecodeState::endOfStream
            && repeatedEnd.outcome == PresentationVideoOutcome::noOp,
        "pipeline end of stream is not stable"
    );

    constexpr std::array<std::int64_t, 3> expectedTimestamps{0, 1'024, 2'048};
    const std::array<Rgba32Float, 3> expectedFirst{
        pixel(255, 0, 0),
        pixel(0, 255, 255),
        pixel(64, 0, 0),
    };
    const std::array<Rgba32Float, 3> expectedLast{
        pixel(255, 255, 0),
        pixel(128, 0, 128),
        pixel(200, 100, 50),
    };
    for (std::size_t index = 0; index < expectedTimestamps.size(); ++index) {
        const auto take = requireFrame(pump.dequeue(1), "pipeline lost a decoded frame");
        require(take.frame->generation == 1, "pipeline returned the wrong generation");
        require(
            take.frame->presentationTimestamp == expectedTimestamps[index],
            "pipeline reordered timestamps"
        );
        renderThroughSharedEntryPoints(take, expectedFirst[index], expectedLast[index]);
    }
    require(!pump.dequeue(1).frame.has_value(), "pipeline returned an extra frame");
}

void capacityRetainsOnePendingFrame(const std::filesystem::path& input) {
    PresentationVideoDecodePump pump(limits(1));
    pump.start(1, input);

    auto fill = pump.fill(1);
    require(fill.state == PresentationVideoDecodeState::blocked, "first capacity boundary did not block");
    require(fill.admittedFrames == 1 && fill.hasPendingFrame, "first pending receipt differs");
    auto take = requireFrame(pump.dequeue(1), "first capacity frame is missing");
    require(take.frame->presentationTimestamp == 0, "first capacity frame differs");

    fill = pump.fill(1);
    require(fill.state == PresentationVideoDecodeState::blocked, "second capacity boundary did not block");
    require(fill.admittedFrames == 1 && fill.hasPendingFrame, "second pending receipt differs");
    take = requireFrame(pump.dequeue(1), "second capacity frame is missing");
    require(take.frame->presentationTimestamp == 1'024, "second capacity frame differs");

    fill = pump.fill(1);
    require(fill.state == PresentationVideoDecodeState::endOfStream, "capacity pipeline did not reach EOS");
    require(fill.admittedFrames == 1 && !fill.hasPendingFrame, "final capacity receipt differs");
    take = requireFrame(pump.dequeue(1), "third capacity frame is missing");
    require(take.frame->presentationTimestamp == 2'048, "third capacity frame differs");
}

void cancellationTerminatesOnlyItsGeneration(const std::filesystem::path& input) {
    PresentationVideoDecodePump pump(limits(1));
    pump.start(1, input);
    const auto initial = pump.fill(1);
    require(
        initial.queuedFrames == 1 && initial.hasPendingFrame,
        "cancellation setup lacks queued and pending frames"
    );
    std::stop_source cancellation;
    cancellation.request_stop();
    requireMediaError(
        [&] { static_cast<void>(pump.fill(1, cancellation.get_token())); },
        MediaFailureCode::cancelled
    );
    require(pump.state() == PresentationVideoDecodeState::cancelled, "cancelled generation stayed active");
    require(!pump.dequeue(1).frame.has_value(), "cancelled generation retained queued frames");
    requirePumpError(
        [&] { static_cast<void>(pump.fill(1)); },
        PresentationVideoDecodeErrorCode::terminalState
    );

    pump.start(2, input);
    const auto fill = pump.fill(2);
    require(fill.state == PresentationVideoDecodeState::blocked, "replacement generation did not run");
    const auto replacement = requireFrame(pump.dequeue(2), "replacement first frame is missing");
    require(replacement.frame->presentationTimestamp == 0, "replacement first frame differs");
}

void decodeFailureTerminatesGeneration(const TemporaryDirectory& directory) {
    const auto unsupportedColor = directory.write(
        "unsupported-three-frames.mov",
        replaceBytes(
            palmier::media::test_fixtures::qtrleOpaqueThreeFrames,
            {0x63, 0x6F, 0x6C, 0x72, 0x6E, 0x63, 0x6C, 0x63,
             0x00, 0x01, 0x00, 0x0D, 0x00, 0x00},
            {0x63, 0x6F, 0x6C, 0x72, 0x6E, 0x63, 0x6C, 0x63,
             0x00, 0x01, 0x00, 0x10, 0x00, 0x00}
        )
    );
    PresentationVideoDecodePump pump(limits(3));
    pump.start(1, unsupportedColor);
    requireMediaError(
        [&] { static_cast<void>(pump.fill(1)); },
        MediaFailureCode::unsupportedColorMetadata
    );
    require(pump.state() == PresentationVideoDecodeState::failed, "failed generation stayed active");
    require(!pump.dequeue(1).frame.has_value(), "failed generation retained queued frames");
    requirePumpError(
        [&] { static_cast<void>(pump.fill(1)); },
        PresentationVideoDecodeErrorCode::terminalState
    );
}

void replacementClearsQueuedAndPendingFrames(const std::filesystem::path& input) {
    PresentationVideoDecodePump pump(limits(1));
    pump.start(1, input);
    const auto firstFill = pump.fill(1);
    require(firstFill.hasPendingFrame && firstFill.queuedFrames == 1, "generation one setup differs");

    requireMediaError(
        [&] { static_cast<void>(pump.start(2, input.parent_path() / "missing.mov")); },
        MediaFailureCode::openFailed
    );
    require(pump.generation() == 1, "failed replacement changed the generation");
    require(pump.state() == PresentationVideoDecodeState::blocked, "failed replacement changed state");
    const auto retained = pump.dequeue(1);
    require(retained.frame.has_value(), "failed replacement cleared the queue");
    require(retained.frame->presentationTimestamp == 0, "failed replacement changed queued data");
    const auto retainedFill = pump.fill(1);
    require(
        retainedFill.queuedFrames == 1 && retainedFill.hasPendingFrame,
        "failed replacement cleared the pending frame"
    );

    const auto replacement = pump.start(2, input);
    require(replacement.queuedFrames == 0, "replacement retained queued frames");
    const auto stale = pump.fill(1);
    require(stale.outcome == PresentationVideoOutcome::stale, "stale fill was not refused");
    const auto secondFill = pump.fill(2);
    require(secondFill.hasPendingFrame && secondFill.queuedFrames == 1, "generation two setup differs");
    const auto take = requireFrame(pump.dequeue(2), "replacement frame is missing");
    require(take.frame->generation == 2, "replacement returned a stale generation");
    require(take.frame->presentationTimestamp == 0, "replacement returned a stale frame");
}

void sameGenerationRejectsChangedInput(
    const std::filesystem::path& input,
    const std::filesystem::path& otherInput
) {
    PresentationVideoDecodePump pump(limits(3));
    pump.start(1, input);
    const auto repeated = pump.start(1, input);
    require(repeated.outcome == PresentationVideoOutcome::noOp, "same input start is not idempotent");
    requirePumpError(
        [&] { static_cast<void>(pump.start(1, otherInput)); },
        PresentationVideoDecodeErrorCode::changedInputWithinGeneration
    );
    const auto fill = pump.fill(1);
    require(fill.state == PresentationVideoDecodeState::endOfStream, "changed input refusal damaged generation");
    const auto take = requireFrame(pump.dequeue(1), "original input frame is missing");
    require(take.frame->presentationTimestamp == 0, "changed input refusal replaced media");
}

void cancellationBeforeReplacementCommitPreservesGeneration(
    const std::filesystem::path& input
) {
    std::stop_source cancellation;
    std::size_t checkpointCount = 0;
    auto pump = palmier::media::detail::PresentationVideoDecodePumpTestAccess::create(
        limits(1),
        [&] {
            ++checkpointCount;
            if (checkpointCount == 2) {
                cancellation.request_stop();
            }
        }
    );
    pump.start(1, input);
    const auto initial = pump.fill(1);
    require(initial.queuedFrames == 1 && initial.hasPendingFrame, "commit cancellation setup differs");

    requireMediaError(
        [&] { static_cast<void>(pump.start(2, input, cancellation.get_token())); },
        MediaFailureCode::cancelled
    );
    require(pump.generation() == 1, "cancelled replacement committed a generation");
    require(pump.state() == PresentationVideoDecodeState::blocked, "cancelled replacement changed state");
    auto take = requireFrame(pump.dequeue(1), "cancelled replacement cleared queued data");
    require(take.frame->presentationTimestamp == 0, "cancelled replacement changed queued data");
    const auto retained = pump.fill(1);
    require(
        retained.queuedFrames == 1 && retained.hasPendingFrame,
        "cancelled replacement cleared pending data"
    );
    take = requireFrame(pump.dequeue(1), "cancelled replacement lost pending data");
    require(take.frame->presentationTimestamp == 1'024, "cancelled replacement changed pending data");
}

void fillBudgetIsIndependentOfQueueCapacity(const std::filesystem::path& input) {
    auto bounded = limits(3);
    bounded.maximumFramesPerFill = 1;
    PresentationVideoDecodePump pump(bounded);
    pump.start(1, input);
    for (std::size_t expectedQueue = 1; expectedQueue <= 3; ++expectedQueue) {
        const auto fill = pump.fill(1);
        require(fill.state == PresentationVideoDecodeState::ready, "fill budget did not yield ready");
        require(fill.admittedFrames == 1, "fill exceeded its frame budget");
        require(fill.queuedFrames == expectedQueue, "fill budget queue count differs");
        require(!fill.hasPendingFrame, "fill budget decoded beyond its admission budget");
    }
    const auto end = pump.fill(1);
    require(end.state == PresentationVideoDecodeState::endOfStream, "bounded fills did not reach EOS");
}

void rejectsDecodeLimitAboveRenderer() {
    auto oversized = limits(1);
    oversized.decode.maximumPixels = palmier::render::maximumRenderFramePixels + 1;
    requirePumpError(
        [&] {
            PresentationVideoDecodePump pump(oversized);
            static_cast<void>(pump);
        },
        PresentationVideoDecodeErrorCode::decodeLimitExceedsRenderBudget
    );

    PresentationVideoDecodePump idle(limits(1));
    requirePumpError(
        [&] { static_cast<void>(idle.fill(1)); },
        PresentationVideoDecodeErrorCode::notStarted
    );

    auto zeroFill = limits(1);
    zeroFill.maximumFramesPerFill = 0;
    requirePumpError(
        [&] {
            PresentationVideoDecodePump pump(zeroFill);
            static_cast<void>(pump);
        },
        PresentationVideoDecodeErrorCode::invalidFillBudget
    );
    auto excessiveFill = limits(1);
    excessiveFill.maximumFramesPerFill = 33;
    requirePumpError(
        [&] {
            PresentationVideoDecodePump pump(excessiveFill);
            static_cast<void>(pump);
        },
        PresentationVideoDecodeErrorCode::invalidFillBudget
    );
}

}

int main() {
    try {
        TemporaryDirectory directory;
        const auto input = directory.write(
            "opaque-three-frames.mov",
            palmier::media::test_fixtures::qtrleOpaqueThreeFrames
        );
        const auto otherInput = directory.write(
            "opaque-three-frames-copy.mov",
            palmier::media::test_fixtures::qtrleOpaqueThreeFrames
        );
        realFramesReachSharedRenderers(input);
        capacityRetainsOnePendingFrame(input);
        cancellationTerminatesOnlyItsGeneration(input);
        decodeFailureTerminatesGeneration(directory);
        replacementClearsQueuedAndPendingFrames(input);
        sameGenerationRejectsChangedInput(input, otherInput);
        cancellationBeforeReplacementCommitPreservesGeneration(input);
        fillBudgetIsIndependentOfQueueCapacity(input);
        rejectsDecodeLimitAboveRenderer();
        std::cout << "PALMIER_FFMPEG_PRESENTATION_PIPELINE_TESTS_OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "PALMIER_FFMPEG_PRESENTATION_PIPELINE_TESTS_FAILED "
                  << error.what() << '\n';
        return 1;
    }
}
