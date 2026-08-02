#include "palmier/windows/project_runtime_mailbox.hpp"

#include <utility>

namespace palmier::windows {

ProjectRuntimeMailbox::ProjectRuntimeMailbox(
    std::function<void()> publicationCheckpoint
) : publicationCheckpoint_(std::move(publicationCheckpoint)) {}

void ProjectRuntimeMailbox::statePublished(
    const project::ProjectRuntimeState& state
) noexcept {
    sequence_.fetch_add(1, std::memory_order_acq_rel);
    projectGeneration_.store(state.projectGeneration, std::memory_order_relaxed);
    session_.store(state.session, std::memory_order_release);
    sequence_.fetch_add(1, std::memory_order_release);
    if (publicationCheckpoint_) {
        try {
            publicationCheckpoint_();
        } catch (...) {
        }
    }
}

std::optional<ProjectRuntimePublication> ProjectRuntimeMailbox::latest() const noexcept {
    for (;;) {
        const auto before = sequence_.load(std::memory_order_acquire);
        if (before == 0) return std::nullopt;
        if ((before & 1U) != 0) continue;
        const auto generation = projectGeneration_.load(std::memory_order_relaxed);
        auto session = session_.load(std::memory_order_acquire);
        const auto after = sequence_.load(std::memory_order_acquire);
        if (before == after && (after & 1U) == 0) {
            return ProjectRuntimePublication{
                after / 2,
                generation,
                std::move(session),
            };
        }
    }
}

}
