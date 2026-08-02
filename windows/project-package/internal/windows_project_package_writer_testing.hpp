#pragma once

#include "palmier/project/windows_project_package_writer.hpp"

namespace palmier::project::testing {

enum class ProjectPackageWriteCheckpoint {
    afterSnapshot,
    afterSerialization,
    afterStagingCreation,
    afterWrite,
    afterFlush,
    beforeCommit,
    afterCommit,
};

class ProjectPackageWriteCheckpoints {
public:
    virtual ~ProjectPackageWriteCheckpoints() = default;
    virtual void arrive(ProjectPackageWriteCheckpoint checkpoint) noexcept = 0;
    virtual bool failRuntimeAcknowledgement() const noexcept { return false; }
};

ProjectPackageWriteReceipt writeProjectPackage(
    ProjectRuntime& runtime,
    const std::filesystem::path& packagePath,
    std::optional<std::uint64_t> expectedProjectGeneration,
    std::stop_token cancellation,
    ProjectPackageWriteCheckpoints* checkpoints
);

ProjectPackageSaveAsReceipt writeProjectPackageAs(
    ProjectRuntime& runtime,
    const std::filesystem::path& sourcePackagePath,
    const std::filesystem::path& destinationPackagePath,
    std::optional<std::uint64_t> expectedProjectGeneration,
    std::stop_token cancellation,
    ProjectPackageWriteCheckpoints* checkpoints
);

}
