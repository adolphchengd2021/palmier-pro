#pragma once

#include "palmier/exporting/h264_project_exporter.hpp"

#include <functional>
#include <stop_token>
#include <string_view>

namespace palmier::exporting::detail {

struct H264ExportTestHooks final {
    std::function<void(std::string_view)> checkpoint;
};

H264ProjectExportReceipt exportStaticProjectH264ForTesting(
    const project::ProjectDocument& document,
    const H264ProjectExportRequest& request,
    const H264ExportLimits& limits,
    std::stop_token cancellation,
    const H264ExportTestHooks& hooks
);

}
