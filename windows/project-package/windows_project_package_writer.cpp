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
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <exception>
#include <limits>
#include <mutex>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

namespace palmier::project {
namespace {

std::atomic<std::uint64_t> stagingSerial{1};
constexpr std::size_t verificationChunkBytes = 64U * 1024U;
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

std::filesystem::path normalizeExistingPackageForCopy(
    const std::filesystem::path& packagePath
) {
    if (!hasPalmierExtension(packagePath)) {
        fail("invalidPackagePath", "validateSource", "source project package must end in .palmier");
    }
    std::error_code error;
    auto requested = std::filesystem::absolute(packagePath, error).lexically_normal();
    if (error) {
        fail("invalidPackagePath", "validateSource", "source project package cannot be resolved");
    }
    const DWORD requestedAttributes = GetFileAttributesW(requested.c_str());
    if (
        requestedAttributes == INVALID_FILE_ATTRIBUTES
        || (requestedAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0
    ) {
        fail(
            "unsupportedPackageEntry",
            "validateSource",
            "source project package cannot be a reparse point",
            requestedAttributes == INVALID_FILE_ATTRIBUTES
                ? GetLastError()
                : ERROR_REPARSE_TAG_INVALID
        );
    }
    auto path = std::filesystem::weakly_canonical(requested, error);
    if (error || !std::filesystem::is_directory(path, error) || error) {
        fail(
            "invalidPackagePath",
            "validateSource",
            "source project package is not an existing directory",
            static_cast<DWORD>(error.value())
        );
    }
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (
        attributes == INVALID_FILE_ATTRIBUTES
        || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0
    ) {
        fail(
            "unsupportedPackageEntry",
            "validateSource",
            "source project package cannot be a reparse point",
            attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_REPARSE_TAG_INVALID
        );
    }
    return path;
}

std::filesystem::path normalizeNewPackageForCopy(
    const std::filesystem::path& packagePath
) {
    if (!hasPalmierExtension(packagePath)) {
        fail("invalidPackagePath", "validateDestination", "Save As destination must end in .palmier");
    }
    std::error_code error;
    auto absolute = std::filesystem::absolute(packagePath, error).lexically_normal();
    if (error || absolute.filename().empty()) {
        fail("invalidPackagePath", "validateDestination", "Save As destination cannot be resolved");
    }
    const DWORD requestedParentAttributes = GetFileAttributesW(absolute.parent_path().c_str());
    if (
        requestedParentAttributes == INVALID_FILE_ATTRIBUTES
        || (requestedParentAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0
    ) {
        fail(
            "unsupportedPackageEntry",
            "validateDestination",
            "Save As destination parent cannot be a reparse point",
            requestedParentAttributes == INVALID_FILE_ATTRIBUTES
                ? GetLastError()
                : ERROR_REPARSE_TAG_INVALID
        );
    }
    auto parent = std::filesystem::weakly_canonical(absolute.parent_path(), error);
    if (error || !std::filesystem::is_directory(parent, error) || error) {
        fail(
            "invalidPackagePath",
            "validateDestination",
            "Save As destination parent is not an existing directory",
            static_cast<DWORD>(error.value())
        );
    }
    auto destination = parent / absolute.filename();
    if (std::filesystem::exists(destination, error) || error) {
        fail(
            "destinationExists",
            "validateDestination",
            "Save As destination already exists",
            static_cast<DWORD>(error.value())
        );
    }
    return destination;
}

std::wstring foldPathComponent(std::wstring value) {
    if (value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        fail("invalidPackagePath", "validateDestination", "project package path is too long");
    }
    if (!value.empty()) {
        const int converted = LCMapStringEx(
            LOCALE_NAME_INVARIANT,
            LCMAP_LOWERCASE,
            value.data(),
            static_cast<int>(value.size()),
            value.data(),
            static_cast<int>(value.size()),
            nullptr,
            nullptr,
            0
        );
        if (converted != static_cast<int>(value.size())) {
            fail(
                "invalidPackagePath",
                "validateDestination",
                "project package path cannot be case-normalized",
                GetLastError()
            );
        }
    }
    return value;
}

bool pathContains(
    const std::filesystem::path& parent,
    const std::filesystem::path& candidate
) {
    auto parentPart = parent.begin();
    auto candidatePart = candidate.begin();
    for (; parentPart != parent.end(); ++parentPart, ++candidatePart) {
        if (
            candidatePart == candidate.end()
            || foldPathComponent(parentPart->native()) != foldPathComponent(candidatePart->native())
        ) {
            return false;
        }
    }
    return true;
}

void validateCopyRelationship(
    const std::filesystem::path& source,
    const std::filesystem::path& destination
) {
    if (pathContains(source, destination) || pathContains(destination, source)) {
        fail(
            "invalidPackageRelationship",
            "validateDestination",
            "Save As source and destination cannot contain each other"
        );
    }
}

struct PackageEntry final {
    std::filesystem::path relativePath;
    bool directory;
    std::uintmax_t bytes;
    std::filesystem::file_time_type lastWriteTime;
};

std::vector<PackageEntry> inventoryPackage(
    const std::filesystem::path& packagePath,
    std::stop_token cancellation
) {
    std::vector<PackageEntry> entries;
    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(packagePath, error);
    const std::filesystem::recursive_directory_iterator end;
    if (error) {
        fail("sourceReadFailed", "inventorySource", "source package cannot be enumerated", static_cast<DWORD>(error.value()));
    }
    while (iterator != end) {
        checkCancellation(cancellation, "inventorySource");
        const auto path = iterator->path();
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            fail("sourceReadFailed", "inventorySource", "source package entry attributes cannot be read", GetLastError());
        }
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            fail("unsupportedPackageEntry", "inventorySource", "source package cannot contain reparse points");
        }
        const bool directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        const bool regularFile = directory ? false : iterator->is_regular_file(error);
        if (error) {
            fail("sourceReadFailed", "inventorySource", "source package entry type cannot be read", static_cast<DWORD>(error.value()));
        }
        if (!directory && !regularFile) {
            fail("unsupportedPackageEntry", "inventorySource", "source package contains an unsupported entry");
        }
        const auto relative = std::filesystem::relative(path, packagePath, error);
        if (error || relative.empty()) {
            fail("sourceReadFailed", "inventorySource", "source package entry cannot be relativized", static_cast<DWORD>(error.value()));
        }
        std::uintmax_t bytes{};
        if (!directory) {
            bytes = std::filesystem::file_size(path, error);
            if (error) {
                fail("sourceReadFailed", "inventorySource", "source package file size cannot be read", static_cast<DWORD>(error.value()));
            }
        }
        const auto writeTime = std::filesystem::last_write_time(path, error);
        if (error) {
            fail("sourceReadFailed", "inventorySource", "source package timestamp cannot be read", static_cast<DWORD>(error.value()));
        }
        entries.push_back({relative, directory, bytes, writeTime});
        iterator.increment(error);
        if (error) {
            fail("sourceReadFailed", "inventorySource", "source package enumeration failed", static_cast<DWORD>(error.value()));
        }
    }
    std::ranges::sort(entries, {}, [](const PackageEntry& entry) {
        return entry.relativePath.generic_wstring();
    });
    const auto projectJson = std::ranges::find_if(entries, [](const PackageEntry& entry) {
        return !entry.directory && _wcsicmp(entry.relativePath.c_str(), L"project.json") == 0;
    });
    if (projectJson == entries.end()) {
        fail("invalidPackagePath", "inventorySource", "source package does not contain project.json");
    }
    return entries;
}

bool sameInventory(
    const std::vector<PackageEntry>& lhs,
    const std::vector<PackageEntry>& rhs
) {
    if (lhs.size() != rhs.size()) return false;
    for (std::size_t index = 0; index < lhs.size(); ++index) {
        const auto& left = lhs[index];
        const auto& right = rhs[index];
        if (
            left.relativePath != right.relativePath
            || left.directory != right.directory
            || left.bytes != right.bytes
            || left.lastWriteTime != right.lastWriteTime
        ) {
            return false;
        }
    }
    return true;
}

void copyFileVerified(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    std::uintmax_t expectedBytes,
    std::stop_token cancellation
) {
    HandleOwner input(CreateFileW(
        source.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr
    ));
    if (!input.valid()) {
        fail("sourceReadFailed", "copyPackage", "source package file cannot be opened", GetLastError());
    }
    HandleOwner output(CreateFileW(
        destination.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr
    ));
    if (!output.valid()) {
        fail("stagingFailed", "copyPackage", "staging package file cannot be created", GetLastError());
    }
    std::array<std::byte, verificationChunkBytes> buffer{};
    std::uintmax_t copied{};
    while (true) {
        checkCancellation(cancellation, "copyPackage");
        DWORD read{};
        if (!ReadFile(input.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
            fail("sourceReadFailed", "copyPackage", "source package file read failed", GetLastError());
        }
        if (read == 0) break;
        DWORD offset{};
        while (offset < read) {
            DWORD written{};
            if (!WriteFile(output.get(), buffer.data() + offset, read - offset, &written, nullptr)) {
                fail("stagingFailed", "copyPackage", "staging package file write failed", GetLastError());
            }
            if (written == 0 || written > read - offset) {
                fail("stagingFailed", "copyPackage", "staging package file write made invalid progress");
            }
            offset += written;
            copied += written;
        }
    }
    if (copied != expectedBytes) {
        fail("sourceChanged", "copyPackage", "source package file changed while it was copied");
    }
    if (!FlushFileBuffers(output.get())) {
        fail("stagingFailed", "flushPackage", "staging package file could not be flushed", GetLastError());
    }
    LARGE_INTEGER origin{};
    if (!SetFilePointerEx(input.get(), origin, nullptr, FILE_BEGIN)
        || !SetFilePointerEx(output.get(), origin, nullptr, FILE_BEGIN)) {
        fail("verificationFailed", "verifyPackage", "copied package file cannot be rewound", GetLastError());
    }
    std::array<std::byte, verificationChunkBytes> comparison{};
    while (true) {
        checkCancellation(cancellation, "verifyPackage");
        DWORD sourceRead{};
        DWORD destinationRead{};
        if (!ReadFile(input.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &sourceRead, nullptr)
            || !ReadFile(output.get(), comparison.data(), static_cast<DWORD>(comparison.size()), &destinationRead, nullptr)) {
            fail("verificationFailed", "verifyPackage", "copied package file cannot be verified", GetLastError());
        }
        if (sourceRead != destinationRead
            || !std::equal(buffer.begin(), buffer.begin() + sourceRead, comparison.begin())) {
            fail("verificationFailed", "verifyPackage", "copied package file differs from its source");
        }
        if (sourceRead == 0) break;
    }
}

class StagingPackage final {
public:
    explicit StagingPackage(const std::filesystem::path& destination) {
        const auto base = destination.filename().native();
        for (std::uint32_t attempt = 0; attempt < 64; ++attempt) {
            const auto serial = stagingSerial.fetch_add(1, std::memory_order_relaxed);
            path_ = destination.parent_path() / (
                base + L".palmier-" + std::to_wstring(GetCurrentProcessId())
                + L"-" + std::to_wstring(serial) + L".partial"
            );
            std::error_code error;
            if (std::filesystem::create_directory(path_, error)) {
                directory_.reset(CreateFileW(
                    path_.c_str(),
                    DELETE | FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                    nullptr,
                    OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS,
                    nullptr
                ));
                if (!directory_.valid()) {
                    const auto nativeCode = GetLastError();
                    std::filesystem::remove(path_, error);
                    fail("stagingFailed", "createStagingPackage", "staging package cannot be opened", nativeCode);
                }
                return;
            }
            if (error && error.value() != ERROR_ALREADY_EXISTS && error.value() != ERROR_FILE_EXISTS) {
                fail("stagingFailed", "createStagingPackage", "staging package cannot be created", static_cast<DWORD>(error.value()));
            }
        }
        fail("stagingFailed", "createStagingPackage", "unique staging package limit exceeded");
    }

    ~StagingPackage() {
        if (installed_) return;
        directory_.reset();
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const noexcept { return path_; }

    void install(const std::filesystem::path& destination) {
        const auto destinationName = destination.native();
        const std::size_t nameBytes = destinationName.size() * sizeof(wchar_t);
        if (nameBytes > (std::numeric_limits<DWORD>::max)() - sizeof(FILE_RENAME_INFO)) {
            fail("installFailed", "installPackage", "Save As destination path exceeds the rename contract");
        }
        std::vector<std::byte> storage(sizeof(FILE_RENAME_INFO) + nameBytes);
        auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
        rename->Flags = FILE_RENAME_FLAG_POSIX_SEMANTICS;
        rename->RootDirectory = nullptr;
        rename->FileNameLength = static_cast<DWORD>(nameBytes);
        std::memcpy(rename->FileName, destinationName.data(), nameBytes);
        if (!SetFileInformationByHandle(
                directory_.get(),
                FileRenameInfoEx,
                rename,
                static_cast<DWORD>(storage.size())
            )) {
            fail("installFailed", "installPackage", "Save As package commit failed", GetLastError());
        }
        installed_ = true;
    }

    bool cleanup() noexcept {
        if (installed_) return true;
        directory_.reset();
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        cleanupCode_ = static_cast<DWORD>(error.value());
        return !error;
    }

    DWORD cleanupCode() const noexcept { return cleanupCode_; }

private:
    std::filesystem::path path_;
    HandleOwner directory_;
    bool installed_{};
    DWORD cleanupCode_{};
};

std::string serializeSnapshot(
    const ProjectRuntimeSaveSnapshotResult& saved,
    std::stop_token cancellation
) {
    std::string content;
    try {
        content = palmier::json::canonical(saved.snapshot.source, cancellation);
    } catch (const palmier::json::Error& error) {
        checkCancellation(cancellation, "serializeProject");
        fail("serializationFailed", "serializeProject", error.what());
    }
    if (content.size() > defaultMaximumProjectJsonBytes) {
        fail("projectJsonTooLarge", "serializeProject", "serialized project.json exceeds the supported size limit");
    }
    return content;
}

void writeNewFile(
    const std::filesystem::path& path,
    std::string_view content,
    std::stop_token cancellation
) {
    HandleOwner file(CreateFileW(
        path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr
    ));
    if (!file.valid()) {
        fail("stagingFailed", "writeProjectJson", "staging project.json cannot be created", GetLastError());
    }
    std::size_t offset{};
    while (offset < content.size()) {
        checkCancellation(cancellation, "writeProjectJson");
        const auto count = static_cast<DWORD>((std::min)(content.size() - offset, 1024U * 1024U));
        DWORD written{};
        if (!WriteFile(file.get(), content.data() + offset, count, &written, nullptr)) {
            fail("stagingFailed", "writeProjectJson", "staging project.json write failed", GetLastError());
        }
        if (written == 0 || written > count) {
            fail("stagingFailed", "writeProjectJson", "staging project.json write made invalid progress");
        }
        offset += written;
    }
    if (!FlushFileBuffers(file.get())) {
        fail("stagingFailed", "flushProjectJson", "staging project.json could not be flushed", GetLastError());
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.get(), &size)
        || size.QuadPart < 0
        || static_cast<std::uint64_t>(size.QuadPart)
            != static_cast<std::uint64_t>(content.size())) {
        fail("verificationFailed", "verifyProjectJson", "staging project.json size verification failed", GetLastError());
    }
    LARGE_INTEGER origin{};
    if (!SetFilePointerEx(file.get(), origin, nullptr, FILE_BEGIN)) {
        fail("verificationFailed", "verifyProjectJson", "staging project.json cannot be rewound", GetLastError());
    }
    std::array<std::byte, verificationChunkBytes> buffer{};
    offset = 0;
    while (offset < content.size()) {
        checkCancellation(cancellation, "verifyProjectJson");
        const auto requested = static_cast<DWORD>((std::min)(
            content.size() - offset,
            buffer.size()
        ));
        DWORD read{};
        if (!ReadFile(file.get(), buffer.data(), requested, &read, nullptr)) {
            fail("verificationFailed", "verifyProjectJson", "staging project.json cannot be read", GetLastError());
        }
        if (
            read != requested
            || std::memcmp(buffer.data(), content.data() + offset, read) != 0
        ) {
            fail("verificationFailed", "verifyProjectJson", "staging project.json content verification failed");
        }
        offset += read;
    }
}

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

ProjectPackageSaveAsReceipt testing::writeProjectPackageAs(
    ProjectRuntime& runtime,
    const std::filesystem::path& sourcePackagePath,
    const std::filesystem::path& destinationPackagePath,
    std::optional<std::uint64_t> expectedProjectGeneration,
    std::stop_token cancellation,
    ProjectPackageWriteCheckpoints* checkpoints
) {
    checkCancellation(cancellation, "beforeAdmission");
    WriterLease lease(cancellation);
    checkCancellation(cancellation, "afterAdmission");
    const auto source = normalizeExistingPackageForCopy(sourcePackagePath);
    const auto destination = normalizeNewPackageForCopy(destinationPackagePath);
    validateCopyRelationship(source, destination);
    HandleOwner sourceGuard(CreateFileW(
        source.c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        nullptr
    ));
    if (!sourceGuard.valid()) {
        fail("sourceReadFailed", "lockSource", "source package cannot be locked", GetLastError());
    }
    HandleOwner destinationParentGuard(CreateFileW(
        destination.parent_path().c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        nullptr
    ));
    if (!destinationParentGuard.valid()) {
        fail(
            "stagingFailed",
            "lockDestinationParent",
            "Save As destination parent cannot be locked",
            GetLastError()
        );
    }
    const auto initialInventory = inventoryPackage(source, cancellation);
    const auto saved = runtimeSaveSnapshot(runtime, expectedProjectGeneration, cancellation);
    arrive(checkpoints, ProjectPackageWriteCheckpoint::afterSnapshot);
    checkCancellation(cancellation, "afterSnapshot");
    const auto content = serializeSnapshot(saved, cancellation);
    arrive(checkpoints, ProjectPackageWriteCheckpoint::afterSerialization);
    checkCancellation(cancellation, "afterSerialization");
    StagingPackage staging(destination);
    std::size_t copiedFiles{};
    std::uintmax_t copiedBytes{};
    try {
        arrive(checkpoints, ProjectPackageWriteCheckpoint::afterStagingCreation);
        checkCancellation(cancellation, "afterStagingCreation");
        for (const auto& entry : initialInventory) {
            checkCancellation(cancellation, "copyPackage");
            const auto stagedPath = staging.path() / entry.relativePath;
            if (entry.directory) {
                std::error_code error;
                if (!std::filesystem::create_directory(stagedPath, error) && error) {
                    fail(
                        "stagingFailed",
                        "copyPackage",
                        "staging package directory cannot be created",
                        static_cast<DWORD>(error.value())
                    );
                }
                continue;
            }
            if (_wcsicmp(entry.relativePath.c_str(), L"project.json") == 0) continue;
            copyFileVerified(source / entry.relativePath, stagedPath, entry.bytes, cancellation);
            ++copiedFiles;
            copiedBytes += entry.bytes;
        }
        writeNewFile(staging.path() / L"project.json", content, cancellation);
        arrive(checkpoints, ProjectPackageWriteCheckpoint::afterWrite);
        checkCancellation(cancellation, "afterWrite");
        arrive(checkpoints, ProjectPackageWriteCheckpoint::afterFlush);
        checkCancellation(cancellation, "afterFlush");
        arrive(checkpoints, ProjectPackageWriteCheckpoint::beforeCommit);
        checkCancellation(cancellation, "validateCommit");
        const auto finalInventory = inventoryPackage(source, cancellation);
        if (!sameInventory(initialInventory, finalInventory)) {
            fail("sourceChanged", "validateCommit", "source package changed during Save As");
        }
        std::error_code destinationError;
        if (std::filesystem::exists(destination, destinationError) || destinationError) {
            fail(
                "destinationExists",
                "validateCommit",
                "Save As destination appeared before commit",
                static_cast<DWORD>(destinationError.value())
            );
        }
        staging.install(destination);
    } catch (...) {
        const auto failure = std::current_exception();
        if (!staging.cleanup()) {
            fail(
                "cleanupFailed",
                "cleanupStagingPackage",
                "Save As staging package cleanup failed",
                staging.cleanupCode()
            );
        }
        std::rethrow_exception(failure);
    }
    arrive(checkpoints, ProjectPackageWriteCheckpoint::afterCommit);
    return {
        saved.projectGeneration,
        saved.snapshot.revision,
        saved.snapshot.stateId,
        content.size(),
        copiedFiles,
        copiedBytes,
    };
}

ProjectPackageSaveAsReceipt writeProjectPackageAs(
    ProjectRuntime& runtime,
    const std::filesystem::path& sourcePackagePath,
    const std::filesystem::path& destinationPackagePath,
    std::optional<std::uint64_t> expectedProjectGeneration,
    std::stop_token cancellation
) {
    return testing::writeProjectPackageAs(
        runtime,
        sourcePackagePath,
        destinationPackagePath,
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
