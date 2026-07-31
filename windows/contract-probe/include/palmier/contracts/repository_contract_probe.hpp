#pragma once

#include <filesystem>
#include <string>

namespace palmier::contracts {

enum class ProbeExitCode : int {
    success = 0,
    usage = 2,
    io = 3,
    json = 4,
    version = 5,
    invariant = 6,
};

struct ProbeResult {
    ProbeExitCode code;
    std::string marker;
    std::string detail;
};

ProbeResult probeRepository(const std::filesystem::path& root);
ProbeResult probeContractVersionFile(const std::filesystem::path& path);

}
