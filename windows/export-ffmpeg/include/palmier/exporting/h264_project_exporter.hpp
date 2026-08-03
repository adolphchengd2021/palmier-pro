#pragma once

#include "palmier/project/project.hpp"

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace palmier::exporting {

enum class H264ExportFailureCode {
    invalidRequest,
    unsupportedProject,
    resourceLimitExceeded,
    unsupportedSourceTiming,
    unsupportedEncoder,
    mediaUnavailable,
    sourceEndedEarly,
    encodeFailed,
    verificationFailed,
    destinationExists,
    stagingFailed,
    cleanupFailed,
    installFailed,
    cancelled,
};

class H264ExportError final : public std::runtime_error {
public:
    H264ExportError(
        H264ExportFailureCode code,
        std::string stage,
        std::string detail,
        int nativeCode = 0
    );

    const H264ExportFailureCode code;
    const std::string stage;
    const int nativeCode;
};

struct H264ExportLimits final {
    std::uint64_t maximumFrames{216'000};
    std::int64_t minimumBitRate{100'000};
    std::int64_t maximumBitRate{100'000'000};
};

struct H264ProjectExportRequest final {
    std::string timelineId;
    std::string trackId;
    std::string clipId;
    std::filesystem::path input;
    std::filesystem::path destination;
    std::int64_t bitRate{8'000'000};
    bool replaceExisting{};
};

struct H264ProjectExportSource final {
    std::string clipId;
    std::filesystem::path input;
};

struct H264ProjectTimelineExportRequest final {
    std::string timelineId;
    std::vector<H264ProjectExportSource> sources;
    std::filesystem::path destination;
    std::int64_t bitRate{8'000'000};
    bool replaceExisting{};
};

struct H264ProjectExportReceipt final {
    std::filesystem::path destination;
    std::string encoderName;
    std::uint64_t encodedFrames{};
    std::uint64_t verifiedFrames{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::int32_t framesPerSecond{};
};

H264ProjectExportReceipt exportStaticProjectH264(
    const project::ProjectDocument& document,
    const H264ProjectExportRequest& request,
    const H264ExportLimits& limits = {},
    std::stop_token cancellation = {}
);

H264ProjectExportReceipt exportStaticProjectTimelineH264(
    const project::ProjectDocument& document,
    const H264ProjectTimelineExportRequest& request,
    const H264ExportLimits& limits = {},
    std::stop_token cancellation = {}
);

}
