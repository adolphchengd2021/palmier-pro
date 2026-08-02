#include "palmier/project/project_runtime.hpp"

#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>

namespace palmier::project {
namespace {

constexpr std::size_t maximumPendingOperations = 64;

void checkCancellation(std::stop_token cancellation) {
    if (cancellation.stop_requested()) {
        throw ProjectRuntimeError("cancelled", "project runtime operation was cancelled");
    }
}

}

ProjectRuntimeError::ProjectRuntimeError(std::string codeValue, std::string detail)
    : std::runtime_error(std::move(detail)), code(std::move(codeValue)) {}

struct ProjectRuntime::Implementation final {
    explicit Implementation(std::shared_ptr<ProjectRuntimeObserver> observerValue)
        : observer(std::move(observerValue)), worker([this] { run(); }) {}

    ~Implementation() { close(); }

    Implementation(const Implementation&) = delete;
    Implementation& operator=(const Implementation&) = delete;

    template<typename Result, typename Operation>
    Result invoke(Operation operation) {
        auto promise = std::make_shared<std::promise<Result>>();
        auto future = promise->get_future();
        std::shared_ptr<ProjectRuntimeObserver> admissionObserver;
        {
            std::scoped_lock lock(queueMutex);
            if (std::this_thread::get_id() == workerId) {
                throw ProjectRuntimeError(
                    "reentrantRuntimeCall",
                    "project runtime operations cannot call the runtime recursively"
                );
            }
            if (!accepting) {
                throw ProjectRuntimeError("runtimeClosed", "project runtime is closed");
            }
            if (operations.size() >= maximumPendingOperations) {
                throw ProjectRuntimeError("runtimeBusy", "project runtime operation queue is full");
            }
            operations.emplace_back([
                this,
                promise,
                operation = std::move(operation)
            ]() mutable {
                try {
                    if constexpr (std::is_void_v<Result>) {
                        operation(*this);
                        promise->set_value();
                    } else {
                        promise->set_value(operation(*this));
                    }
                } catch (...) {
                    promise->set_exception(std::current_exception());
                }
            });
            admissionObserver = observer;
        }
        condition.notify_one();
        if (admissionObserver) admissionObserver->operationAdmitted();
        return future.get();
    }

    ProjectSession& requireSession() {
        if (!session) {
            throw ProjectRuntimeError("noActiveProject", "project runtime has no active project");
        }
        return *session;
    }

    void requireProjectGeneration(
        const std::optional<std::uint64_t>& expectedProjectGeneration
    ) const {
        if (
            expectedProjectGeneration
            && *expectedProjectGeneration != projectGeneration
        ) {
            throw ProjectRuntimeError(
                "staleProjectGeneration",
                "project runtime operation targets a replaced project"
            );
        }
    }

    std::shared_ptr<const ProjectSessionSnapshot> publishedSnapshot(
        std::stop_token cancellation
    ) {
        checkCancellation(cancellation);
        return std::make_shared<const ProjectSessionSnapshot>(
            requireSession().snapshot(cancellation)
        );
    }

    void publishState(const ProjectRuntimeState& state) noexcept {
        if (observer) observer->statePublished(state);
    }

    void close() noexcept {
        bool calledFromWorker{};
        {
            std::scoped_lock lock(queueMutex);
            accepting = false;
            calledFromWorker = std::this_thread::get_id() == workerId;
        }
        condition.notify_all();
        if (calledFromWorker) {
            return;
        }
        std::scoped_lock closeLock(closeMutex);
        if (worker.joinable()) {
            worker.join();
        }
    }

    void run() {
        {
            std::scoped_lock lock(queueMutex);
            workerId = std::this_thread::get_id();
        }
        for (;;) {
            std::function<void()> operation;
            {
                std::unique_lock lock(queueMutex);
                condition.wait(lock, [this] { return !operations.empty() || !accepting; });
                if (operations.empty()) {
                    return;
                }
                operation = std::move(operations.front());
                operations.pop_front();
            }
            operation();
        }
    }

    std::mutex queueMutex;
    std::condition_variable condition;
    std::deque<std::function<void()>> operations;
    bool accepting{true};
    std::thread::id workerId;
    std::mutex closeMutex;
    std::shared_ptr<ProjectRuntimeObserver> observer;
    std::jthread worker;
    std::unique_ptr<ProjectSession> session;
    std::uint64_t projectGeneration{};
};

ProjectRuntime::ProjectRuntime(std::shared_ptr<ProjectRuntimeObserver> observer)
    : implementation_(std::make_unique<Implementation>(std::move(observer))) {}

ProjectRuntime::~ProjectRuntime() { close(); }

ProjectRuntimeState ProjectRuntime::install(
    ProjectDocument document,
    std::uint64_t projectGeneration,
    IdGenerator idGenerator,
    bool allowDiscardDirty,
    std::stop_token cancellation
) {
    return implementation_->invoke<ProjectRuntimeState>([
        document = std::move(document),
        projectGeneration,
        idGenerator = std::move(idGenerator),
        allowDiscardDirty,
        cancellation
    ](Implementation& runtime) mutable {
        checkCancellation(cancellation);
        if (projectGeneration == 0) {
            throw ProjectRuntimeError(
                "invalidProjectGeneration",
                "project generation must be positive"
            );
        }
        if (projectGeneration <= runtime.projectGeneration) {
            throw ProjectRuntimeError(
                "staleProjectGeneration",
                "project generation must advance monotonically"
            );
        }
        if (runtime.session && runtime.session->dirty() && !allowDiscardDirty) {
            throw ProjectRuntimeError(
                "dirtyProject",
                "save or explicitly discard the current project before replacement"
            );
        }
        auto candidate = std::make_unique<ProjectSession>(document, std::move(idGenerator));
        auto state = std::make_shared<const ProjectSessionSnapshot>(
            candidate->snapshot(cancellation)
        );
        checkCancellation(cancellation);
        runtime.session.swap(candidate);
        runtime.projectGeneration = projectGeneration;
        ProjectRuntimeState result{projectGeneration, std::move(state)};
        runtime.publishState(result);
        return result;
    });
}

ProjectRuntimeState ProjectRuntime::snapshot(
    std::optional<std::uint64_t> expectedProjectGeneration,
    std::stop_token cancellation
) {
    return implementation_->invoke<ProjectRuntimeState>([
        expectedProjectGeneration,
        cancellation
    ](Implementation& runtime) {
        runtime.requireProjectGeneration(expectedProjectGeneration);
        auto state = runtime.publishedSnapshot(cancellation);
        return ProjectRuntimeState{runtime.projectGeneration, std::move(state)};
    });
}

std::uint64_t ProjectRuntime::projectGeneration(std::stop_token cancellation) {
    return implementation_->invoke<std::uint64_t>([cancellation](Implementation& runtime) {
        checkCancellation(cancellation);
        return runtime.projectGeneration;
    });
}

ProjectRuntimeTimelineResult ProjectRuntime::getTimeline(
    const TimelineQuery& query,
    std::optional<std::uint64_t> expectedProjectGeneration,
    std::stop_token cancellation
) {
    return implementation_->invoke<ProjectRuntimeTimelineResult>([
        query,
        expectedProjectGeneration,
        cancellation
    ](Implementation& runtime) {
        checkCancellation(cancellation);
        runtime.requireProjectGeneration(expectedProjectGeneration);
        auto& session = runtime.requireSession();
        auto timeline = session.getTimeline(query, cancellation);
        return ProjectRuntimeTimelineResult{
            runtime.projectGeneration,
            session.revision(),
            std::move(timeline),
        };
    });
}

ProjectRuntimeSaveSnapshotResult ProjectRuntime::saveSnapshot(
    std::optional<std::uint64_t> expectedProjectGeneration,
    std::stop_token cancellation
) {
    return implementation_->invoke<ProjectRuntimeSaveSnapshotResult>([
        expectedProjectGeneration,
        cancellation
    ](Implementation& runtime) {
        checkCancellation(cancellation);
        runtime.requireProjectGeneration(expectedProjectGeneration);
        return ProjectRuntimeSaveSnapshotResult{
            runtime.projectGeneration,
            runtime.requireSession().saveSnapshot(cancellation),
        };
    });
}

ProjectRuntimeCommandResult ProjectRuntime::splitClips(
    SplitClipsCommand command,
    std::optional<std::uint64_t> expectedProjectGeneration,
    std::stop_token cancellation
) {
    return implementation_->invoke<ProjectRuntimeCommandResult>([
        command = std::move(command),
        expectedProjectGeneration,
        cancellation
    ](Implementation& runtime) mutable {
        checkCancellation(cancellation);
        runtime.requireProjectGeneration(expectedProjectGeneration);
        auto result = runtime.requireSession().splitClips(command, cancellation);
        if (runtime.observer) runtime.observer->operationCommitted();
        auto state = result.publication;
        ProjectRuntimeCommandResult published{
            runtime.projectGeneration,
            std::move(result),
            std::move(state),
        };
        runtime.publishState({published.projectGeneration, published.session});
        return published;
    });
}

ProjectRuntimeCommandResult ProjectRuntime::undo(
    std::optional<std::uint64_t> expectedProjectGeneration,
    std::stop_token cancellation
) {
    return implementation_->invoke<ProjectRuntimeCommandResult>([
        expectedProjectGeneration,
        cancellation
    ](Implementation& runtime) {
        checkCancellation(cancellation);
        runtime.requireProjectGeneration(expectedProjectGeneration);
        auto result = runtime.requireSession().undo(cancellation);
        if (runtime.observer) runtime.observer->operationCommitted();
        auto state = result.publication;
        ProjectRuntimeCommandResult published{
            runtime.projectGeneration,
            std::move(result),
            std::move(state),
        };
        runtime.publishState({published.projectGeneration, published.session});
        return published;
    });
}

ProjectRuntimeState ProjectRuntime::markPersisted(
    std::uint64_t stateId,
    std::optional<std::uint64_t> expectedProjectGeneration,
    std::stop_token cancellation
) {
    return implementation_->invoke<ProjectRuntimeState>([
        stateId,
        expectedProjectGeneration,
        cancellation
    ](Implementation& runtime) {
        checkCancellation(cancellation);
        runtime.requireProjectGeneration(expectedProjectGeneration);
        auto& session = runtime.requireSession();
        const auto previousPersistedStateId = session.persistedStateId();
        auto state = session.markPersisted(stateId);
        const bool changed = session.persistedStateId() != previousPersistedStateId;
        if (changed && runtime.observer) runtime.observer->operationCommitted();
        ProjectRuntimeState published{runtime.projectGeneration, std::move(state)};
        if (changed) runtime.publishState(published);
        return published;
    });
}

void ProjectRuntime::close() noexcept {
    if (implementation_) {
        implementation_->close();
    }
}

}
