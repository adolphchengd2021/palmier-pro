#pragma once

#include "palmier/exporting/h264_project_exporter.hpp"

#include <cstdint>
#include <filesystem>
#include <stop_token>
#include <string>

namespace palmier::exporting {

struct ProjectClipH264ExportRequest final {
    std::filesystem::path packagePath;
    std::string trackId;
    std::string clipId;
    std::filesystem::path destination;
    std::int64_t bitRate{8'000'000};
    bool replaceExisting{};
};

// Caller must run this synchronous media and filesystem workflow off the UI thread.
H264ProjectExportReceipt exportProjectClipH264(
    const project::ProjectDocument& document,
    const ProjectClipH264ExportRequest& request,
    const H264ExportLimits& limits = {},
    std::stop_token cancellation = {}
);

}
