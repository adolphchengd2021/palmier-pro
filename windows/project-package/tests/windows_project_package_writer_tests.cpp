#include "palmier/project/windows_project_package_writer.hpp"

#include "internal/windows_project_package_writer_testing.hpp"

#include "palmier/json/json_document.hpp"
#include "palmier/project/project_package_reader.hpp"

#define NOMINMAX
#include <Windows.h>

#include <charconv>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace {

using palmier::json::Value;
using palmier::project::CommandError;
using palmier::project::ProjectPackageWriteError;
using palmier::project::ProjectPackageWriteWarning;
using palmier::project::ProjectRuntime;
using palmier::project::SplitClipsCommand;
using palmier::project::SplitPoint;
using palmier::project::testing::ProjectPackageWriteCheckpoint;
using palmier::project::testing::ProjectPackageWriteCheckpoints;

constexpr std::string_view minimalProject = R"({
    "timelines":[{
        "id":"timeline","name":"Timeline","fps":30,"width":1920,"height":1080,
        "tracks":[{"id":"track","type":"video","clips":[{
            "id":"target","mediaRef":"media","mediaType":"video",
            "sourceClipType":"video","startFrame":0,"durationFrames":120,
            "trimStartFrame":0,"trimEndFrame":0,"speed":1,"volume":1,
            "opacity":1,"blendMode":"normal","linkGroupId":null,
            "captionGroupId":null,"multicamGroupId":null
        }]}]
    }],
    "activeTimelineId":"timeline","openTimelineIds":["timeline"]
})";

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::string readText(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot read test file");
    return {
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()
    };
}

void writeText(const std::filesystem::path& path, std::string_view content) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) throw std::runtime_error("cannot write test file");
    stream.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!stream) throw std::runtime_error("cannot finish test file");
}

class TemporaryPackage final {
public:
    TemporaryPackage() {
        std::random_device random;
        for (int attempt = 0; attempt < 32; ++attempt) {
            const auto candidate = std::filesystem::temp_directory_path()
                / ("palmier-writer-tests-" + std::to_string(random())
                    + "-" + std::to_string(random()) + ".palmier");
            std::error_code error;
            if (std::filesystem::create_directory(candidate, error)) {
                path_ = candidate;
                writeText(path_ / "project.json", minimalProject);
                return;
            }
        }
        throw std::runtime_error("cannot create unique writer test package");
    }

    explicit TemporaryPackage(const std::filesystem::path& source) : TemporaryPackage() {
        std::error_code error;
        std::filesystem::remove(path_ / "project.json", error);
        for (const auto& entry : std::filesystem::recursive_directory_iterator(source)) {
            const auto relative = std::filesystem::relative(entry.path(), source);
            const auto destination = path_ / relative;
            if (entry.is_directory()) {
                std::filesystem::create_directories(destination);
            } else if (entry.is_regular_file()) {
                std::filesystem::create_directories(destination.parent_path());
                std::filesystem::copy_file(
                    entry.path(),
                    destination,
                    std::filesystem::copy_options::overwrite_existing
                );
            }
        }
    }

    ~TemporaryPackage() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const noexcept { return path_; }

    void resetProject(std::string_view content = minimalProject) const {
        writeText(path_ / "project.json", content);
    }

    void requireNoStagingFiles() const {
        for (const auto& entry : std::filesystem::directory_iterator(path_)) {
            require(
                entry.path().filename().native().find(L".partial") == std::wstring::npos,
                "writer left a staging file"
            );
        }
    }

private:
    std::filesystem::path path_;
};

auto generatedIds(std::shared_ptr<int> next = std::make_shared<int>(0)) {
    return [next] { return "writer-generated-" + std::to_string(++*next); };
}

void installRuntime(
    ProjectRuntime& runtime,
    const std::filesystem::path& path,
    std::uint64_t generation = 1
) {
    runtime.install(
        palmier::project::readProjectPackage(path, generatedIds()),
        generation,
        generatedIds()
    );
}

template<typename Operation>
void requireWriteError(Operation operation, const std::string& code) {
    try {
        operation();
    } catch (const ProjectPackageWriteError& error) {
        require(error.code == code, "unexpected writer error: " + error.code);
        return;
    }
    throw std::runtime_error("expected writer error: " + code);
}

std::string decodePointerToken(std::string_view token) {
    std::string result;
    for (std::size_t index = 0; index < token.size(); ++index) {
        if (token[index] == '~' && index + 1 < token.size()) {
            if (token[index + 1] == '0') {
                result.push_back('~');
                ++index;
                continue;
            }
            if (token[index + 1] == '1') {
                result.push_back('/');
                ++index;
                continue;
            }
        }
        result.push_back(token[index]);
    }
    return result;
}

const Value& atPointer(const Value& root, std::string_view pointer) {
    require(pointer.empty() || pointer.front() == '/', "invalid canary pointer");
    const Value* current = &root;
    std::size_t start = pointer.empty() ? pointer.size() : 1;
    while (start < pointer.size()) {
        const auto separator = pointer.find('/', start);
        const auto token = decodePointerToken(pointer.substr(start, separator - start));
        if (current->kind() == Value::Kind::object) {
            current = current->find(token);
            require(current != nullptr, "missing canary object token: " + token);
        } else if (current->kind() == Value::Kind::array) {
            std::size_t index{};
            const auto result = std::from_chars(token.data(), token.data() + token.size(), index);
            require(
                result.ec == std::errc{} && result.ptr == token.data() + token.size(),
                "invalid canary array index"
            );
            require(index < current->array().size(), "canary array index out of range");
            current = &current->array()[index];
        } else {
            throw std::runtime_error("canary pointer crosses a scalar value");
        }
        if (separator == std::string_view::npos) break;
        start = separator + 1;
    }
    return *current;
}

void requireDeclaredCanaries(
    const std::filesystem::path& repositoryRoot,
    const std::filesystem::path& packagePath
) {
    const auto declaration = palmier::json::read(
        repositoryRoot / "contracts/project/v1/canaries.json"
    );
    const auto* canaries = declaration.find("canaries");
    require(canaries != nullptr && canaries->kind() == Value::Kind::array, "missing canaries");
    std::map<std::string, Value, std::less<>> files;
    for (const auto& entry : canaries->array()) {
        const auto* file = entry.find("file");
        const auto* pointer = entry.find("pointer");
        const auto* expected = entry.find("value");
        require(file && pointer && expected, "invalid canary declaration");
        auto [iterator, inserted] = files.try_emplace(file->string());
        if (inserted) iterator->second = palmier::json::read(packagePath / file->string());
        const auto& actual = atPointer(iterator->second, pointer->string());
        require(
            palmier::json::canonical(actual) == palmier::json::canonical(*expected),
            "canary changed: " + pointer->string()
        );
    }
}

class CancellingCheckpoint final : public ProjectPackageWriteCheckpoints {
public:
    CancellingCheckpoint(ProjectPackageWriteCheckpoint target, std::stop_source& source)
        : target_(target), source_(source) {}

    void arrive(ProjectPackageWriteCheckpoint checkpoint) noexcept override {
        if (checkpoint == target_) source_.request_stop();
    }

private:
    ProjectPackageWriteCheckpoint target_;
    std::stop_source& source_;
};

class RuntimeMutationCheckpoint final : public ProjectPackageWriteCheckpoints {
public:
    explicit RuntimeMutationCheckpoint(ProjectRuntime& runtime) : runtime_(runtime) {}

    void arrive(ProjectPackageWriteCheckpoint checkpoint) noexcept override {
        if (checkpoint != ProjectPackageWriteCheckpoint::afterSnapshot || mutated_) return;
        try {
            static_cast<void>(runtime_.splitClips(SplitClipsCommand{
                std::vector<SplitPoint>{{"target", 40}}, std::nullopt, std::nullopt,
            }));
            mutated_ = true;
        } catch (...) {
            failed_ = true;
        }
    }

    bool mutated() const noexcept { return mutated_; }
    bool failed() const noexcept { return failed_; }

private:
    ProjectRuntime& runtime_;
    bool mutated_{};
    bool failed_{};
};

class RuntimeCloseCheckpoint final : public ProjectPackageWriteCheckpoints {
public:
    explicit RuntimeCloseCheckpoint(ProjectRuntime& runtime) : runtime_(runtime) {}

    void arrive(ProjectPackageWriteCheckpoint checkpoint) noexcept override {
        if (checkpoint == ProjectPackageWriteCheckpoint::afterCommit) runtime_.close();
    }

private:
    ProjectRuntime& runtime_;
};

class FailingAcknowledgementCheckpoint final : public ProjectPackageWriteCheckpoints {
public:
    void arrive(ProjectPackageWriteCheckpoint) noexcept override {}
    bool failRuntimeAcknowledgement() const noexcept override { return true; }
};

class ExternalReplaceCheckpoint final : public ProjectPackageWriteCheckpoints {
public:
    explicit ExternalReplaceCheckpoint(std::filesystem::path path)
        : path_(std::move(path)), replacement_(path_.parent_path() / "external-replacement.tmp") {
        writeText(replacement_, "external-change");
    }

    void arrive(ProjectPackageWriteCheckpoint checkpoint) noexcept override {
        if (checkpoint != ProjectPackageWriteCheckpoint::beforeCommit || changed_) return;
        changed_ = MoveFileExW(
            replacement_.c_str(),
            path_.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
        ) != FALSE;
        if (!changed_) nativeError_ = GetLastError();
    }

    bool changed() const noexcept { return changed_; }
    DWORD nativeError() const noexcept { return nativeError_; }

private:
    std::filesystem::path path_;
    std::filesystem::path replacement_;
    bool changed_{};
    DWORD nativeError_{};
};

void editSaveRestartPreservesCanariesAndState(const std::filesystem::path& root) {
    TemporaryPackage package(root / "fixtures/contracts/projects/unknown-fields.palmier");
    requireDeclaredCanaries(root, package.path());
    ProjectRuntime runtime;
    installRuntime(runtime, package.path(), 11);
    const auto split = runtime.splitClips(
        SplitClipsCommand{
            std::vector<SplitPoint>{{"clip-save-target", 90}}, std::nullopt, std::nullopt,
        },
        11
    );
    require(split.command.changed && split.session->dirty(), "split did not make project dirty");
    const auto receipt = palmier::project::writeProjectPackage(runtime, package.path(), 11);
    require(receipt.projectGeneration == 11, "write generation");
    require(receipt.revision == 1 && receipt.stateId == split.session->stateId, "write identity");
    require(receipt.runtimeAcknowledged && !receipt.runtimeDirty, "write did not clear exact dirty state");
    require(receipt.warning == ProjectPackageWriteWarning::none, "successful write returned warning");
    package.requireNoStagingFiles();

    runtime.close();
    requireDeclaredCanaries(root, package.path());
    ProjectRuntime reopened;
    installRuntime(reopened, package.path(), 12);
    const auto timeline = reopened.getTimeline({}, 12).timeline;
    const auto& clips = timeline.find("tracks")->array().front().find("clips")->array();
    require(clips.size() == 3, "restart did not retain both split pieces");
    bool foundFirst{};
    bool foundSecond{};
    for (const auto& clip : clips) {
        const auto id = clip.find("id")->string();
        const auto start = clip.find("startFrame")->number().integer.value();
        const auto duration = clip.find("durationFrames")->number().integer.value();
        if (id == "clip-save-target") foundFirst = start == 60 && duration == 30;
        if (id.starts_with("writer-generated-")) foundSecond = start == 90 && duration == 30;
    }
    require(foundFirst && foundSecond, "restart changed split IDs or frame timing");
    require(!reopened.snapshot(12).session->dirty(), "reopened project must start persisted");
    try {
        static_cast<void>(reopened.undo(12));
        throw std::runtime_error("restart unexpectedly retained an undo action");
    } catch (const CommandError& error) {
        require(error.code == "nothingToUndo", "restart returned the wrong undo boundary");
    }
}

void savingAnOlderSnapshotLeavesNewerRuntimeDirty() {
    TemporaryPackage package;
    ProjectRuntime runtime;
    installRuntime(runtime, package.path());
    RuntimeMutationCheckpoint checkpoint(runtime);
    const auto receipt = palmier::project::testing::writeProjectPackage(
        runtime,
        package.path(),
        1,
        {},
        &checkpoint
    );
    require(checkpoint.mutated() && !checkpoint.failed(), "runtime mutation hook failed");
    require(receipt.runtimeAcknowledged && receipt.runtimeDirty, "newer edit was marked persisted");
    const auto persisted = runtime.snapshot(1).session;
    require(persisted->revision == 1 && persisted->dirty(), "newer runtime state lost dirty flag");
    auto disk = palmier::project::readProjectPackage(package.path(), generatedIds());
    require(disk.project().timelines.front().tracks.front().clips.size() == 1, "save snapshot was not isolated");
}

void committedSaveReportsClosedRuntimeAsWarning() {
    TemporaryPackage package;
    ProjectRuntime runtime;
    installRuntime(runtime, package.path());
    static_cast<void>(runtime.splitClips(SplitClipsCommand{
        std::vector<SplitPoint>{{"target", 40}}, std::nullopt, std::nullopt,
    }));
    RuntimeCloseCheckpoint closeRuntime(runtime);
    const auto receipt = palmier::project::testing::writeProjectPackage(
        runtime,
        package.path(),
        1,
        {},
        &closeRuntime
    );
    require(!receipt.runtimeAcknowledged, "closed runtime was acknowledged");
    require(receipt.runtimeDirty, "unacknowledged save must be conservatively dirty");
    require(
        receipt.warning == ProjectPackageWriteWarning::runtimeClosedAfterSave,
        "closed runtime warning was not preserved"
    );
    const auto reopened = palmier::project::readProjectPackage(package.path(), generatedIds());
    require(
        reopened.project().timelines.front().tracks.front().clips.size() == 2,
        "closed runtime hid a committed disk save"
    );
}

void committedSaveConvertsUnexpectedAcknowledgementFailureToWarning() {
    TemporaryPackage package;
    ProjectRuntime runtime;
    installRuntime(runtime, package.path());
    static_cast<void>(runtime.splitClips(SplitClipsCommand{
        std::vector<SplitPoint>{{"target", 40}}, std::nullopt, std::nullopt,
    }));
    FailingAcknowledgementCheckpoint failure;
    const auto receipt = palmier::project::testing::writeProjectPackage(
        runtime,
        package.path(),
        1,
        {},
        &failure
    );
    require(!receipt.runtimeAcknowledged && receipt.runtimeDirty, "failure was acknowledged");
    require(
        receipt.warning == ProjectPackageWriteWarning::runtimeAcknowledgementFailed,
        "unexpected acknowledgement failure did not become a warning"
    );
    require(runtime.snapshot(1).session->dirty(), "failed acknowledgement cleared runtime dirty state");
    const auto reopened = palmier::project::readProjectPackage(package.path(), generatedIds());
    require(
        reopened.project().timelines.front().tracks.front().clips.size() == 2,
        "acknowledgement failure hid a committed disk save"
    );
}

void cancellationPreservesDestinationAndCleansStaging() {
    for (const auto checkpoint : {
        ProjectPackageWriteCheckpoint::afterSnapshot,
        ProjectPackageWriteCheckpoint::afterSerialization,
        ProjectPackageWriteCheckpoint::afterStagingCreation,
        ProjectPackageWriteCheckpoint::afterWrite,
        ProjectPackageWriteCheckpoint::afterFlush,
        ProjectPackageWriteCheckpoint::beforeCommit,
    }) {
        TemporaryPackage package;
        const auto baseline = readText(package.path() / "project.json");
        ProjectRuntime runtime;
        installRuntime(runtime, package.path());
        static_cast<void>(runtime.splitClips(SplitClipsCommand{
            std::vector<SplitPoint>{{"target", 40}}, std::nullopt, std::nullopt,
        }));
        std::stop_source source;
        CancellingCheckpoint cancelling(checkpoint, source);
        requireWriteError(
            [&] {
                static_cast<void>(palmier::project::testing::writeProjectPackage(
                    runtime,
                    package.path(),
                    1,
                    source.get_token(),
                    &cancelling
                ));
            },
            "cancelled"
        );
        require(readText(package.path() / "project.json") == baseline, "cancellation replaced destination");
        require(runtime.snapshot(1).session->dirty(), "cancelled save cleared dirty state");
        package.requireNoStagingFiles();
    }

    TemporaryPackage package;
    ProjectRuntime runtime;
    installRuntime(runtime, package.path());
    static_cast<void>(runtime.splitClips(SplitClipsCommand{
        std::vector<SplitPoint>{{"target", 40}}, std::nullopt, std::nullopt,
    }));
    std::stop_source lateCancellation;
    CancellingCheckpoint afterCommit(ProjectPackageWriteCheckpoint::afterCommit, lateCancellation);
    const auto receipt = palmier::project::testing::writeProjectPackage(
        runtime,
        package.path(),
        1,
        lateCancellation.get_token(),
        &afterCommit
    );
    require(lateCancellation.stop_requested(), "postcommit checkpoint did not request cancellation");
    require(receipt.runtimeAcknowledged && !receipt.runtimeDirty, "postcommit cancellation hid success");
    package.requireNoStagingFiles();
}

void concurrentExternalReplacementIsExcluded() {
    TemporaryPackage package;
    ProjectRuntime runtime;
    installRuntime(runtime, package.path());
    static_cast<void>(runtime.splitClips(SplitClipsCommand{
        std::vector<SplitPoint>{{"target", 40}}, std::nullopt, std::nullopt,
    }));
    ExternalReplaceCheckpoint external(package.path() / "project.json");
    const auto receipt = palmier::project::testing::writeProjectPackage(
        runtime,
        package.path(),
        1,
        {},
        &external
    );
    require(
        !external.changed()
            && (external.nativeError() == ERROR_SHARING_VIOLATION
                || external.nativeError() == ERROR_ACCESS_DENIED),
        "destination replacement guard was bypassed"
    );
    require(receipt.runtimeAcknowledged && !receipt.runtimeDirty, "coordinated save did not commit");
    const auto reopened = palmier::project::readProjectPackage(package.path(), generatedIds());
    require(
        reopened.project().timelines.front().tracks.front().clips.size() == 2,
        "concurrent writer attempt corrupted the committed project"
    );
    package.requireNoStagingFiles();
}

void invalidAndBusyDestinationsFailBeforeMutation() {
    TemporaryPackage package;
    ProjectRuntime runtime;
    installRuntime(runtime, package.path());
    requireWriteError(
        [&] {
            static_cast<void>(palmier::project::writeProjectPackage(
                runtime,
                package.path().parent_path() / "wrong.extension",
                1
            ));
        },
        "invalidPackagePath"
    );

    const auto projectJson = package.path() / "project.json";
    const HANDLE exclusive = CreateFileW(
        projectJson.c_str(),
        GENERIC_READ,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    require(exclusive != INVALID_HANDLE_VALUE, "cannot lock destination fixture");
    requireWriteError(
        [&] {
            static_cast<void>(palmier::project::writeProjectPackage(runtime, package.path(), 1));
        },
        "projectJsonUnavailable"
    );
    CloseHandle(exclusive);
    package.requireNoStagingFiles();
}

void runTests(const std::filesystem::path& root) {
    editSaveRestartPreservesCanariesAndState(root);
    savingAnOlderSnapshotLeavesNewerRuntimeDirty();
    committedSaveReportsClosedRuntimeAsWarning();
    committedSaveConvertsUnexpectedAcknowledgementFailureToWarning();
    cancellationPreservesDestinationAndCleansStaging();
    concurrentExternalReplacementIsExcluded();
    invalidAndBusyDestinationsFailBeforeMutation();
}

}

int main() {
    std::exception_ptr failure;
    std::jthread worker([&failure] {
        try {
            runTests(std::filesystem::path(PALMIER_REPOSITORY_ROOT));
        } catch (...) {
            failure = std::current_exception();
        }
    });
    worker.join();
    if (failure) {
        try {
            std::rethrow_exception(failure);
        } catch (const std::exception& error) {
            std::cerr << "PALMIER_WINDOWS_PROJECT_PACKAGE_TESTS_FAILED " << error.what() << '\n';
            return 1;
        }
    }
    std::cout << "PALMIER_WINDOWS_PROJECT_PACKAGE_TESTS_OK\n";
    return 0;
}
