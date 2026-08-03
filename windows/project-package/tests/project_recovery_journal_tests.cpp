#include "palmier/project/project_recovery_journal.hpp"

#include "internal/project_recovery_journal_testing.hpp"

#include "palmier/json/json_document.hpp"
#include "palmier/project/project_package_reader.hpp"

#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <random>
#include <semaphore>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using palmier::project::ProjectRecoveryJournal;
using palmier::project::ProjectRecoveryJournalError;
using palmier::project::ProjectRecoveryJournalStatus;
using palmier::project::ProjectRuntime;
using palmier::project::SetClipPropertiesCommand;
using palmier::project::SplitClipsCommand;
using palmier::project::SplitPoint;
using palmier::project::testing::ProjectRecoveryJournalCheckpoint;
using palmier::project::testing::ProjectRecoveryJournalCheckpoints;

constexpr std::string_view minimalProject = R"({
    "timelines":[{
        "id":"timeline","name":"Timeline","fps":30,"width":1920,"height":1080,
        "tracks":[{"id":"track","type":"video","clips":[{
            "id":"target","mediaRef":"media","mediaType":"video",
            "sourceClipType":"video","startFrame":0,"durationFrames":120,
            "trimStartFrame":0,"trimEndFrame":0,"speed":1,"volume":1,
            "opacity":1,"blendMode":"normal","linkGroupId":null,
            "captionGroupId":null,"multicamGroupId":null
        },{
            "id":"canary","mediaRef":"media","mediaType":"video",
            "sourceClipType":"video","startFrame":120,"durationFrames":30,
            "trimStartFrame":0,"trimEndFrame":0,"speed":1,"volume":1,
            "opacity":1,"blendMode":"normal","linkGroupId":null,
            "captionGroupId":null,"multicamGroupId":null,
            "x-recovery-clip":{"keep":[1,2,3]}
        }]}]
    }],
    "activeTimelineId":"timeline","openTimelineIds":["timeline"],
    "x-recovery-root":{"future":true}
})";

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::string readText(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot read recovery test file");
    return {
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()
    };
}

void writeText(const std::filesystem::path& path, std::string_view content) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) throw std::runtime_error("cannot write recovery test file");
    stream.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!stream) throw std::runtime_error("cannot finish recovery test file");
}

void replaceJournalField(
    const std::filesystem::path& path,
    std::string field,
    palmier::json::Value value
) {
    auto journal = palmier::json::parse(readText(path));
    journal.object().insert_or_assign(std::move(field), std::move(value));
    writeText(path, palmier::json::canonical(journal));
}

class TemporaryWorkspace final {
public:
    TemporaryWorkspace() {
        std::random_device random;
        for (int attempt = 0; attempt < 32; ++attempt) {
            const auto candidate = std::filesystem::temp_directory_path()
                / (
                    "palmier-recovery-tests-"
                    + std::to_string(random())
                    + "-"
                    + std::to_string(random())
                );
            std::error_code error;
            if (std::filesystem::create_directory(candidate, error)) {
                root_ = candidate;
                package_ = root_ / "Movie Project.palmier";
                recovery_ = root_ / "recovery";
                std::filesystem::create_directory(package_);
                writeText(package_ / "project.json", minimalProject);
                return;
            }
        }
        throw std::runtime_error("cannot create recovery test workspace");
    }

    ~TemporaryWorkspace() {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    const std::filesystem::path& package() const noexcept { return package_; }
    const std::filesystem::path& recovery() const noexcept { return recovery_; }

    void requireNoPartialFiles() const {
        if (!std::filesystem::exists(recovery_)) return;
        for (const auto& entry : std::filesystem::directory_iterator(recovery_)) {
            require(
                !entry.path().filename().native().ends_with(L".partial"),
                "recovery write left a staging file"
            );
        }
    }

private:
    std::filesystem::path root_;
    std::filesystem::path package_;
    std::filesystem::path recovery_;
};

auto generatedIds(std::shared_ptr<int> next = std::make_shared<int>(0)) {
    return [next] { return "recovery-generated-" + std::to_string(++*next); };
}

void installRuntime(
    ProjectRuntime& runtime,
    const TemporaryWorkspace& workspace,
    std::uint64_t generation
) {
    static_cast<void>(runtime.install(
        palmier::project::readProjectPackage(workspace.package(), generatedIds()),
        generation,
        generatedIds()
    ));
}

void split(ProjectRuntime& runtime, std::uint64_t generation) {
    static_cast<void>(runtime.splitClips(
        SplitClipsCommand{
            std::vector<SplitPoint>{{"target", 60}},
            std::nullopt,
            std::nullopt,
        },
        generation
    ));
}

void shortenFirstClip(ProjectRuntime& runtime, std::uint64_t generation) {
    static_cast<void>(runtime.setClipProperties(
        SetClipPropertiesCommand{
            std::vector<std::string>{"target"},
            50,
            std::nullopt,
            std::nullopt,
            std::nullopt,
        },
        generation
    ));
}

template<typename Operation>
void requireRecoveryError(Operation operation, const std::string& code) {
    try {
        operation();
    } catch (const ProjectRecoveryJournalError& error) {
        require(error.code == code, "unexpected recovery error: " + error.code);
        return;
    }
    throw std::runtime_error("expected recovery error: " + code);
}

class CancelAtCheckpoint final : public ProjectRecoveryJournalCheckpoints {
public:
    CancelAtCheckpoint(
        ProjectRecoveryJournalCheckpoint target,
        std::stop_source& cancellation
    ) : target_(target), cancellation_(cancellation) {}

    void arrive(ProjectRecoveryJournalCheckpoint checkpoint) noexcept override {
        if (checkpoint == target_) cancellation_.request_stop();
    }

private:
    ProjectRecoveryJournalCheckpoint target_;
    std::stop_source& cancellation_;
};

class GateAfterSnapshot final : public ProjectRecoveryJournalCheckpoints {
public:
    void arrive(ProjectRecoveryJournalCheckpoint checkpoint) noexcept override {
        if (checkpoint != ProjectRecoveryJournalCheckpoint::afterSnapshot) return;
        entered.release();
        proceed.acquire();
    }

    std::binary_semaphore entered{0};
    std::binary_semaphore proceed{0};
};

void dirtySnapshotRoundTripsWithoutTouchingPackage() {
    TemporaryWorkspace workspace;
    ProjectRuntime runtime;
    constexpr std::uint64_t generation = 41;
    installRuntime(runtime, workspace, generation);
    split(runtime, generation);
    const auto baseline = readText(workspace.package() / "project.json");
    ProjectRecoveryJournal journal(workspace.recovery());

    const auto receipt = journal.write(runtime, workspace.package(), generation);
    require(receipt.projectGeneration == generation, "recovery generation mismatch");
    require(receipt.revision == 1 && receipt.stateId == 1, "recovery state mismatch");
    require(receipt.projectJsonBytes > 0 && receipt.journalBytes > receipt.projectJsonBytes, "recovery size receipt mismatch");
    require(std::filesystem::exists(receipt.journalPath), "recovery journal was not installed");
    require(
        receipt.journalPath.filename().string().find("Movie Project") == std::string::npos,
        "recovery filename exposed the project path"
    );
    require(readText(workspace.package() / "project.json") == baseline, "recovery write changed the live project");

    const auto inspection = journal.inspect(workspace.package());
    require(inspection.status == ProjectRecoveryJournalStatus::recoverable, "recovery candidate is not recoverable");
    require(inspection.candidate.has_value(), "recoverable inspection omitted candidate");
    require(inspection.candidate->revision == 1, "recovery candidate revision mismatch");
    const auto recovered = palmier::json::parse(inspection.candidate->projectJson);
    require(
        recovered.find("x-recovery-root") != nullptr,
        "recovery candidate discarded unknown root data"
    );
    const auto* timelines = recovered.find("timelines");
    require(
        timelines != nullptr && timelines->kind() == palmier::json::Value::Kind::array
            && !timelines->array().empty(),
        "recovery candidate lost timelines"
    );
    const auto* tracks = timelines->array().front().find("tracks");
    require(
        tracks != nullptr && tracks->kind() == palmier::json::Value::Kind::array
            && !tracks->array().empty(),
        "recovery candidate lost tracks"
    );
    const auto* clipValue = tracks->array().front().find("clips");
    require(
        clipValue != nullptr && clipValue->kind() == palmier::json::Value::Kind::array,
        "recovery candidate lost clips"
    );
    const auto& clips = clipValue->array();
    require(clips.size() == 3, "recovery candidate did not retain the split");
    const auto canary = std::ranges::find_if(clips, [](const auto& clip) {
        const auto* id = clip.find("id");
        return id != nullptr
            && id->kind() == palmier::json::Value::Kind::string
            && id->string() == "canary";
    });
    require(
        canary != clips.end() && canary->find("x-recovery-clip") != nullptr,
        "recovery candidate discarded unknown clip data"
    );
    workspace.requireNoPartialFiles();
}

void changedBaselineIsStaleAndNeverApplied() {
    TemporaryWorkspace workspace;
    ProjectRuntime runtime;
    constexpr std::uint64_t generation = 42;
    installRuntime(runtime, workspace, generation);
    split(runtime, generation);
    ProjectRecoveryJournal journal(workspace.recovery());
    static_cast<void>(journal.write(runtime, workspace.package(), generation));

    writeText(
        workspace.package() / "project.json",
        R"({"timelines":[],"activeTimelineId":"external","openTimelineIds":[]})"
    );
    const auto inspection = journal.inspect(workspace.package());
    require(inspection.status == ProjectRecoveryJournalStatus::staleBaseline, "external change was not detected");
    require(inspection.candidate.has_value(), "stale recovery metadata was lost");
    require(
        readText(workspace.package() / "project.json").find("external") != std::string::npos,
        "stale inspection modified the live project"
    );
}

void diskEquivalentCandidateIsRedundant() {
    TemporaryWorkspace workspace;
    ProjectRuntime runtime;
    constexpr std::uint64_t generation = 47;
    installRuntime(runtime, workspace, generation);
    split(runtime, generation);
    const auto saved = runtime.saveSnapshot(generation);
    writeText(
        workspace.package() / "project.json",
        palmier::json::canonical(saved.snapshot.source)
    );
    ProjectRecoveryJournal journal(workspace.recovery());
    static_cast<void>(journal.write(runtime, workspace.package(), generation));

    const auto inspection = journal.inspect(workspace.package());
    require(
        inspection.status == ProjectRecoveryJournalStatus::redundant,
        "disk-equivalent recovery candidate was offered"
    );
    require(inspection.candidate.has_value(), "redundant recovery metadata was lost");
}

void retirementRequiresMatchingGenerationAndCommittedRevision() {
    TemporaryWorkspace workspace;
    ProjectRuntime runtime;
    constexpr std::uint64_t generation = 43;
    installRuntime(runtime, workspace, generation);
    split(runtime, generation);
    ProjectRecoveryJournal journal(workspace.recovery());
    static_cast<void>(journal.write(runtime, workspace.package(), generation));

    require(!journal.retire(workspace.package(), generation + 1, 1), "wrong generation retired recovery");
    require(!journal.retire(workspace.package(), generation, 0), "older commit retired recovery");
    shortenFirstClip(runtime, generation);
    static_cast<void>(journal.write(runtime, workspace.package(), generation));
    require(!journal.retire(workspace.package(), generation, 1), "older commit retired newer recovery");
    std::stop_source cancellation;
    cancellation.request_stop();
    requireRecoveryError(
        [&] {
            static_cast<void>(journal.retire(
                workspace.package(),
                generation,
                2,
                cancellation.get_token()
            ));
        },
        "cancelled"
    );
    require(
        journal.inspect(workspace.package()).status == ProjectRecoveryJournalStatus::recoverable,
        "cancelled retirement removed recovery"
    );
    require(journal.retire(workspace.package(), generation, 2), "matching commit did not retire recovery");
    require(
        journal.inspect(workspace.package()).status == ProjectRecoveryJournalStatus::missing,
        "retired recovery journal remained discoverable"
    );
    require(!journal.retire(workspace.package(), generation, 2), "missing recovery reported retirement");
}

void interruptedReplacementPreservesPreviousJournal() {
    TemporaryWorkspace workspace;
    ProjectRuntime runtime;
    constexpr std::uint64_t generation = 44;
    installRuntime(runtime, workspace, generation);
    split(runtime, generation);
    ProjectRecoveryJournal journal(workspace.recovery());
    const auto first = journal.write(runtime, workspace.package(), generation);
    const auto previous = readText(first.journalPath);
    shortenFirstClip(runtime, generation);

    const std::vector checkpoints{
        ProjectRecoveryJournalCheckpoint::afterSnapshot,
        ProjectRecoveryJournalCheckpoint::afterBaselineRead,
        ProjectRecoveryJournalCheckpoint::afterSerialization,
        ProjectRecoveryJournalCheckpoint::afterStagingCreation,
        ProjectRecoveryJournalCheckpoint::afterWrite,
        ProjectRecoveryJournalCheckpoint::afterFlush,
        ProjectRecoveryJournalCheckpoint::beforeCommit,
    };
    for (const auto checkpoint : checkpoints) {
        std::stop_source cancellation;
        CancelAtCheckpoint injected(checkpoint, cancellation);
        requireRecoveryError(
            [&] {
                static_cast<void>(palmier::project::testing::writeProjectRecoveryJournal(
                    journal,
                    runtime,
                    workspace.package(),
                    generation,
                    cancellation.get_token(),
                    &injected
                ));
            },
            "cancelled"
        );
        require(readText(first.journalPath) == previous, "cancelled recovery replaced prior journal");
        const auto inspection = journal.inspect(workspace.package());
        require(
            inspection.status == ProjectRecoveryJournalStatus::recoverable
                && inspection.candidate.has_value()
                && inspection.candidate->revision == 1,
            "cancelled recovery exposed partial state"
        );
        workspace.requireNoPartialFiles();
    }

    const auto second = journal.write(runtime, workspace.package(), generation);
    require(second.revision == 2, "post-cancellation recovery did not advance");
    require(readText(second.journalPath) != previous, "new recovery did not replace old journal");
    workspace.requireNoPartialFiles();
}

void cleanAndCorruptJournalsFailExplicitly() {
    TemporaryWorkspace workspace;
    ProjectRuntime runtime;
    constexpr std::uint64_t generation = 45;
    installRuntime(runtime, workspace, generation);
    ProjectRecoveryJournal journal(workspace.recovery());
    requireRecoveryError(
        [&] { static_cast<void>(journal.write(runtime, workspace.package(), generation)); },
        "projectClean"
    );

    split(runtime, generation);
    const auto receipt = journal.write(runtime, workspace.package(), generation);
    writeText(receipt.journalPath, R"({"contractVersion":1,"project":{}})");
    requireRecoveryError(
        [&] { static_cast<void>(journal.inspect(workspace.package())); },
        "invalidRecoveryJournal"
    );
}

void tamperedIdentityAndPayloadFailExplicitly() {
    TemporaryWorkspace workspace;
    ProjectRuntime runtime;
    constexpr std::uint64_t generation = 49;
    installRuntime(runtime, workspace, generation);
    split(runtime, generation);
    ProjectRecoveryJournal journal(workspace.recovery());
    const auto receipt = journal.write(runtime, workspace.package(), generation);
    const auto validJournal = readText(receipt.journalPath);

    replaceJournalField(
        receipt.journalPath,
        "packagePath",
        palmier::json::Value("C:\\Elsewhere\\Other.palmier")
    );
    requireRecoveryError(
        [&] { static_cast<void>(journal.inspect(workspace.package())); },
        "recoveryIdentityMismatch"
    );

    writeText(receipt.journalPath, validJournal);
    auto payload = palmier::json::parse(validJournal);
    payload.object().at("project").object().insert_or_assign(
        "x-tampered",
        palmier::json::Value(true)
    );
    writeText(receipt.journalPath, palmier::json::canonical(payload));
    requireRecoveryError(
        [&] { static_cast<void>(journal.inspect(workspace.package())); },
        "recoveryPayloadMismatch"
    );

    writeText(receipt.journalPath, validJournal);
    replaceJournalField(
        receipt.journalPath,
        "persistedStateId",
        palmier::json::Value(palmier::json::Number{"1", 1})
    );
    requireRecoveryError(
        [&] { static_cast<void>(journal.inspect(workspace.package())); },
        "invalidRecoveryJournal"
    );
}

void concurrentWriterWaitIsCancellable() {
    TemporaryWorkspace workspace;
    ProjectRuntime runtime;
    constexpr std::uint64_t generation = 46;
    installRuntime(runtime, workspace, generation);
    split(runtime, generation);
    ProjectRecoveryJournal journal(workspace.recovery());
    GateAfterSnapshot gate;
    std::exception_ptr firstFailure;
    std::jthread first([&] {
        try {
            static_cast<void>(palmier::project::testing::writeProjectRecoveryJournal(
                journal,
                runtime,
                workspace.package(),
                generation,
                {},
                &gate
            ));
        } catch (...) {
            firstFailure = std::current_exception();
        }
    });
    gate.entered.acquire();

    std::stop_source cancellation;
    std::binary_semaphore secondStarted{0};
    std::exception_ptr secondFailure;
    std::jthread second([&] {
        secondStarted.release();
        try {
            static_cast<void>(journal.write(
                runtime,
                workspace.package(),
                generation,
                cancellation.get_token()
            ));
        } catch (...) {
            secondFailure = std::current_exception();
        }
    });
    secondStarted.acquire();
    cancellation.request_stop();
    gate.proceed.release();
    first.join();
    second.join();
    require(secondFailure != nullptr, "cancelled waiting recovery writer succeeded");
    try {
        std::rethrow_exception(secondFailure);
    } catch (const ProjectRecoveryJournalError& error) {
        require(error.code == "cancelled", "waiting recovery returned wrong error");
        require(error.stage == "waitForRecoveryWriter", "waiting recovery returned wrong stage");
    }

    if (firstFailure) std::rethrow_exception(firstFailure);
    require(
        journal.inspect(workspace.package()).status == ProjectRecoveryJournalStatus::recoverable,
        "admitted recovery writer did not complete"
    );
    workspace.requireNoPartialFiles();
}

void cancellationAfterCommitKeepsInstalledJournal() {
    TemporaryWorkspace workspace;
    ProjectRuntime runtime;
    constexpr std::uint64_t generation = 48;
    installRuntime(runtime, workspace, generation);
    split(runtime, generation);
    ProjectRecoveryJournal journal(workspace.recovery());
    std::stop_source cancellation;
    CancelAtCheckpoint injected(
        ProjectRecoveryJournalCheckpoint::afterCommit,
        cancellation
    );

    const auto receipt = palmier::project::testing::writeProjectRecoveryJournal(
        journal,
        runtime,
        workspace.package(),
        generation,
        cancellation.get_token(),
        &injected
    );
    require(cancellation.stop_requested(), "post-commit cancellation was not injected");
    require(std::filesystem::exists(receipt.journalPath), "post-commit cancellation removed journal");
    require(
        journal.inspect(workspace.package()).status == ProjectRecoveryJournalStatus::recoverable,
        "post-commit cancellation hid installed journal"
    );
}

void runTests() {
    dirtySnapshotRoundTripsWithoutTouchingPackage();
    changedBaselineIsStaleAndNeverApplied();
    diskEquivalentCandidateIsRedundant();
    retirementRequiresMatchingGenerationAndCommittedRevision();
    interruptedReplacementPreservesPreviousJournal();
    cleanAndCorruptJournalsFailExplicitly();
    tamperedIdentityAndPayloadFailExplicitly();
    concurrentWriterWaitIsCancellable();
    cancellationAfterCommitKeepsInstalledJournal();
}

}

int main() {
    std::exception_ptr failure;
    std::jthread worker([&failure] {
        try {
            runTests();
        } catch (...) {
            failure = std::current_exception();
        }
    });
    worker.join();
    if (failure) {
        try {
            std::rethrow_exception(failure);
        } catch (const std::exception& error) {
            std::cerr << "PALMIER_PROJECT_RECOVERY_JOURNAL_TESTS_FAILED " << error.what() << '\n';
            return 1;
        }
    }
    std::cout << "PALMIER_PROJECT_RECOVERY_JOURNAL_TESTS_OK\n";
    return 0;
}
