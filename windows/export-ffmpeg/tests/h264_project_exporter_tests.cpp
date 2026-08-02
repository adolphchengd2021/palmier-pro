#include "media_test_fixtures.hpp"
#include "media_test_support.hpp"
#include "internal/h264_project_exporter_testing.hpp"
#include "palmier/exporting/h264_project_exporter.hpp"
#include "palmier/media/ffmpeg_media_reader.hpp"
#include "palmier/project/project_reader.hpp"

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace {

using palmier::exporting::H264ExportError;
using palmier::exporting::H264ExportFailureCode;
using palmier::exporting::H264ExportLimits;
using palmier::exporting::H264ProjectExportRequest;
using palmier::media::FfmpegMediaReader;
using palmier::media::FfmpegVideoFrameReader;
using palmier::media::StreamKind;
using palmier::media::test_support::TemporaryDirectory;
using palmier::media::test_support::require;

palmier::project::ProjectDocument document(
    std::string_view fps = "10",
    std::string_view duration = "5"
) {
    const std::string source = std::string(R"({
        "timelines":[{
            "id":"timeline","fps":)")
        + std::string(fps)
        + R"(,"width":64,"height":64,
            "tracks":[{
                "id":"track","type":"video","clips":[{
                    "id":"clip","mediaRef":"media","mediaType":"video",
                    "sourceClipType":"video","startFrame":0,
                    "durationFrames":)"
        + std::string(duration)
        + R"(,"trimStartFrame":0,"trimEndFrame":0,"speed":1,
                    "opacity":0.75,"blendMode":"normal",
                    "transform":{
                        "centerX":0.5,"centerY":0.5,"width":1,"height":1,
                        "rotation":0,"flipHorizontal":false,"flipVertical":false
                    },
                    "crop":{"left":0,"top":0,"right":0,"bottom":0},
                    "edgeRounding":0,"edgeSoftness":0,
                    "effects":[{
                        "id":"effect","type":"color.exposure","enabled":true,
                        "params":{"ev":{"value":0.5}}
                    }]
                }]
            }]
        }],
        "activeTimelineId":"timeline"
    })";
    return palmier::project::readProject(source, [] {
        return std::string("unexpected-generated-id");
    });
}

palmier::project::ProjectDocument overlappingDocument() {
    constexpr auto source = R"({
        "timelines":[{
            "id":"timeline","fps":10,"width":64,"height":64,
            "tracks":[
                {"id":"track","type":"video","clips":[{
                    "id":"clip","mediaRef":"media","mediaType":"video",
                    "sourceClipType":"video","startFrame":0,"durationFrames":5,
                    "trimStartFrame":0,"trimEndFrame":0,"speed":1,
                    "opacity":1,"blendMode":"normal"
                }]},
                {"id":"title-track","type":"text","clips":[{
                    "id":"title","mediaRef":"title","mediaType":"text",
                    "sourceClipType":"text","startFrame":1,"durationFrames":2
                }]}
            ]
        }],
        "activeTimelineId":"timeline"
    })";
    return palmier::project::readProject(source, [] {
        return std::string("unexpected-generated-id");
    });
}

H264ProjectExportRequest request(
    const std::filesystem::path& input,
    const std::filesystem::path& destination,
    bool replaceExisting = false
) {
    return {
        "timeline",
        "track",
        "clip",
        input,
        destination,
        500'000,
        replaceExisting,
    };
}

template<typename Operation>
void requireExportError(Operation operation, H264ExportFailureCode code) {
    try {
        operation();
    } catch (const H264ExportError& error) {
        require(error.code == code, "unexpected export error code");
        require(!error.stage.empty(), "export error stage is empty");
        return;
    }
    throw std::runtime_error("expected export failure");
}

std::vector<std::uint8_t> readBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(input.is_open(), "test output could not be opened");
    input.seekg(0, std::ios::end);
    const auto size = input.tellg();
    require(size >= 0, "test output size could not be read");
    input.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    input.read(
        reinterpret_cast<char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size())
    );
    require(input.good() || input.eof(), "test output bytes could not be read");
    return bytes;
}

void requireNoStagingFiles(const std::filesystem::path& directory) {
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        require(
            entry.path().filename().wstring().find(L".partial.mp4")
                == std::wstring::npos,
            "export left a staging file"
        );
    }
}

void exportsAndIndependentlyDecodesEveryFrame(
    const std::filesystem::path& input,
    const std::filesystem::path& destination
) {
    const auto receipt = palmier::exporting::exportStaticProjectH264(
        document(),
        request(input, destination)
    );
    require(receipt.destination == destination, "receipt destination changed");
    require(receipt.encoderName == "h264_mf", "unlocked H.264 encoder was selected");
    require(receipt.encodedFrames == 5, "encoded frame count changed");
    require(receipt.verifiedFrames == 5, "verified frame count changed");
    require(receipt.width == 64 && receipt.height == 64, "receipt size changed");
    require(receipt.framesPerSecond == 10, "receipt frame rate changed");

    const auto probe = FfmpegMediaReader::probe(destination);
    const auto video = std::find_if(
        probe.streams.begin(),
        probe.streams.end(),
        [](const auto& value) { return value.kind == StreamKind::video; }
    );
    require(video != probe.streams.end(), "exported MP4 has no video stream");
    require(video->codecName == "h264", "exported MP4 codec changed");
    require(video->width == 64 && video->height == 64, "exported dimensions changed");
    require(
        video->realFrameRate.numerator == 10
            && video->realFrameRate.denominator == 1,
        "exported frame cadence changed"
    );

    FfmpegVideoFrameReader reader(destination);
    std::uint64_t frames = 0;
    std::uint8_t minimumRed = 255;
    std::uint8_t maximumRed = 0;
    while (const auto frame = reader.nextFrame()) {
        ++frames;
        require(
            palmier::media::isPrototypeBt709RgbColor(frame->color),
            "decoded BT.709 RGB metadata changed"
        );
        for (std::size_t index = 0; index < frame->rgba8.size(); index += 4) {
            minimumRed = (std::min)(minimumRed, frame->rgba8[index]);
            maximumRed = (std::max)(maximumRed, frame->rgba8[index]);
        }
    }
    require(frames == 5, "independent output decode frame count changed");
    require(maximumRed > minimumRed, "exported video lost visible pixel variation");
}

void refusesExistingDestinationWithoutMutation(
    const TemporaryDirectory& directory,
    const std::filesystem::path& input
) {
    const std::vector<std::uint8_t> sentinel{1, 2, 3, 4};
    const auto destination = directory.write("existing.mp4", sentinel);
    requireExportError(
        [&] {
            static_cast<void>(palmier::exporting::exportStaticProjectH264(
                document(),
                request(input, destination)
            ));
        },
        H264ExportFailureCode::destinationExists
    );
    require(readBytes(destination) == sentinel, "existing destination was mutated");
    requireNoStagingFiles(directory.path());
}

void replacementInstallsOnlyVerifiedOutput(
    const TemporaryDirectory& directory,
    const std::filesystem::path& input
) {
    const std::vector<std::uint8_t> sentinel{9, 8, 7, 6};
    const auto destination = directory.write("replace.mp4", sentinel);
    const auto receipt = palmier::exporting::exportStaticProjectH264(
        document(),
        request(input, destination, true)
    );
    require(receipt.verifiedFrames == 5, "replacement was not verified");
    require(readBytes(destination) != sentinel, "replacement retained old bytes");
    requireNoStagingFiles(directory.path());
}

void failedInstallPreservesExistingDestination(
    const TemporaryDirectory& directory,
    const std::filesystem::path& input
) {
    const std::vector<std::uint8_t> sentinel{5, 4, 3, 2, 1};
    const auto destination = directory.write("locked.mp4", sentinel);
    HANDLE lock = CreateFileW(
        destination.c_str(),
        GENERIC_READ,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    require(lock != INVALID_HANDLE_VALUE, "test destination could not be locked");
    requireExportError(
        [&] {
            static_cast<void>(palmier::exporting::exportStaticProjectH264(
                document(),
                request(input, destination, true)
            ));
        },
        H264ExportFailureCode::installFailed
    );
    CloseHandle(lock);
    require(readBytes(destination) == sentinel, "failed install mutated destination");
    requireNoStagingFiles(directory.path());
}

void cancellationAndLimitsDoNotCreateOutput(
    const TemporaryDirectory& directory,
    const std::filesystem::path& input
) {
    const auto cancelledDestination = directory.path() / "cancelled.mp4";
    std::stop_source cancellation;
    cancellation.request_stop();
    requireExportError(
        [&] {
            static_cast<void>(palmier::exporting::exportStaticProjectH264(
                document(),
                request(input, cancelledDestination),
                {},
                cancellation.get_token()
            ));
        },
        H264ExportFailureCode::cancelled
    );
    require(!std::filesystem::exists(cancelledDestination), "cancel installed output");

    const auto limitedDestination = directory.path() / "limited.mp4";
    H264ExportLimits limits;
    limits.maximumFrames = 4;
    requireExportError(
        [&] {
            static_cast<void>(palmier::exporting::exportStaticProjectH264(
                document(),
                request(input, limitedDestination),
                limits
            ));
        },
        H264ExportFailureCode::resourceLimitExceeded
    );
    require(!std::filesystem::exists(limitedDestination), "limit failure installed output");
    requireNoStagingFiles(directory.path());
}

void cancellationAfterStagingPreservesExistingDestination(
    const TemporaryDirectory& directory,
    const std::filesystem::path& input
) {
    const std::vector<std::uint8_t> sentinel{4, 3, 2, 1};
    const auto destination = directory.write("cancel-staging.mp4", sentinel);
    std::stop_source cancellation;
    palmier::exporting::detail::H264ExportTestHooks hooks;
    hooks.checkpoint = [&](std::string_view name) {
        if (name == "afterStaging") {
            cancellation.request_stop();
        }
    };
    requireExportError(
        [&] {
            static_cast<void>(
                palmier::exporting::detail::exportStaticProjectH264ForTesting(
                    document(),
                    request(input, destination, true),
                    {},
                    cancellation.get_token(),
                    hooks
                )
            );
        },
        H264ExportFailureCode::cancelled
    );
    require(
        readBytes(destination) == sentinel,
        "post-staging cancellation mutated destination"
    );
    requireNoStagingFiles(directory.path());
}

void invalidDestinationIsRefusedBeforeStaging(
    const TemporaryDirectory& directory,
    const std::filesystem::path& input
) {
    const auto destination = directory.path() / "missing" / "output.mp4";
    requireExportError(
        [&] {
            static_cast<void>(palmier::exporting::exportStaticProjectH264(
                document(),
                request(input, destination)
            ));
        },
        H264ExportFailureCode::invalidRequest
    );
    require(!std::filesystem::exists(destination), "invalid path installed output");
    requireNoStagingFiles(directory.path());
}

void overlappingVisibleLayerIsRefusedBeforeStaging(
    const TemporaryDirectory& directory,
    const std::filesystem::path& input
) {
    const auto destination = directory.path() / "overlap.mp4";
    requireExportError(
        [&] {
            static_cast<void>(palmier::exporting::exportStaticProjectH264(
                overlappingDocument(),
                request(input, destination)
            ));
        },
        H264ExportFailureCode::unsupportedProject
    );
    require(!std::filesystem::exists(destination), "overlap installed output");
    requireNoStagingFiles(directory.path());
}

void timingMismatchIsRefused(
    const TemporaryDirectory& directory,
    const std::filesystem::path& input
) {
    const auto rateDestination = directory.path() / "rate.mp4";
    requireExportError(
        [&] {
            static_cast<void>(palmier::exporting::exportStaticProjectH264(
                document("30", "5"),
                request(input, rateDestination)
            ));
        },
        H264ExportFailureCode::unsupportedSourceTiming
    );
    require(!std::filesystem::exists(rateDestination), "rate failure installed output");
    requireNoStagingFiles(directory.path());
}

void earlyEofCleansStaging(
    const TemporaryDirectory& directory,
    const std::filesystem::path& input
) {
    const auto earlyDestination = directory.path() / "early.mp4";
    requireExportError(
        [&] {
            static_cast<void>(palmier::exporting::exportStaticProjectH264(
                document("10", "6"),
                request(input, earlyDestination)
            ));
        },
        H264ExportFailureCode::sourceEndedEarly
    );
    require(!std::filesystem::exists(earlyDestination), "early EOF installed output");
    requireNoStagingFiles(directory.path());
}

void cancellationBeforeInstallPreservesExistingDestination(
    const TemporaryDirectory& directory,
    const std::filesystem::path& input
) {
    const std::vector<std::uint8_t> sentinel{8, 6, 4, 2};
    const auto destination = directory.write("cancel-install.mp4", sentinel);
    std::stop_source cancellation;
    palmier::exporting::detail::H264ExportTestHooks hooks;
    hooks.checkpoint = [&](std::string_view name) {
        if (name == "beforeInstall") {
            cancellation.request_stop();
        }
    };
    requireExportError(
        [&] {
            static_cast<void>(
                palmier::exporting::detail::exportStaticProjectH264ForTesting(
                    document(),
                    request(input, destination, true),
                    {},
                    cancellation.get_token(),
                    hooks
                )
            );
        },
        H264ExportFailureCode::cancelled
    );
    require(
        readBytes(destination) == sentinel,
        "pre-install cancellation mutated destination"
    );
    requireNoStagingFiles(directory.path());
}

}

int main(int argumentCount, char* arguments[]) {
    try {
        require(argumentCount == 2, "expected one test mode");
        TemporaryDirectory directory;
        const auto input = directory.write(
            "source.mp4",
            palmier::media::test_fixtures::h264Aac
        );
        const std::string_view mode(arguments[1]);
        if (mode == "--contract") {
            refusesExistingDestinationWithoutMutation(directory, input);
            cancellationAndLimitsDoNotCreateOutput(directory, input);
            cancellationAfterStagingPreservesExistingDestination(directory, input);
            invalidDestinationIsRefusedBeforeStaging(directory, input);
            overlappingVisibleLayerIsRefusedBeforeStaging(directory, input);
            timingMismatchIsRefused(directory, input);
            std::cout << "h264 project exporter contract tests passed\n";
            return 0;
        }
        require(mode == "--native", "unknown test mode");
        try {
            const auto destination = directory.path() / "export.mp4";
            exportsAndIndependentlyDecodesEveryFrame(input, destination);
            requireNoStagingFiles(directory.path());
            replacementInstallsOnlyVerifiedOutput(directory, input);
            failedInstallPreservesExistingDestination(directory, input);
            earlyEofCleansStaging(directory, input);
            cancellationBeforeInstallPreservesExistingDestination(directory, input);
        } catch (const H264ExportError& error) {
            if (error.code == H264ExportFailureCode::unsupportedEncoder) {
                std::cout << "h264_mf unavailable: " << error.what() << '\n';
                return 77;
            }
            throw;
        }
        std::cout << "h264 project exporter native tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
