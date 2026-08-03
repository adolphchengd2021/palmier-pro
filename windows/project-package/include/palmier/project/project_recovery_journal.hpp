#pragma once

#include "palmier/project/project_runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <string_view>

namespace palmier::project {

enum class ProjectRecoveryJournalStatus {
    missing,
    recoverable,
    staleBaseline,
    redundant,
};

struct ProjectRecoveryJournalCandidate final {
    std::filesystem::path journalPath;
    std::string packagePath;
    std::uint64_t projectGeneration;
    std::uint64_t revision;
    std::uint64_t stateId;
    std::uint64_t persistedStateId;
    std::uint64_t createdUnixMilliseconds;
    std::string baselineProjectJsonSha256;
    std::string projectJsonSha256;
    std::string projectJson;
};

struct ProjectRecoveryJournalInspection final {
    ProjectRecoveryJournalStatus status;
    std::optional<ProjectRecoveryJournalCandidate> candidate;
};

struct ProjectRecoveryJournalWriteReceipt final {
    std::filesystem::path journalPath;
    std::uint64_t projectGeneration;
    std::uint64_t revision;
    std::uint64_t stateId;
    std::size_t projectJsonBytes;
    std::size_t journalBytes;
};

struct ProjectRecoveryJournalFingerprint final {
    std::filesystem::path journalPath;
    std::string journalSha256;
    std::size_t journalBytes;
};

class ProjectRecoveryJournalError final : public std::runtime_error {
public:
    ProjectRecoveryJournalError(
        std::string code,
        std::string stage,
        std::string detail,
        int nativeCode = 0
    );

    const std::string code;
    const std::string stage;
    const int nativeCode;
};

// Operations perform synchronous filesystem work and must run off the UI thread.
class ProjectRecoveryJournal final {
public:
    ProjectRecoveryJournal();
    explicit ProjectRecoveryJournal(std::filesystem::path recoveryRoot);

    const std::filesystem::path& configuredRecoveryRoot() const noexcept;

    ProjectRecoveryJournalWriteReceipt write(
        ProjectRuntime& runtime,
        const std::filesystem::path& packagePath,
        std::optional<std::uint64_t> expectedProjectGeneration = {},
        std::stop_token cancellation = {}
    ) const;
    ProjectRecoveryJournalInspection inspect(
        const std::filesystem::path& packagePath,
        std::stop_token cancellation = {}
    ) const;
    std::optional<ProjectRecoveryJournalFingerprint> fingerprint(
        const std::filesystem::path& packagePath,
        std::stop_token cancellation = {}
    ) const;
    bool discard(
        const std::filesystem::path& packagePath,
        std::string_view expectedJournalSha256,
        std::stop_token cancellation = {}
    ) const;
    bool retire(
        const std::filesystem::path& packagePath,
        std::uint64_t expectedProjectGeneration,
        std::uint64_t committedRevision,
        std::stop_token cancellation = {}
    ) const;

private:
    std::filesystem::path recoveryRoot_;
};

}
