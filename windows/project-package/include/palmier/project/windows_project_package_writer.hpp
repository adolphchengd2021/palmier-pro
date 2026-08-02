#pragma once

#include "palmier/project/project_runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <stdexcept>
#include <string>

namespace palmier::project {

enum class ProjectPackageWriteWarning {
    none,
    runtimeReplacedAfterSave,
    runtimeClosedAfterSave,
    runtimeAcknowledgementFailed,
};

struct ProjectPackageWriteReceipt final {
    std::uint64_t projectGeneration;
    std::uint64_t revision;
    std::uint64_t stateId;
    std::size_t projectJsonBytes;
    bool runtimeAcknowledged;
    bool runtimeDirty;
    ProjectPackageWriteWarning warning;
};

class ProjectPackageWriteError final : public std::runtime_error {
public:
    ProjectPackageWriteError(
        std::string code,
        std::string stage,
        std::string detail,
        int nativeCode = 0
    );

    const std::string code;
    const std::string stage;
    const int nativeCode;
};

// Caller must run this synchronous file operation off the UI thread.
ProjectPackageWriteReceipt writeProjectPackage(
    ProjectRuntime& runtime,
    const std::filesystem::path& packagePath,
    std::optional<std::uint64_t> expectedProjectGeneration = {},
    std::stop_token cancellation = {}
);

}
