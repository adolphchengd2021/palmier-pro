#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0A00
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include "palmier/project/windows_project_package_writer.hpp"

#include "internal/windows_project_package_writer_testing.hpp"

#include "palmier/project/project_package_reader.hpp"

#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <exception>
#include <limits>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

namespace palmier::project {
namespace {

std::atomic<std::uint64_t> stagingSerial{1};
std::condition_variable_any writerGateCondition;
std::mutex writerGateMutex;
bool writerActive{};

[[noreturn]] void fail(
    std::string code,
    std::string stage,
    std::string detail,
    DWORD nativeCode = ERROR_SUCCESS
) {
    throw ProjectPackageWriteError(
        std::move(code),
        std::move(stage),
        std::move(detail),
        static_cast<int>(nativeCode)
    );
}

void checkCancellation(std::stop_token cancellation, std::string_view stage) {
    if (cancellation.stop_requested()) {
        fail("cancelled", std::string(stage), "project package write was cancelled");
    }
}

void arrive(
    testing::ProjectPackageWriteCheckpoints* checkpoints,
    testing::ProjectPackageWriteCheckpoint checkpoint
) noexcept {
    if (checkpoints != nullptr) checkpoints->arrive(checkpoint);
}

class WriterLease final {
public:
    explicit WriterLease(std::stop_token cancellation) {
        std::unique_lock lock(writerGateMutex);
        const bool acquired = writerGateCondition.wait(
            lock,
            cancellation,
            [] { return !writerActive; }
        );
        if (!acquired) {
            fail(
                "cancelled",
                "waitForWriter",
                "project package write was cancelled while waiting for the writer"
            );
        }
        writerActive = true;
    }

    ~WriterLease() {
        {
            const std::lock_guard lock(writerGateMutex);
            writerActive = false;
        }
        writerGateCondition.notify_one();
    }

    WriterLease(const WriterLease&) = delete;
    WriterLease& operator=(const WriterLease&) = delete;
};

class HandleOwner final {
public:
    explicit HandleOwner(HANDLE value = INVALID_HANDLE_VALUE) noexcept : value_(value) {}

    ~HandleOwner() {
        if (valid()) CloseHandle(value_);
    }

    HandleOwner(const HandleOwner&) = delete;
    HandleOwner& operator=(const HandleOwner&) = delete;

    HANDLE get() const noexcept { return value_; }
    bool valid() const noexcept { return value_ != INVALID_HANDLE_VALUE; }

    void reset(HANDLE value = INVALID_HANDLE_VALUE) noexcept {
        if (valid()) CloseHandle(value_);
        value_ = value;
    }

private:
    HANDLE value_;
};

bool hasPalmierExtension(const std::filesystem::path& path) {
    constexpr std::wstring_view expected = L".palmier";
    const auto extension = path.extension().native();
    if (extension.size() != expected.size()) return false;
    for (std::size_t index = 0; index < extension.size(); ++index) {
        auto character = extension[index];
        if (character >= L'A' && character <= L'Z') character += L'a' - L'A';
        if (character != expected[index]) return false;
    }
    return true;
}

struct DestinationSnapshot final {
    DWORD volumeSerialNumber;
    DWORD fileIndexHigh;
    DWORD fileIndexLow;
    DWORD fileSizeHigh;
    DWORD fileSizeLow;
    FILETIME lastWriteTime;
};

DestinationSnapshot readDestinationSnapshot(HANDLE file) {
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(file, &information)) {
        fail(
            "projectJsonUnavailable",
            "validateDestination",
            "project.json identity cannot be read",
            GetLastError()
        );
    }
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        fail(
            "invalidPackagePath",
            "validateDestination",
            "project.json must be a regular file"
        );
    }
    return {
        information.dwVolumeSerialNumber,
        information.nFileIndexHigh,
        information.nFileIndexLow,
        information.nFileSizeHigh,
        information.nFileSizeLow,
        information.ftLastWriteTime,
    };
}

bool sameDestination(
    const DestinationSnapshot& lhs,
    const DestinationSnapshot& rhs
) noexcept {
    return lhs.volumeSerialNumber == rhs.volumeSerialNumber
        && lhs.fileIndexHigh == rhs.fileIndexHigh
        && lhs.fileIndexLow == rhs.fileIndexLow
        && lhs.fileSizeHigh == rhs.fileSizeHigh
        && lhs.fileSizeLow == rhs.fileSizeLow
        && CompareFileTime(&lhs.lastWriteTime, &rhs.lastWriteTime) == 0;
}

class DestinationGuard final {
public:
    explicit DestinationGuard(std::filesystem::path projectJson)
        : projectJson_(std::move(projectJson)) {
        availability_.reset(CreateFileW(
            projectJson_.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        ));
        if (!availability_.valid()) {
            fail(
                "projectJsonUnavailable",
                "validateDestination",
                "project.json cannot be opened for coordinated replacement",
                GetLastError()
            );
        }
        file_.reset(CreateFileW(
            projectJson_.c_str(),
            FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        ));
        if (!file_.valid()) {
            fail(
                "projectJsonUnavailable",
                "validateDestination",
                "project.json cannot be locked for coordinated replacement",
                GetLastError()
            );
        }
        snapshot_ = readDestinationSnapshot(file_.get());
    }

    DestinationGuard(const DestinationGuard&) = delete;
    DestinationGuard& operator=(const DestinationGuard&) = delete;

    const std::filesystem::path& path() const noexcept { return projectJson_; }
    const DestinationSnapshot& snapshot() const noexcept { return snapshot_; }
    DestinationSnapshot currentSnapshot() const {
        return readDestinationSnapshot(file_.get());
    }

private:
    std::filesystem::path projectJson_;
    HandleOwner availability_;
    HandleOwner file_;
    DestinationSnapshot snapshot_{};
};

DestinationGuard validateDestination(const std::filesystem::path& packagePath) {
    if (!hasPalmierExtension(packagePath)) {
        fail(
            "invalidPackagePath",
            "validateDestination",
            "project package path must end in .palmier"
        );
    }
    std::error_code absoluteError;
    auto absolutePackage = std::filesystem::absolute(packagePath, absoluteError);
    if (absoluteError) {
        fail(
            "invalidPackagePath",
            "validateDestination",
            "project package path cannot be resolved"
        );
    }
    absolutePackage = absolutePackage.lexically_normal();
    const DWORD packageAttributes = GetFileAttributesW(absolutePackage.c_str());
    if (
        packageAttributes == INVALID_FILE_ATTRIBUTES
        || (packageAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0
    ) {
        fail(
            "invalidPackagePath",
            "validateDestination",
            "project package path is not a writable directory",
            packageAttributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_DIRECTORY
        );
    }
    auto projectJson = absolutePackage / L"project.json";
    return DestinationGuard(std::move(projectJson));
}

class StagingFile final {
public:
    explicit StagingFile(const std::filesystem::path& destination) {
        const auto parent = destination.parent_path();
        const auto base = destination.filename().native();
        for (std::uint32_t attempt = 0; attempt < 64; ++attempt) {
            const auto serial = stagingSerial.fetch_add(1, std::memory_order_relaxed);
            path_ = parent / (
                base
                + L".palmier-"
                + std::to_wstring(GetCurrentProcessId())
                + L"-"
                + std::to_wstring(serial)
                + L".partial"
            );
            file_ = CreateFileW(
                path_.c_str(),
                GENERIC_READ | GENERIC_WRITE | DELETE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr,
                CREATE_NEW,
                FILE_ATTRIBUTE_NORMAL,
                nullptr
            );
            if (file_ != INVALID_HANDLE_VALUE) return;
            const DWORD error = GetLastError();
            if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) {
                fail(
                    "stagingFailed",
                    "createStaging",
                    "project.json staging file could not be created",
                    error
                );
            }
        }
        fail(
            "stagingFailed",
            "createStaging",
            "unique project.json staging file limit exceeded"
        );
    }

    ~StagingFile() {
        static_cast<void>(cleanup());
        if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
    }

    StagingFile(const StagingFile&) = delete;
    StagingFile& operator=(const StagingFile&) = delete;

    void write(std::string_view content, std::stop_token cancellation) {
        constexpr std::size_t chunkBytes = 1024U * 1024U;
        std::size_t offset{};
        while (offset < content.size()) {
            checkCancellation(cancellation, "writeStaging");
            const auto remaining = content.size() - offset;
            const auto requested = static_cast<DWORD>((std::min)(remaining, chunkBytes));
            DWORD written{};
            if (!WriteFile(file_, content.data() + offset, requested, &written, nullptr)) {
                fail(
                    "stagingFailed",
                    "writeStaging",
                    "project.json staging write failed",
                    GetLastError()
                );
            }
            if (written == 0 || written > requested) {
                fail(
                    "stagingFailed",
                    "writeStaging",
                    "project.json staging write made invalid progress"
                );
            }
            offset += written;
        }
    }

    void flush() {
        if (!FlushFileBuffers(file_)) {
            fail(
                "stagingFailed",
                "flushStaging",
                "project.json staging data could not be flushed",
                GetLastError()
            );
        }
    }

    void install(const std::filesystem::path& destination) {
        const auto destinationName = destination.native();
        constexpr auto maximumInfoBytes = (std::numeric_limits<DWORD>::max)();
        if (destinationName.size() > maximumInfoBytes / sizeof(wchar_t)) {
            fail(
                "installFailed",
                "installDestination",
                "project.json destination path exceeds the rename contract"
            );
        }
        const std::size_t nameBytes = destinationName.size() * sizeof(wchar_t);
        if (nameBytes > maximumInfoBytes - sizeof(FILE_RENAME_INFO)) {
            fail(
                "installFailed",
                "installDestination",
                "project.json destination path exceeds the rename contract"
            );
        }
        std::vector<std::byte> storage(sizeof(FILE_RENAME_INFO) + nameBytes);
        auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
        rename->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS
            | FILE_RENAME_FLAG_POSIX_SEMANTICS;
        rename->RootDirectory = nullptr;
        rename->FileNameLength = static_cast<DWORD>(nameBytes);
        std::memcpy(rename->FileName, destinationName.data(), nameBytes);
        if (!SetFileInformationByHandle(
                file_,
                FileRenameInfoEx,
                rename,
                static_cast<DWORD>(storage.size())
            )) {
            fail(
                "installFailed",
                "installDestination",
                "project.json atomic replacement failed",
                GetLastError()
            );
        }
        installed_ = true;
    }

    bool cleanup() noexcept {
        if (installed_ || removed_ || file_ == INVALID_HANDLE_VALUE) return true;
        FILE_DISPOSITION_INFO disposition{static_cast<BOOLEAN>(TRUE)};
        if (!SetFileInformationByHandle(
                file_,
                FileDispositionInfo,
                &disposition,
                static_cast<DWORD>(sizeof(disposition))
            )) {
            cleanupCode_ = GetLastError();
            return false;
        }
        removed_ = true;
        return true;
    }

    DWORD cleanupCode() const noexcept { return cleanupCode_; }

private:
    std::filesystem::path path_;
    HANDLE file_{INVALID_HANDLE_VALUE};
    bool installed_{};
    bool removed_{};
    DWORD cleanupCode_{};
};

ProjectRuntimeSaveSnapshotResult runtimeSaveSnapshot(
    ProjectRuntime& runtime,
    std::optional<std::uint64_t> expectedProjectGeneration,
    std::stop_token cancellation
) {
    try {
        return runtime.saveSnapshot(expectedProjectGeneration, cancellation);
    } catch (const ProjectRuntimeError& error) {
        fail(error.code, "snapshotRuntime", error.what());
    }
}

ProjectPackageWriteReceipt acknowledgePersistence(
    ProjectRuntime& runtime,
    const ProjectRuntimeSaveSnapshotResult& saved,
    std::size_t projectJsonBytes,
    testing::ProjectPackageWriteCheckpoints* checkpoints
) {
    try {
        if (checkpoints != nullptr && checkpoints->failRuntimeAcknowledgement()) {
            throw std::runtime_error("injected runtime acknowledgement failure");
        }
        const auto acknowledged = runtime.markPersisted(
            saved.snapshot.stateId,
            saved.projectGeneration
        );
        return {
            saved.projectGeneration,
            saved.snapshot.revision,
            saved.snapshot.stateId,
            projectJsonBytes,
            true,
            acknowledged.session->dirty(),
            ProjectPackageWriteWarning::none,
        };
    } catch (const ProjectRuntimeError& error) {
        auto warning = ProjectPackageWriteWarning::runtimeAcknowledgementFailed;
        if (error.code == "staleProjectGeneration" || error.code == "noActiveProject") {
            warning = ProjectPackageWriteWarning::runtimeReplacedAfterSave;
        } else if (error.code == "runtimeClosed") {
            warning = ProjectPackageWriteWarning::runtimeClosedAfterSave;
        }
        return {
            saved.projectGeneration,
            saved.snapshot.revision,
            saved.snapshot.stateId,
            projectJsonBytes,
            false,
            true,
            warning,
        };
    } catch (...) {
        return {
            saved.projectGeneration,
            saved.snapshot.revision,
            saved.snapshot.stateId,
            projectJsonBytes,
            false,
            true,
            ProjectPackageWriteWarning::runtimeAcknowledgementFailed,
        };
    }
}

}

ProjectPackageWriteError::ProjectPackageWriteError(
    std::string codeValue,
    std::string stageValue,
    std::string detail,
    int nativeCodeValue
) : std::runtime_error(std::move(detail)),
    code(std::move(codeValue)),
    stage(std::move(stageValue)),
    nativeCode(nativeCodeValue) {}

ProjectPackageWriteReceipt writeProjectPackage(
    ProjectRuntime& runtime,
    const std::filesystem::path& packagePath,
    std::optional<std::uint64_t> expectedProjectGeneration,
    std::stop_token cancellation
) {
    return testing::writeProjectPackage(
        runtime,
        packagePath,
        expectedProjectGeneration,
        cancellation,
        nullptr
    );
}

namespace testing {

ProjectPackageWriteReceipt writeProjectPackage(
    ProjectRuntime& runtime,
    const std::filesystem::path& packagePath,
    std::optional<std::uint64_t> expectedProjectGeneration,
    std::stop_token cancellation,
    ProjectPackageWriteCheckpoints* checkpoints
) {
    checkCancellation(cancellation, "beforeAdmission");
    WriterLease lease(cancellation);
    checkCancellation(cancellation, "afterAdmission");
    const auto destination = validateDestination(packagePath);
    const auto saved = runtimeSaveSnapshot(runtime, expectedProjectGeneration, cancellation);
    arrive(checkpoints, ProjectPackageWriteCheckpoint::afterSnapshot);
    checkCancellation(cancellation, "afterSnapshot");

    std::string content;
    try {
        content = palmier::json::canonical(saved.snapshot.source, cancellation);
    } catch (const palmier::json::Error& error) {
        checkCancellation(cancellation, "serializeProject");
        fail("serializationFailed", "serializeProject", error.what());
    }
    if (content.size() > defaultMaximumProjectJsonBytes) {
        fail(
            "projectJsonTooLarge",
            "serializeProject",
            "serialized project.json exceeds the supported size limit"
        );
    }
    arrive(checkpoints, ProjectPackageWriteCheckpoint::afterSerialization);
    checkCancellation(cancellation, "afterSerialization");

    StagingFile staging(destination.path());
    try {
        arrive(checkpoints, ProjectPackageWriteCheckpoint::afterStagingCreation);
        checkCancellation(cancellation, "afterStagingCreation");
        staging.write(content, cancellation);
        arrive(checkpoints, ProjectPackageWriteCheckpoint::afterWrite);
        checkCancellation(cancellation, "afterWrite");
        staging.flush();
        arrive(checkpoints, ProjectPackageWriteCheckpoint::afterFlush);
        checkCancellation(cancellation, "afterFlush");
        arrive(checkpoints, ProjectPackageWriteCheckpoint::beforeCommit);
        checkCancellation(cancellation, "beforeCommit");
        if (!sameDestination(
                destination.snapshot(),
                destination.currentSnapshot()
            )) {
            fail(
                "destinationChanged",
                "validateCommit",
                "project.json changed after the save snapshot was prepared"
            );
        }
        staging.install(destination.path());
    } catch (...) {
        const auto failure = std::current_exception();
        if (!staging.cleanup()) {
            fail(
                "cleanupFailed",
                "cleanupStaging",
                "project.json staging cleanup failed",
                staging.cleanupCode()
            );
        }
        std::rethrow_exception(failure);
    }

    arrive(checkpoints, ProjectPackageWriteCheckpoint::afterCommit);
    return acknowledgePersistence(runtime, saved, content.size(), checkpoints);
}

}

}
