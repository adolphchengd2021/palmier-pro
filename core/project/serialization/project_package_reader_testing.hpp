#pragma once

#include "palmier/project/project_package_reader.hpp"

namespace palmier::project::testing {

inline constexpr std::size_t projectJsonReadChunkBytes = 64U * 1024U;

enum class ProjectPackageReadCheckpoint {
    afterOpen,
    afterChunk,
    beforeParse,
    duringParse,
    afterParse,
};

class ProjectPackageReadCheckpoints {
public:
    virtual ~ProjectPackageReadCheckpoints() = default;
    virtual void arrive(ProjectPackageReadCheckpoint checkpoint) = 0;
};

ProjectDocument readProjectPackage(
    const std::filesystem::path& packagePath,
    const IdGenerator& idGenerator,
    ProjectPackageReadOptions options,
    ProjectPackageReadCheckpoints* checkpoints
);

}
