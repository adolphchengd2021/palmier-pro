#pragma once

#include "palmier/project/project_session.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>

namespace palmier::project {

struct ProjectRuntimeState final {
    std::uint64_t projectGeneration;
    std::shared_ptr<const ProjectSessionSnapshot> session;
};

struct ProjectRuntimeTimelineResult final {
    std::uint64_t projectGeneration;
    std::uint64_t revision;
    palmier::json::Value timeline;
};

struct ProjectRuntimeSaveSnapshotResult final {
    std::uint64_t projectGeneration;
    ProjectSaveSnapshot snapshot;
};

struct ProjectRuntimeCommandResult final {
    std::uint64_t projectGeneration;
    CommandResult command;
    std::shared_ptr<const ProjectSessionSnapshot> session;
};

class ProjectRuntimeError final : public std::runtime_error {
public:
    ProjectRuntimeError(std::string code, std::string detail);

    const std::string code;
};

class ProjectRuntimeObserver {
public:
    virtual ~ProjectRuntimeObserver() = default;
    virtual void operationAdmitted() noexcept = 0;
    virtual void operationCommitted() noexcept {}
    virtual void statePublished(const ProjectRuntimeState&) noexcept {}
};

class ProjectRuntime final {
public:
    explicit ProjectRuntime(std::shared_ptr<ProjectRuntimeObserver> observer = {});
    ~ProjectRuntime();

    ProjectRuntime(const ProjectRuntime&) = delete;
    ProjectRuntime& operator=(const ProjectRuntime&) = delete;

    ProjectRuntimeState install(
        ProjectDocument document,
        std::uint64_t projectGeneration,
        IdGenerator idGenerator,
        bool allowDiscardDirty = false,
        std::stop_token cancellation = {}
    );
    ProjectRuntimeState snapshot(
        std::optional<std::uint64_t> expectedProjectGeneration = {},
        std::stop_token cancellation = {}
    );
    std::uint64_t projectGeneration(std::stop_token cancellation = {});
    ProjectRuntimeTimelineResult getTimeline(
        const TimelineQuery& query = {},
        std::optional<std::uint64_t> expectedProjectGeneration = {},
        std::stop_token cancellation = {}
    );
    ProjectRuntimeSaveSnapshotResult saveSnapshot(
        std::optional<std::uint64_t> expectedProjectGeneration = {},
        std::stop_token cancellation = {}
    );
    ProjectRuntimeCommandResult splitClips(
        SplitClipsCommand command,
        std::optional<std::uint64_t> expectedProjectGeneration = {},
        std::stop_token cancellation = {}
    );
    ProjectRuntimeCommandResult undo(
        std::optional<std::uint64_t> expectedProjectGeneration = {},
        std::stop_token cancellation = {}
    );
    ProjectRuntimeState markPersisted(
        std::uint64_t stateId,
        std::optional<std::uint64_t> expectedProjectGeneration = {},
        std::stop_token cancellation = {}
    );
    void close() noexcept;

private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
};

}
