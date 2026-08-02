#include "palmier/project/project_runtime.hpp"
#include "palmier/project/project_reader.hpp"

#include <condition_variable>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using palmier::project::ClipMove;
using palmier::project::MoveClipsCommand;
using palmier::project::ProjectRuntime;
using palmier::project::ProjectRuntimeError;
using palmier::project::ProjectRuntimeObserver;
using palmier::project::RemoveClipsCommand;
using palmier::project::SplitClipsCommand;
using palmier::project::SplitPoint;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

class RuntimeObserver final : public ProjectRuntimeObserver {
public:
    struct Publication final {
        std::uint64_t projectGeneration;
        std::uint64_t revision;
        std::uint64_t stateId;
        std::uint64_t persistedStateId;
    };

    void operationAdmitted() noexcept override {
        std::scoped_lock lock(mutex_);
        ++admissionCount_;
        condition_.notify_all();
    }

    void operationCommitted() noexcept override {
        std::scoped_lock lock(mutex_);
        if (cancelAfterCommit_) cancelAfterCommit_->request_stop();
        condition_.notify_all();
    }

    void statePublished(const palmier::project::ProjectRuntimeState& state) noexcept override {
        std::scoped_lock lock(mutex_);
        publications_.push_back({
            state.projectGeneration,
            state.session->revision,
            state.session->stateId,
            state.session->persistedStateId,
        });
        condition_.notify_all();
    }

    void waitForAdmissions(std::size_t count) {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [&] { return admissionCount_ >= count; });
    }

    void cancelAfterCommit(std::shared_ptr<std::stop_source> source) {
        std::scoped_lock lock(mutex_);
        cancelAfterCommit_ = std::move(source);
    }

    std::vector<Publication> publications() const {
        std::scoped_lock lock(mutex_);
        return publications_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::size_t admissionCount_{};
    std::shared_ptr<std::stop_source> cancelAfterCommit_;
    std::vector<Publication> publications_;
};

palmier::project::ProjectDocument projectDocument(std::string projectName = "Project") {
    const auto source = std::string(R"({
        "timelines":[{
            "id":"timeline","name":")") + projectName + R"(",
            "fps":30,"width":1920,"height":1080,
            "tracks":[{"id":"track","type":"video","clips":[{
                "id":"target","mediaRef":"media","mediaType":"video",
                "sourceClipType":"video","startFrame":0,"durationFrames":120,
                "speed":1,"opacity":1,"blendMode":"normal"
            }]}]
        }],
        "activeTimelineId":"timeline","openTimelineIds":["timeline"]
    })";
    return palmier::project::readProject(source, [] {
        return std::string("unexpected-reader-id");
    });
}

template<typename Operation>
void requireRuntimeError(Operation operation, const std::string& code) {
    try {
        operation();
    } catch (const ProjectRuntimeError& error) {
        require(error.code == code, "unexpected runtime error: " + error.code);
        return;
    }
    throw std::runtime_error("expected project runtime error: " + code);
}

void mutationPublishesOneSessionState() {
    auto observer = std::make_shared<RuntimeObserver>();
    ProjectRuntime runtime(observer);
    int nextId = 0;
    const auto installed = runtime.install(projectDocument(), 1, [&] {
        return "runtime-id-" + std::to_string(++nextId);
    });
    require(installed.projectGeneration == 1, "install generation");
    require(installed.session->revision == 0 && !installed.session->dirty(), "install state");

    const auto split = runtime.splitClips(SplitClipsCommand{
        std::vector<SplitPoint>{{"target", 40}}, std::nullopt, std::nullopt,
    });
    require(split.projectGeneration == 1, "split generation");
    require(split.command.changed && split.command.revisionAfter == 1, "split receipt");
    require(split.session->revision == 1 && split.session->dirty(), "split publication");
    const auto timeline = runtime.getTimeline();
    require(timeline.projectGeneration == 1 && timeline.revision == 1, "query identity");
    require(
        timeline.timeline.find("tracks")->array().front().find("clips")->array().size() == 2,
        "query reads split state"
    );

    const auto undo = runtime.undo();
    require(undo.command.revisionAfter == 2, "undo receipt");
    require(
        undo.session->stateId == 0 && !undo.session->dirty() && undo.session->redoDepth == 1,
        "undo publication"
    );
    require(
        runtime.getTimeline().timeline.find("tracks")->array().front()
            .find("clips")->array().size() == 1,
        "undo restores runtime state"
    );
    const auto redo = runtime.redo();
    require(redo.command.revisionAfter == 3, "redo receipt");
    require(
        redo.session->stateId == 1
            && redo.session->dirty()
            && redo.session->undoDepth == 1
            && redo.session->redoDepth == 0,
        "redo publication"
    );
    require(
        runtime.getTimeline().timeline.find("tracks")->array().front()
            .find("clips")->array().size() == 2,
        "redo restores runtime state"
    );
    const auto publications = observer->publications();
    require(publications.size() == 4, "install, split, undo, and redo each publish once");
    require(
        publications[0].projectGeneration == 1
        && publications[0].revision == 0
        && publications[0].stateId == 0,
        "install publication identity"
    );
    require(
        publications[1].revision == 1
        && publications[1].stateId == 1
        && publications[2].revision == 2
        && publications[2].stateId == 0
        && publications[3].revision == 3
        && publications[3].stateId == 1,
        "mutation publications preserve exact session identity"
    );
}

void moveNoOpDoesNotPublishOrAdvanceHistory() {
    auto observer = std::make_shared<RuntimeObserver>();
    ProjectRuntime runtime(observer);
    int nextId{};
    runtime.install(projectDocument(), 1, [&] {
        return "runtime-move-id-" + std::to_string(++nextId);
    });
    const auto noOp = runtime.moveClips(MoveClipsCommand{{
        ClipMove{"target", std::nullopt, std::int64_t{0}},
    }});
    require(!noOp.command.changed, "exact move should not change runtime state");
    require(noOp.session->revision == 0 && noOp.session->undoDepth == 0, "move no-op history");
    require(observer->publications().size() == 1, "move no-op must not publish state");

    const auto moved = runtime.moveClips(MoveClipsCommand{{
        ClipMove{"target", std::nullopt, std::int64_t{200}},
    }});
    require(moved.command.changed && moved.session->revision == 1, "runtime move commit");
    require(moved.session->undoDepth == 1 && moved.session->dirty(), "runtime move identity");
    const auto publications = observer->publications();
    require(publications.size() == 2, "changed move should publish exactly once");
    require(publications.back().revision == 1, "move publication revision");
}

void removePublishesOneSharedStateAndUndo() {
    auto observer = std::make_shared<RuntimeObserver>();
    ProjectRuntime runtime(observer);
    runtime.install(projectDocument(), 1, [] { return std::string("runtime-remove-id"); });
    const auto removed = runtime.removeClips(RemoveClipsCommand{{"target"}}, 1);
    require(removed.command.changed && removed.session->revision == 1, "runtime remove commit");
    require(removed.session->undoDepth == 1 && removed.session->dirty(), "runtime remove identity");
    require(observer->publications().size() == 2, "remove should publish exactly once");
    const auto restored = runtime.undo(1);
    require(restored.session->undoDepth == 0 && !restored.session->dirty(), "runtime remove undo");
    require(observer->publications().size() == 3, "remove undo should publish exactly once");
}

void dirtyAndGenerationGatesProtectReplacement() {
    ProjectRuntime runtime;
    int nextId = 0;
    runtime.install(projectDocument("First"), 1, [&] {
        return "first-id-" + std::to_string(++nextId);
    });
    static_cast<void>(runtime.splitClips(SplitClipsCommand{
        std::vector<SplitPoint>{{"target", 40}}, std::nullopt, std::nullopt,
    }));
    requireRuntimeError(
        [&] {
            static_cast<void>(runtime.install(
                projectDocument("Second"),
                2,
                [] { return std::string("second-id"); }
            ));
        },
        "dirtyProject"
    );
    require(runtime.snapshot().projectGeneration == 1, "dirty refusal keeps active project");

    const auto replaced = runtime.install(
        projectDocument("Second"),
        2,
        [] { return std::string("second-id"); },
        true
    );
    require(replaced.projectGeneration == 2 && !replaced.session->dirty(), "explicit discard replacement");
    requireRuntimeError(
        [&] {
            static_cast<void>(runtime.install(
                projectDocument("Stale"),
                2,
                [] { return std::string("stale-id"); },
                true
            ));
        },
        "staleProjectGeneration"
    );
    requireRuntimeError(
        [&] {
            static_cast<void>(runtime.install(
                projectDocument("Invalid"),
                0,
                [] { return std::string("invalid-id"); },
                true
            ));
        },
        "invalidProjectGeneration"
    );
    requireRuntimeError(
        [&] {
            static_cast<void>(runtime.splitClips(
                SplitClipsCommand{
                    std::vector<SplitPoint>{{"target", 40}}, std::nullopt, std::nullopt,
                },
                1
            ));
        },
        "staleProjectGeneration"
    );
    require(runtime.snapshot(2).session->revision == 0, "stale command cannot mutate replacement");
}

void persistenceAcknowledgementPublishesOnlyOnChange() {
    auto observer = std::make_shared<RuntimeObserver>();
    ProjectRuntime runtime(observer);
    int nextId = 0;
    runtime.install(projectDocument(), 1, [&] {
        return "persist-id-" + std::to_string(++nextId);
    });
    static_cast<void>(runtime.markPersisted(0));
    require(
        observer->publications().size() == 1,
        "unchanged persistence acknowledgement must not republish state"
    );
    const auto split = runtime.splitClips(SplitClipsCommand{
        std::vector<SplitPoint>{{"target", 40}}, std::nullopt, std::nullopt,
    });
    const auto persisted = runtime.markPersisted(split.session->stateId);
    require(!persisted.session->dirty(), "persistence acknowledgement clears dirty state");
    static_cast<void>(runtime.markPersisted(split.session->stateId));
    const auto publications = observer->publications();
    require(publications.size() == 3, "changed persistence acknowledgement publishes once");
    require(
        publications.back().stateId == split.session->stateId
        && publications.back().persistedStateId == split.session->stateId,
        "persistence publication carries exact content identity"
    );
}

void saveSnapshotCarriesExactRuntimeIdentity() {
    ProjectRuntime runtime;
    int nextId = 0;
    runtime.install(projectDocument(), 7, [&] {
        return "save-id-" + std::to_string(++nextId);
    });
    const auto split = runtime.splitClips(
        SplitClipsCommand{
            std::vector<SplitPoint>{{"target", 40}}, std::nullopt, std::nullopt,
        },
        7
    );
    const auto saved = runtime.saveSnapshot(7);
    require(saved.projectGeneration == 7, "save snapshot generation");
    require(saved.snapshot.revision == split.session->revision, "save snapshot revision");
    require(saved.snapshot.stateId == split.session->stateId, "save snapshot state identity");
    require(
        saved.snapshot.source.find("timelines") != nullptr,
        "save snapshot retains the full source document"
    );
    requireRuntimeError(
        [&] { static_cast<void>(runtime.saveSnapshot(6)); },
        "staleProjectGeneration"
    );
}

void operationsAreSerializedAndQueuedCancellationDoesNotCommit() {
    auto observer = std::make_shared<RuntimeObserver>();
    ProjectRuntime runtime(observer);
    std::mutex gateMutex;
    std::condition_variable gateCondition;
    bool generatorEntered{};
    bool releaseGenerator{};
    int nextId = 0;
    runtime.install(projectDocument(), 1, [&] {
        std::unique_lock lock(gateMutex);
        generatorEntered = true;
        gateCondition.notify_all();
        gateCondition.wait(lock, [&] { return releaseGenerator; });
        return "serial-id-" + std::to_string(++nextId);
    });

    std::exception_ptr splitFailure;
    std::jthread splitThread([&] {
        try {
            static_cast<void>(runtime.splitClips(SplitClipsCommand{
                std::vector<SplitPoint>{{"target", 40}}, std::nullopt, std::nullopt,
            }));
        } catch (...) {
            splitFailure = std::current_exception();
        }
    });
    {
        std::unique_lock lock(gateMutex);
        gateCondition.wait(lock, [&] { return generatorEntered; });
    }

    std::stop_source cancellation;
    std::string queryError;
    std::jthread queryThread([&] {
        try {
            static_cast<void>(runtime.getTimeline(
                {},
                std::nullopt,
                cancellation.get_token()
            ));
        } catch (const ProjectRuntimeError& error) {
            queryError = error.code;
        }
    });
    observer->waitForAdmissions(3);
    cancellation.request_stop();
    {
        std::scoped_lock lock(gateMutex);
        releaseGenerator = true;
    }
    gateCondition.notify_all();
    splitThread.join();
    queryThread.join();
    if (splitFailure) std::rethrow_exception(splitFailure);
    require(queryError == "cancelled", "queued cancellation must be observed");
    require(runtime.snapshot().session->revision == 1, "cancelled query cannot mutate runtime");
}

void cancellationAfterCommitStillPublishesSuccess() {
    auto observer = std::make_shared<RuntimeObserver>();
    ProjectRuntime runtime(observer);
    int nextId = 0;
    runtime.install(projectDocument(), 1, [&] {
        return "late-cancel-id-" + std::to_string(++nextId);
    });
    auto cancellation = std::make_shared<std::stop_source>();
    observer->cancelAfterCommit(cancellation);
    const auto result = runtime.splitClips(
        SplitClipsCommand{
            std::vector<SplitPoint>{{"target", 40}}, std::nullopt, std::nullopt,
        },
        1,
        cancellation->get_token()
    );
    require(cancellation->stop_requested(), "commit checkpoint must request cancellation");
    require(result.command.changed && result.session->revision == 1, "committed operation must publish success");
    const auto publications = observer->publications();
    require(
        publications.size() == 2 && publications.back().revision == 1,
        "late cancellation cannot suppress committed state publication"
    );
}

void reentrancyAndCloseAreTerminal() {
    ProjectRuntime runtime;
    runtime.install(projectDocument(), 1, [&] {
        static_cast<void>(runtime.snapshot());
        return std::string("unreachable-id");
    });
    requireRuntimeError(
        [&] {
            static_cast<void>(runtime.splitClips(SplitClipsCommand{
                std::vector<SplitPoint>{{"target", 40}}, std::nullopt, std::nullopt,
            }));
        },
        "reentrantRuntimeCall"
    );
    require(runtime.snapshot().session->revision == 0, "reentrant failure cannot commit");

    std::jthread firstClose([&] { runtime.close(); });
    std::jthread secondClose([&] { runtime.close(); });
    firstClose.join();
    secondClose.join();
    requireRuntimeError(
        [&] { static_cast<void>(runtime.snapshot()); },
        "runtimeClosed"
    );
}

void emptyRuntimeRefusesQueries() {
    ProjectRuntime runtime;
    requireRuntimeError(
        [&] { static_cast<void>(runtime.snapshot()); },
        "noActiveProject"
    );
}

}

int main() {
    try {
        mutationPublishesOneSessionState();
        moveNoOpDoesNotPublishOrAdvanceHistory();
        removePublishesOneSharedStateAndUndo();
        dirtyAndGenerationGatesProtectReplacement();
        persistenceAcknowledgementPublishesOnlyOnChange();
        saveSnapshotCarriesExactRuntimeIdentity();
        operationsAreSerializedAndQueuedCancellationDoesNotCommit();
        cancellationAfterCommitStillPublishesSuccess();
        reentrancyAndCloseAreTerminal();
        emptyRuntimeRefusesQueries();
        std::cout << "PALMIER_PROJECT_RUNTIME_TESTS_OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "PALMIER_PROJECT_RUNTIME_TESTS_FAILED " << error.what() << '\n';
        return 1;
    }
}
