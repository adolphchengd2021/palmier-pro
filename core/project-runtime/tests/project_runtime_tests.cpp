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

using palmier::project::ProjectRuntime;
using palmier::project::ProjectRuntimeError;
using palmier::project::ProjectRuntimeObserver;
using palmier::project::SplitClipsCommand;
using palmier::project::SplitPoint;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

class RuntimeObserver final : public ProjectRuntimeObserver {
public:
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

    void waitForAdmissions(std::size_t count) {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [&] { return admissionCount_ >= count; });
    }

    void cancelAfterCommit(std::shared_ptr<std::stop_source> source) {
        std::scoped_lock lock(mutex_);
        cancelAfterCommit_ = std::move(source);
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    std::size_t admissionCount_{};
    std::shared_ptr<std::stop_source> cancelAfterCommit_;
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
    ProjectRuntime runtime;
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
    require(undo.session->stateId == 0 && !undo.session->dirty(), "undo publication");
    require(
        runtime.getTimeline().timeline.find("tracks")->array().front()
            .find("clips")->array().size() == 1,
        "undo restores runtime state"
    );
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
        dirtyAndGenerationGatesProtectReplacement();
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
