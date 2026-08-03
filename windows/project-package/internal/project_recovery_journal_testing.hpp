#pragma once

#include "palmier/project/project_recovery_journal.hpp"

namespace palmier::project::testing {

enum class ProjectRecoveryJournalCheckpoint {
    afterSnapshot,
    afterBaselineRead,
    afterSerialization,
    afterStagingCreation,
    afterWrite,
    afterFlush,
    beforeCommit,
    afterCommit,
};

class ProjectRecoveryJournalCheckpoints {
public:
    virtual ~ProjectRecoveryJournalCheckpoints() = default;
    virtual void arrive(ProjectRecoveryJournalCheckpoint checkpoint) noexcept = 0;
};

ProjectRecoveryJournalWriteReceipt writeProjectRecoveryJournal(
    const ProjectRecoveryJournal& journal,
    ProjectRuntime& runtime,
    const std::filesystem::path& packagePath,
    std::optional<std::uint64_t> expectedProjectGeneration,
    std::stop_token cancellation,
    ProjectRecoveryJournalCheckpoints* checkpoints
);

}
