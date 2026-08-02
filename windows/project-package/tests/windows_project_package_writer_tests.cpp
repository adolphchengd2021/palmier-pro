#include "palmier/project/windows_project_package_writer.hpp"
#include "palmier/project/project_package_service.hpp"

#include "internal/windows_project_package_writer_testing.hpp"

#include "palmier/json/json_document.hpp"
#include "palmier/project/project_package_reader.hpp"

#define NOMINMAX
#include <Windows.h>

#include <algorithm>
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
using palmier::project::ClipMove;
using palmier::project::MoveClipsCommand;
using palmier::project::ProjectPackageWriteError;
using palmier::project::ProjectPackageWriteWarning;
using palmier::project::ProjectPackageServiceError;
using palmier::project::ProjectRuntime;
using palmier::project::RemoveClipsCommand;
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

class TemporaryDestination final {
public:
    TemporaryDestination() {
        std::random_device random;
        for (int attempt = 0; attempt < 32; ++attempt) {
            const auto candidate = std::filesystem::temp_directory_path()
                / ("palmier-save-as-tests-" + std::to_string(random())
                    + "-" + std::to_string(random()) + ".palmier");
            std::error_code error;
            if (!std::filesystem::exists(candidate, error) && !error) {
                path_ = candidate;
                return;
            }
        }
        throw std::runtime_error("cannot reserve unique Save As destination");
    }

    ~TemporaryDestination() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const noexcept { return path_; }

    void requireAbsentAndNoStaging() const {
        require(!std::filesystem::exists(path_), "Save As destination unexpectedly exists");
        requireNoStaging();
    }

    void requireNoStaging() const {
        const auto prefix = path_.filename().native() + L".palmier-";
        for (const auto& entry : std::filesystem::directory_iterator(path_.parent_path())) {
            const auto name = entry.path().filename().native();
            require(
                !name.starts_with(prefix) || !name.ends_with(L".partial"),
                "Save As left a staging package"
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

template<typename Operation>
void requireServiceError(Operation operation, const std::string& code) {
    try {
        operation();
    } catch (const ProjectPackageServiceError& error) {
        require(error.code == code, "unexpected package service error: " + error.code);
        return;
    }
    throw std::runtime_error("expected package service error: " + code);
}

std::wstring currentExecutablePath() {
    std::vector<wchar_t> buffer(32'768);
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    require(length > 0 && length < buffer.size(), "cannot resolve package test executable");
    return {buffer.data(), length};
}

std::wstring quoteProcessArgument(std::wstring_view value) {
    require(value.find(L'"') == std::wstring_view::npos, "test process argument contains a quote");
    return L"\"" + std::wstring(value) + L"\"";
}

class PackageLockChild final {
public:
    explicit PackageLockChild(const std::filesystem::path& packagePath) {
        std::random_device random;
        const auto suffix = std::to_wstring(GetCurrentProcessId())
            + L"-" + std::to_wstring(random()) + L"-" + std::to_wstring(random());
        readyName_ = L"Local\\PalmierPro.ProjectPackage.Tests.Ready." + suffix;
        releaseName_ = L"Local\\PalmierPro.ProjectPackage.Tests.Release." + suffix;
        ready_ = CreateEventW(nullptr, TRUE, FALSE, readyName_.c_str());
        release_ = CreateEventW(nullptr, TRUE, FALSE, releaseName_.c_str());
        if (ready_ == nullptr || release_ == nullptr) {
            cleanupHandles();
            throw std::runtime_error("cannot create package lock process events");
        }
        auto command = quoteProcessArgument(currentExecutablePath())
            + L" --hold-package-lock " + quoteProcessArgument(packagePath.native())
            + L" " + quoteProcessArgument(readyName_)
            + L" " + quoteProcessArgument(releaseName_);
        std::vector<wchar_t> commandBuffer(command.begin(), command.end());
        commandBuffer.push_back(L'\0');
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(
                nullptr,
                commandBuffer.data(),
                nullptr,
                nullptr,
                FALSE,
                CREATE_NO_WINDOW,
                nullptr,
                nullptr,
                &startup,
                &process
            )) {
            cleanupHandles();
            throw std::runtime_error("cannot start package lock child process");
        }
        process_ = process.hProcess;
        CloseHandle(process.hThread);
    }

    ~PackageLockChild() {
        if (release_ != nullptr) SetEvent(release_);
        if (process_ != nullptr) WaitForSingleObject(process_, 10'000);
        cleanupHandles();
    }

    void waitUntilReady() const {
        require(WaitForSingleObject(ready_, 10'000) == WAIT_OBJECT_0, "package lock child did not become ready");
    }

    void releaseAndRequireSuccess() {
        require(SetEvent(release_) != FALSE, "cannot release package lock child");
        require(WaitForSingleObject(process_, 10'000) == WAIT_OBJECT_0, "package lock child did not exit");
        DWORD exitCode{};
        require(GetExitCodeProcess(process_, &exitCode) != FALSE, "cannot read package lock child result");
        require(exitCode == 0, "package lock child failed");
    }

private:
    void cleanupHandles() noexcept {
        if (process_ != nullptr) CloseHandle(process_);
        if (ready_ != nullptr) CloseHandle(ready_);
        if (release_ != nullptr) CloseHandle(release_);
        process_ = nullptr;
        ready_ = nullptr;
        release_ = nullptr;
    }

    std::wstring readyName_;
    std::wstring releaseName_;
    HANDLE process_{};
    HANDLE ready_{};
    HANDLE release_{};
};

int holdPackageLock(
    const std::filesystem::path& packagePath,
    const std::wstring& readyName,
    const std::wstring& releaseName
) {
    const HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, readyName.c_str());
    const HANDLE release = OpenEventW(SYNCHRONIZE, FALSE, releaseName.c_str());
    if (ready == nullptr || release == nullptr) {
        if (ready != nullptr) CloseHandle(ready);
        if (release != nullptr) CloseHandle(release);
        return 2;
    }
    int result = 0;
    try {
        palmier::project::ProjectPackageService service;
        service.activate(service.prepareActivation(packagePath, 1));
        if (!SetEvent(ready) || WaitForSingleObject(release, 10'000) != WAIT_OBJECT_0) {
            result = 3;
        }
    } catch (...) {
        result = 4;
    }
    CloseHandle(ready);
    CloseHandle(release);
    return result;
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

class DestinationCreationCheckpoint final : public ProjectPackageWriteCheckpoints {
public:
    explicit DestinationCreationCheckpoint(std::filesystem::path destination)
        : destination_(std::move(destination)) {}

    void arrive(ProjectPackageWriteCheckpoint checkpoint) noexcept override {
        if (checkpoint != ProjectPackageWriteCheckpoint::beforeCommit || attempted_) return;
        attempted_ = true;
        try {
            std::filesystem::create_directory(destination_);
            writeText(destination_ / "canary.txt", "external-destination");
        } catch (...) {
            failed_ = true;
        }
    }

    bool succeeded() const noexcept { return attempted_ && !failed_; }

private:
    std::filesystem::path destination_;
    bool attempted_{};
    bool failed_{};
};

class SourceMutationCheckpoint final : public ProjectPackageWriteCheckpoints {
public:
    explicit SourceMutationCheckpoint(std::filesystem::path sourceFile)
        : sourceFile_(std::move(sourceFile)) {}

    void arrive(ProjectPackageWriteCheckpoint checkpoint) noexcept override {
        if (checkpoint != ProjectPackageWriteCheckpoint::afterSnapshot || attempted_) return;
        attempted_ = true;
        try {
            writeText(sourceFile_, "externally-replaced-with-longer-content");
        } catch (...) {
            failed_ = true;
        }
    }

    bool succeeded() const noexcept { return attempted_ && !failed_; }

private:
    std::filesystem::path sourceFile_;
    bool attempted_{};
    bool failed_{};
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
    const auto splitTimeline = runtime.getTimeline({}, 11).timeline;
    const auto& splitClips = splitTimeline.find("tracks")->array().front()
        .find("clips")->array();
    const auto movedClip = std::find_if(
        splitClips.begin(),
        splitClips.end(),
        [](const Value& clip) {
            return clip.find("id")->string().starts_with("writer-generated-");
        }
    );
    require(movedClip != splitClips.end(), "split did not create a movable right clip");
    const auto movedClipId = movedClip->find("id")->string();
    const auto move = runtime.moveClips(
        MoveClipsCommand{{ClipMove{
            movedClipId,
            std::nullopt,
            std::int64_t{200},
        }}},
        11
    );
    require(move.command.changed && move.session->revision == 2, "move did not update runtime");
    const auto receipt = palmier::project::writeProjectPackage(runtime, package.path(), 11);
    require(receipt.projectGeneration == 11, "write generation");
    require(receipt.revision == 2 && receipt.stateId == move.session->stateId, "write identity");
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
        if (id.starts_with("writer-generated-")) foundSecond = start == 200 && duration == 30;
    }
    require(foundFirst && foundSecond, "restart changed split IDs or frame timing");
    require(!reopened.snapshot(12).session->dirty(), "reopened project must start persisted");
    try {
        static_cast<void>(reopened.undo(12));
        throw std::runtime_error("restart unexpectedly retained an undo action");
    } catch (const CommandError& error) {
        require(error.code == "nothingToUndo", "restart returned the wrong undo boundary");
    }

    const auto removed = reopened.removeClips(RemoveClipsCommand{{movedClipId}}, 12);
    require(removed.command.changed && removed.session->revision == 1, "remove did not update runtime");
    const auto removeReceipt = palmier::project::writeProjectPackage(reopened, package.path(), 12);
    require(
        removeReceipt.runtimeAcknowledged && !removeReceipt.runtimeDirty,
        "remove save did not clear exact dirty state"
    );
    reopened.close();
    requireDeclaredCanaries(root, package.path());
    ProjectRuntime removedReopened;
    installRuntime(removedReopened, package.path(), 13);
    const auto removedTimeline = removedReopened.getTimeline({}, 13).timeline;
    const auto& remainingClips = removedTimeline.find("tracks")->array().front()
        .find("clips")->array();
    require(
        std::none_of(
            remainingClips.begin(),
            remainingClips.end(),
            [&](const Value& clip) { return clip.find("id")->string() == movedClipId; }
        ),
        "restart restored a removed clip"
    );
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

void saveAsPreservesPackageAndAdoptsCommittedIdentity(
    const std::filesystem::path& repositoryRoot
) {
    TemporaryPackage source(
        repositoryRoot / "fixtures/contracts/projects/unknown-fields.palmier"
    );
    TemporaryDestination destination;
    std::filesystem::create_directories(source.path() / "Media" / "nested");
    std::filesystem::create_directories(source.path() / "UnknownEmpty");
    writeText(source.path() / "Media" / "nested" / "clip.bin", "media-payload");
    writeText(source.path() / "extension.data", "unknown-payload");
    const auto sourceProject = readText(source.path() / "project.json");

    ProjectRuntime runtime;
    installRuntime(runtime, source.path(), 9);
    static_cast<void>(runtime.splitClips(SplitClipsCommand{
        std::vector<SplitPoint>{{"clip-save-target", 90}}, std::nullopt, std::nullopt,
    }));
    palmier::project::ProjectPackageService service;
    auto activation = service.prepareActivation(source.path(), 9);
    service.activate(std::move(activation));
    const auto initialIdentity = service.identity();
    require(initialIdentity.has_value(), "source identity was not activated");
    require(initialIdentity->path == std::filesystem::weakly_canonical(source.path()), "source identity was not normalized");

    const auto result = service.saveAs(runtime, destination.path(), 9);
    require(result.write.runtimeAcknowledged, "Save As was not acknowledged");
    require(!result.write.runtimeDirty, "Save As left the committed state dirty");
    require(result.identityAdopted && result.identity.has_value(), "Save As identity was not adopted");
    require(
        result.identity->path == std::filesystem::weakly_canonical(destination.path()),
        "Save As adopted the wrong destination"
    );
    require(result.package.copiedFileCount == 3, "Save As copied the wrong file count");
    require(
        result.package.copiedBytes
            == 28 + std::filesystem::file_size(source.path() / "media.json"),
        "Save As copied the wrong byte count"
    );
    require(readText(source.path() / "project.json") == sourceProject, "Save As changed source project.json");
    require(readText(destination.path() / "extension.data") == "unknown-payload", "Save As lost an unknown file");
    require(readText(destination.path() / "Media" / "nested" / "clip.bin") == "media-payload", "Save As changed media bytes");
    require(std::filesystem::is_directory(destination.path() / "UnknownEmpty"), "Save As lost an empty directory");
    const auto sourceDocument = palmier::project::readProjectPackage(source.path(), generatedIds());
    const auto destinationDocument = palmier::project::readProjectPackage(destination.path(), generatedIds());
    require(sourceDocument.project().timelines.front().tracks.front().clips.size() == 2, "Save As mutated the source package");
    require(destinationDocument.project().timelines.front().tracks.front().clips.size() == 3, "Save As omitted runtime edits");
    requireDeclaredCanaries(repositoryRoot, destination.path());
    const auto currentIdentity = service.identity();
    require(currentIdentity && currentIdentity->serial > initialIdentity->serial, "Save As identity serial did not advance");
}

void saveAsRefusesExistingAndNestedDestinations() {
    TemporaryPackage source;
    ProjectRuntime runtime;
    installRuntime(runtime, source.path(), 4);
    palmier::project::ProjectPackageService service;
    service.activate(service.prepareActivation(source.path(), 4));

    TemporaryPackage existing;
    const auto existingProject = readText(existing.path() / "project.json");
    requireWriteError(
        [&] { static_cast<void>(service.saveAs(runtime, existing.path(), 4)); },
        "destinationExists"
    );
    require(readText(existing.path() / "project.json") == existingProject, "existing destination was changed");

    const auto nested = source.path() / "nested.palmier";
    requireWriteError(
        [&] { static_cast<void>(service.saveAs(runtime, nested, 4)); },
        "invalidPackageRelationship"
    );
    require(!std::filesystem::exists(nested), "nested destination was created");
    const auto identity = service.identity();
    require(identity && identity->path == std::filesystem::weakly_canonical(source.path()), "failed Save As changed identity");
}

void saveAsCancellationPreservesSourceAndCleansStaging() {
    for (const auto checkpoint : {
        ProjectPackageWriteCheckpoint::afterSnapshot,
        ProjectPackageWriteCheckpoint::afterSerialization,
        ProjectPackageWriteCheckpoint::afterStagingCreation,
        ProjectPackageWriteCheckpoint::afterWrite,
        ProjectPackageWriteCheckpoint::afterFlush,
        ProjectPackageWriteCheckpoint::beforeCommit,
    }) {
        TemporaryPackage source;
        TemporaryDestination destination;
        const auto baseline = readText(source.path() / "project.json");
        ProjectRuntime runtime;
        installRuntime(runtime, source.path(), 12);
        static_cast<void>(runtime.splitClips(SplitClipsCommand{
            std::vector<SplitPoint>{{"target", 40}}, std::nullopt, std::nullopt,
        }));
        std::stop_source cancellation;
        CancellingCheckpoint checkpointHook(checkpoint, cancellation);
        requireWriteError(
            [&] {
                static_cast<void>(palmier::project::testing::writeProjectPackageAs(
                    runtime,
                    source.path(),
                    destination.path(),
                    12,
                    cancellation.get_token(),
                    &checkpointHook
                ));
            },
            "cancelled"
        );
        require(readText(source.path() / "project.json") == baseline, "cancelled Save As changed source");
        require(runtime.snapshot(12).session->dirty(), "cancelled Save As cleared dirty state");
        destination.requireAbsentAndNoStaging();
    }
}

void saveAsRefusesCommitRaceAndSourceMutation() {
    {
        TemporaryPackage source;
        TemporaryDestination destination;
        ProjectRuntime runtime;
        installRuntime(runtime, source.path(), 16);
        DestinationCreationCheckpoint race(destination.path());
        requireWriteError(
            [&] {
                static_cast<void>(palmier::project::testing::writeProjectPackageAs(
                    runtime,
                    source.path(),
                    destination.path(),
                    16,
                    {},
                    &race
                ));
            },
            "destinationExists"
        );
        require(race.succeeded(), "destination race hook failed");
        require(readText(destination.path() / "canary.txt") == "external-destination", "Save As overwrote raced destination");
        destination.requireNoStaging();
    }
    {
        TemporaryPackage source;
        TemporaryDestination destination;
        writeText(source.path() / "unknown.bin", "initial");
        ProjectRuntime runtime;
        installRuntime(runtime, source.path(), 17);
        SourceMutationCheckpoint mutation(source.path() / "unknown.bin");
        requireWriteError(
            [&] {
                static_cast<void>(palmier::project::testing::writeProjectPackageAs(
                    runtime,
                    source.path(),
                    destination.path(),
                    17,
                    {},
                    &mutation
                ));
            },
            "sourceChanged"
        );
        require(mutation.succeeded(), "source mutation hook failed");
        destination.requireAbsentAndNoStaging();
    }
}

void saveAsAdoptsTargetAndLeavesNewerEditDirty() {
    TemporaryPackage source;
    TemporaryDestination destination;
    ProjectRuntime runtime;
    installRuntime(runtime, source.path(), 14);
    palmier::project::ProjectPackageService service(
        [](ProjectRuntime& targetRuntime,
           const std::filesystem::path& sourcePath,
           const std::filesystem::path& destinationPath,
           std::optional<std::uint64_t> generation,
           std::stop_token cancellation) {
            auto receipt = palmier::project::writeProjectPackageAs(
                targetRuntime,
                sourcePath,
                destinationPath,
                generation,
                cancellation
            );
            static_cast<void>(targetRuntime.splitClips(SplitClipsCommand{
                std::vector<SplitPoint>{{"target", 40}}, std::nullopt, std::nullopt,
            }));
            return receipt;
        }
    );
    service.activate(service.prepareActivation(source.path(), 14));

    const auto result = service.saveAs(runtime, destination.path(), 14);
    require(result.identityAdopted && result.write.runtimeAcknowledged, "Save As did not adopt target");
    require(result.write.runtimeDirty, "newer edit was incorrectly marked persisted");
    require(runtime.snapshot(14).session->dirty(), "newer runtime state is not dirty");
    const auto firstDestination = palmier::project::readProjectPackage(destination.path(), generatedIds());
    require(firstDestination.project().timelines.front().tracks.front().clips.size() == 1, "Save As snapshot included a newer edit");

    const auto save = service.save(runtime, 14);
    require(save.runtimeAcknowledged && !save.runtimeDirty, "follow-up Save did not persist target");
    const auto secondDestination = palmier::project::readProjectPackage(destination.path(), generatedIds());
    require(secondDestination.project().timelines.front().tracks.front().clips.size() == 2, "follow-up Save did not write adopted target");
    const auto original = palmier::project::readProjectPackage(source.path(), generatedIds());
    require(original.project().timelines.front().tracks.front().clips.size() == 1, "follow-up Save wrote the old source");
}

void saveAsIdentityChangeAfterCommitKeepsRuntimeDirty() {
    TemporaryPackage source;
    TemporaryDestination destination;
    ProjectRuntime runtime;
    installRuntime(runtime, source.path(), 15);
    static_cast<void>(runtime.splitClips(SplitClipsCommand{
        std::vector<SplitPoint>{{"target", 40}}, std::nullopt, std::nullopt,
    }));
    palmier::project::ProjectPackageService* servicePointer{};
    palmier::project::ProjectPackageService service(
        [&](ProjectRuntime& targetRuntime,
            const std::filesystem::path& sourcePath,
            const std::filesystem::path& destinationPath,
            std::optional<std::uint64_t> generation,
            std::stop_token cancellation) {
            auto receipt = palmier::project::writeProjectPackageAs(
                targetRuntime,
                sourcePath,
                destinationPath,
                generation,
                cancellation
            );
            servicePointer->close();
            return receipt;
        }
    );
    servicePointer = &service;
    service.activate(service.prepareActivation(source.path(), 15));

    const auto result = service.saveAs(runtime, destination.path(), 15);
    require(!result.identityAdopted, "closed package service adopted a target");
    require(!result.write.runtimeAcknowledged && result.write.runtimeDirty, "identity change cleared dirty state");
    require(!service.identity().has_value(), "closed package service restored an identity");
    require(runtime.snapshot(15).session->dirty(), "identity change marked runtime persisted");
    const auto committedCopy = palmier::project::readProjectPackage(destination.path(), generatedIds());
    require(committedCopy.project().timelines.front().tracks.front().clips.size() == 2, "committed copy is incomplete");
    const auto original = palmier::project::readProjectPackage(source.path(), generatedIds());
    require(original.project().timelines.front().tracks.front().clips.size() == 1, "identity race changed source");
}

void projectPackageLeaseRefusesAnotherProcessAndReleasesOnExit() {
    TemporaryPackage package;
    PackageLockChild child(package.path());
    child.waitUntilReady();
    palmier::project::ProjectPackageService service;
    requireServiceError(
        [&] { static_cast<void>(service.prepareActivation(package.path(), 1)); },
        "projectPackageBusy"
    );
    child.releaseAndRequireSuccess();
    auto activation = service.prepareActivation(package.path(), 1);
    require(activation.valid(), "package lease was not released after process exit");
}

void projectPackageLeaseRefusesAnotherServiceInProcess() {
    TemporaryPackage package;
    palmier::project::ProjectPackageService first;
    first.activate(first.prepareActivation(package.path(), 1));
    auto reload = first.prepareActivation(package.path(), 2);
    require(reload.valid(), "active service could not prepare its own package reload");

    palmier::project::ProjectPackageService second;
    requireServiceError(
        [&] { static_cast<void>(second.prepareActivation(package.path(), 1)); },
        "projectPackageBusy"
    );
    reload = {};
    first.close();
    auto activation = second.prepareActivation(package.path(), 1);
    require(activation.valid(), "same-process package lease was not released");
}

void runTests(const std::filesystem::path& root) {
    editSaveRestartPreservesCanariesAndState(root);
    savingAnOlderSnapshotLeavesNewerRuntimeDirty();
    committedSaveReportsClosedRuntimeAsWarning();
    committedSaveConvertsUnexpectedAcknowledgementFailureToWarning();
    cancellationPreservesDestinationAndCleansStaging();
    concurrentExternalReplacementIsExcluded();
    invalidAndBusyDestinationsFailBeforeMutation();
    saveAsPreservesPackageAndAdoptsCommittedIdentity(root);
    saveAsRefusesExistingAndNestedDestinations();
    saveAsCancellationPreservesSourceAndCleansStaging();
    saveAsRefusesCommitRaceAndSourceMutation();
    saveAsAdoptsTargetAndLeavesNewerEditDirty();
    saveAsIdentityChangeAfterCommitKeepsRuntimeDirty();
    projectPackageLeaseRefusesAnotherProcessAndReleasesOnExit();
    projectPackageLeaseRefusesAnotherServiceInProcess();
}

}

int wmain(int argc, wchar_t* argv[]) {
    if (argc == 5 && std::wstring_view(argv[1]) == L"--hold-package-lock") {
        int result = 5;
        std::jthread worker([&] {
            result = holdPackageLock(argv[2], argv[3], argv[4]);
        });
        worker.join();
        return result;
    }
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
