#pragma once

#include "palmier/project/project_runtime.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace palmier::windows {

struct ProjectRuntimePublication final {
    std::uint64_t token{};
    std::uint64_t projectGeneration{};
    std::shared_ptr<const project::ProjectSessionSnapshot> session;
};

class ProjectRuntimeMailbox final : public project::ProjectRuntimeObserver {
public:
    explicit ProjectRuntimeMailbox(std::function<void()> publicationCheckpoint = {});

    void operationAdmitted() noexcept override {}
    void statePublished(const project::ProjectRuntimeState& state) noexcept override;

    std::optional<ProjectRuntimePublication> latest() const noexcept;

private:
    std::atomic<std::uint64_t> sequence_{};
    std::atomic<std::uint64_t> projectGeneration_{};
    std::atomic<std::shared_ptr<const project::ProjectSessionSnapshot>> session_{};
    std::function<void()> publicationCheckpoint_;
};

}
