#pragma once

#include "palmier/exporting/h264_project_exporter.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string_view>
#include <vector>

namespace palmier::exporting::detail {

struct FileFlushResult final {
    bool flushed;
    int nativeCode;
};

struct H264ExportTestHooks final {
    std::function<void(std::string_view)> checkpoint;
    std::function<FileFlushResult(
        std::uintptr_t,
        const std::filesystem::path&
    )> flushFileBuffers;
};

void installStagingFileForTesting(
    const std::filesystem::path& destination,
    const std::vector<std::uint8_t>& payload,
    bool replaceExisting,
    std::stop_token cancellation,
    const H264ExportTestHooks& hooks
);

H264ProjectExportReceipt exportStaticProjectH264ForTesting(
    const project::ProjectDocument& document,
    const H264ProjectExportRequest& request,
    const H264ExportLimits& limits,
    std::stop_token cancellation,
    const H264ExportTestHooks& hooks
);

H264ProjectExportReceipt exportStaticProjectTimelineH264ForTesting(
    const project::ProjectDocument& document,
    const H264ProjectTimelineExportRequest& request,
    const H264ExportLimits& limits,
    std::stop_token cancellation,
    const H264ExportTestHooks& hooks
);

}
