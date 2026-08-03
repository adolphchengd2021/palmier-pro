#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0A00
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include "palmier/project/project_recovery_journal.hpp"

#include "internal/project_recovery_journal_testing.hpp"

#include "palmier/project/project_package_reader.hpp"

#define NOMINMAX
#include <Windows.h>
#include <bcrypt.h>
#include <ShlObj.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace palmier::project {
namespace {

constexpr std::size_t maximumJournalBytes = defaultMaximumProjectJsonBytes + 1024U * 1024U;
constexpr std::size_t ioChunkBytes = 1024U * 1024U;
constexpr std::uint64_t recoveryContractVersion = 1;
std::atomic<std::uint64_t> stagingSerial{1};
std::condition_variable_any recoveryGateCondition;
std::mutex recoveryGateMutex;
bool recoveryMutationActive{};

[[noreturn]] void fail(
    std::string code,
    std::string stage,
    std::string detail,
    int nativeCode = 0
) {
    throw ProjectRecoveryJournalError(
        std::move(code),
        std::move(stage),
        std::move(detail),
        nativeCode
    );
}

void checkCancellation(std::stop_token cancellation, std::string_view stage) {
    if (cancellation.stop_requested()) {
        fail("cancelled", std::string(stage), "project recovery operation was cancelled");
    }
}

void arrive(
    testing::ProjectRecoveryJournalCheckpoints* checkpoints,
    testing::ProjectRecoveryJournalCheckpoint checkpoint
) noexcept {
    if (checkpoints != nullptr) checkpoints->arrive(checkpoint);
}

class HandleOwner final {
public:
    explicit HandleOwner(HANDLE value = INVALID_HANDLE_VALUE) noexcept : value_(value) {}
    ~HandleOwner() { reset(); }

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

class RecoveryMutationLease final {
public:
    explicit RecoveryMutationLease(std::stop_token cancellation) {
        std::unique_lock lock(recoveryGateMutex);
        if (cancellation.stop_requested()) {
            fail("cancelled", "waitForRecoveryWriter", "recovery mutation was cancelled before admission");
        }
        const auto acquired = recoveryGateCondition.wait(
            lock,
            cancellation,
            [] { return !recoveryMutationActive; }
        );
        if (!acquired || cancellation.stop_requested()) {
            fail("cancelled", "waitForRecoveryWriter", "recovery mutation was cancelled while waiting");
        }
        recoveryMutationActive = true;
    }

    ~RecoveryMutationLease() {
        {
            const std::lock_guard lock(recoveryGateMutex);
            recoveryMutationActive = false;
        }
        recoveryGateCondition.notify_one();
    }

    RecoveryMutationLease(const RecoveryMutationLease&) = delete;
    RecoveryMutationLease& operator=(const RecoveryMutationLease&) = delete;
};

class AlgorithmOwner final {
public:
    ~AlgorithmOwner() {
        if (value_ != nullptr) BCryptCloseAlgorithmProvider(value_, 0);
    }

    BCRYPT_ALG_HANDLE* output() noexcept { return &value_; }
    BCRYPT_ALG_HANDLE get() const noexcept { return value_; }

private:
    BCRYPT_ALG_HANDLE value_{};
};

std::string sha256(const void* data, std::size_t size) {
    if (size > (std::numeric_limits<ULONG>::max)()) {
        fail("hashInputTooLarge", "hash", "SHA-256 input exceeds the Windows API limit");
    }
    AlgorithmOwner algorithm;
    auto status = BCryptOpenAlgorithmProvider(
        algorithm.output(),
        BCRYPT_SHA256_ALGORITHM,
        nullptr,
        0
    );
    if (!BCRYPT_SUCCESS(status)) {
        fail("hashUnavailable", "hash", "SHA-256 provider could not be opened", status);
    }
    std::array<UCHAR, 32> digest{};
    status = BCryptHash(
        algorithm.get(),
        nullptr,
        0,
        reinterpret_cast<PUCHAR>(const_cast<void*>(data)),
        static_cast<ULONG>(size),
        digest.data(),
        static_cast<ULONG>(digest.size())
    );
    if (!BCRYPT_SUCCESS(status)) {
        fail("hashFailed", "hash", "SHA-256 hashing failed", status);
    }
    constexpr std::array<char, 16> hexadecimal{
        '0', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
    };
    std::string result;
    result.reserve(digest.size() * 2);
    for (const auto byte : digest) {
        result.push_back(hexadecimal[byte >> 4U]);
        result.push_back(hexadecimal[byte & 0x0fU]);
    }
    return result;
}

std::string sha256(std::string_view source) {
    return sha256(source.data(), source.size());
}

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

std::filesystem::path normalizePackage(const std::filesystem::path& packagePath) {
    if (packagePath.empty() || !hasPalmierExtension(packagePath)) {
        fail("invalidPackagePath", "validatePackage", "project package path must end in .palmier");
    }
    std::error_code error;
    auto absolute = std::filesystem::absolute(packagePath, error);
    if (error) {
        fail("invalidPackagePath", "validatePackage", "project package path cannot be made absolute", error.value());
    }
    const DWORD attributes = GetFileAttributesW(absolute.c_str());
    if (
        attributes == INVALID_FILE_ATTRIBUTES
        || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0
        || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0
    ) {
        fail(
            "invalidPackagePath",
            "validatePackage",
            "project package must be an existing non-reparse directory",
            static_cast<int>(
                attributes == INVALID_FILE_ATTRIBUTES
                    ? GetLastError()
                    : ERROR_REPARSE_TAG_INVALID
            )
        );
    }
    auto normalized = std::filesystem::weakly_canonical(absolute, error);
    if (error) {
        fail("invalidPackagePath", "validatePackage", "project package path cannot be normalized", error.value());
    }
    return normalized;
}

std::filesystem::path defaultRecoveryRoot() {
    PWSTR raw{};
    const auto result = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &raw);
    std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owner(raw, CoTaskMemFree);
    if (FAILED(result) || raw == nullptr) {
        fail(
            "recoveryRootUnavailable",
            "resolveRecoveryRoot",
            "Local AppData could not be resolved",
            static_cast<int>(result)
        );
    }
    return std::filesystem::path(raw) / L"Palmier Pro" / L"Recovery" / L"v1";
}

std::filesystem::path ensureRecoveryRoot(const std::filesystem::path& configured) {
    auto root = configured.empty() ? defaultRecoveryRoot() : configured;
    std::error_code error;
    root = std::filesystem::absolute(root, error);
    if (error) {
        fail("recoveryRootUnavailable", "prepareRecoveryRoot", "recovery root cannot be made absolute", error.value());
    }
    std::filesystem::create_directories(root, error);
    if (error) {
        fail("recoveryRootUnavailable", "prepareRecoveryRoot", "recovery root cannot be created", error.value());
    }
    root = std::filesystem::weakly_canonical(root, error);
    if (error) {
        fail("recoveryRootUnavailable", "prepareRecoveryRoot", "recovery root cannot be normalized", error.value());
    }
    const DWORD attributes = GetFileAttributesW(root.c_str());
    if (
        attributes == INVALID_FILE_ATTRIBUTES
        || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0
        || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0
    ) {
        fail(
            "recoveryRootUnavailable",
            "prepareRecoveryRoot",
            "recovery root must be a non-reparse directory",
            static_cast<int>(
                attributes == INVALID_FILE_ATTRIBUTES
                    ? GetLastError()
                    : ERROR_REPARSE_TAG_INVALID
            )
        );
    }
    return root;
}

std::wstring foldPath(const std::filesystem::path& path) {
    const auto source = path.native();
    if (source.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        fail("invalidPackagePath", "keyPackage", "project package path is too long");
    }
    std::wstring result(source.size(), L'\0');
    if (!source.empty()) {
        const auto converted = LCMapStringEx(
            LOCALE_NAME_INVARIANT,
            LCMAP_LOWERCASE,
            source.data(),
            static_cast<int>(source.size()),
            result.data(),
            static_cast<int>(result.size()),
            nullptr,
            nullptr,
            0
        );
        if (converted != static_cast<int>(result.size())) {
            fail("invalidPackagePath", "keyPackage", "project package path cannot be case-folded", static_cast<int>(GetLastError()));
        }
    }
    return result;
}

std::string packageKey(const std::filesystem::path& normalizedPackage) {
    const auto folded = foldPath(normalizedPackage);
    return sha256(folded.data(), folded.size() * sizeof(wchar_t));
}

std::string utf8Path(const std::filesystem::path& path) {
    const auto source = path.native();
    if (source.empty()) return {};
    if (source.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        fail("invalidPackagePath", "encodePackagePath", "project package path is too long");
    }
    const auto count = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        source.data(),
        static_cast<int>(source.size()),
        nullptr,
        0,
        nullptr,
        nullptr
    );
    if (count <= 0) {
        fail("invalidPackagePath", "encodePackagePath", "project package path is not valid Unicode", static_cast<int>(GetLastError()));
    }
    std::string result(static_cast<std::size_t>(count), '\0');
    const auto converted = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        source.data(),
        static_cast<int>(source.size()),
        result.data(),
        count,
        nullptr,
        nullptr
    );
    if (converted != count) {
        fail("invalidPackagePath", "encodePackagePath", "project package path UTF-8 conversion failed", static_cast<int>(GetLastError()));
    }
    return result;
}

std::filesystem::path pathFromUtf8(std::string_view source) {
    if (source.empty() || source.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        fail("recoveryIdentityMismatch", "parseJournal", "recovery package path is invalid");
    }
    const auto count = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        source.data(),
        static_cast<int>(source.size()),
        nullptr,
        0
    );
    if (count <= 0) {
        fail(
            "recoveryIdentityMismatch",
            "parseJournal",
            "recovery package path is not valid UTF-8",
            static_cast<int>(GetLastError())
        );
    }
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    const auto converted = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        source.data(),
        static_cast<int>(source.size()),
        result.data(),
        count
    );
    if (converted != count) {
        fail(
            "recoveryIdentityMismatch",
            "parseJournal",
            "recovery package path conversion failed",
            static_cast<int>(GetLastError())
        );
    }
    return std::filesystem::path(std::move(result));
}

std::filesystem::path journalPath(
    const std::filesystem::path& root,
    const std::filesystem::path& normalizedPackage
) {
    auto name = std::filesystem::path(packageKey(normalizedPackage));
    name += L".palmier-recovery.json";
    return root / name;
}

struct FileSnapshot final {
    DWORD volumeSerialNumber{};
    DWORD fileIndexHigh{};
    DWORD fileIndexLow{};
    DWORD fileSizeHigh{};
    DWORD fileSizeLow{};
    FILETIME lastWriteTime{};
};

FileSnapshot fileSnapshot(HANDLE file, std::string_view stage) {
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(file, &information)) {
        fail("fileUnavailable", std::string(stage), "file identity cannot be read", static_cast<int>(GetLastError()));
    }
    if (
        (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0
        || (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0
    ) {
        fail("unsupportedRecoveryFile", std::string(stage), "file must be regular and non-reparse");
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

bool sameFileSnapshot(const FileSnapshot& lhs, const FileSnapshot& rhs) noexcept {
    return lhs.volumeSerialNumber == rhs.volumeSerialNumber
        && lhs.fileIndexHigh == rhs.fileIndexHigh
        && lhs.fileIndexLow == rhs.fileIndexLow
        && lhs.fileSizeHigh == rhs.fileSizeHigh
        && lhs.fileSizeLow == rhs.fileSizeLow
        && CompareFileTime(&lhs.lastWriteTime, &rhs.lastWriteTime) == 0;
}

std::string readHandle(
    HANDLE file,
    std::size_t maximumBytes,
    std::stop_token cancellation,
    std::string_view stage
) {
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0) {
        fail("fileUnavailable", std::string(stage), "file size cannot be read", static_cast<int>(GetLastError()));
    }
    if (static_cast<std::uint64_t>(size.QuadPart) > maximumBytes) {
        fail("recoveryFileTooLarge", std::string(stage), "file exceeds the recovery size limit");
    }
    LARGE_INTEGER origin{};
    if (!SetFilePointerEx(file, origin, nullptr, FILE_BEGIN)) {
        fail("fileUnavailable", std::string(stage), "file cannot be rewound", static_cast<int>(GetLastError()));
    }
    std::string result(static_cast<std::size_t>(size.QuadPart), '\0');
    std::size_t offset{};
    while (offset < result.size()) {
        checkCancellation(cancellation, stage);
        const auto requested = static_cast<DWORD>((std::min)(
            result.size() - offset,
            ioChunkBytes
        ));
        DWORD read{};
        if (!ReadFile(file, result.data() + offset, requested, &read, nullptr)) {
            fail("fileUnavailable", std::string(stage), "file read failed", static_cast<int>(GetLastError()));
        }
        if (read == 0 || read > requested) {
            fail("fileUnavailable", std::string(stage), "file read made invalid progress");
        }
        offset += read;
    }
    return result;
}

std::optional<std::string> readFile(
    const std::filesystem::path& path,
    std::size_t maximumBytes,
    bool missingAllowed,
    std::stop_token cancellation,
    std::string_view stage
) {
    checkCancellation(cancellation, stage);
    HandleOwner file(CreateFileW(
        path.c_str(),
        GENERIC_READ | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr
    ));
    if (!file.valid()) {
        const auto error = GetLastError();
        if (missingAllowed && (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)) {
            return std::nullopt;
        }
        fail("fileUnavailable", std::string(stage), "file cannot be opened", static_cast<int>(error));
    }
    const auto before = fileSnapshot(file.get(), stage);
    auto content = readHandle(file.get(), maximumBytes, cancellation, stage);
    const auto after = fileSnapshot(file.get(), stage);
    if (!sameFileSnapshot(before, after)) {
        fail("fileChanged", std::string(stage), "file changed while it was being read");
    }
    return content;
}

palmier::json::Value unsignedNumber(std::uint64_t value) {
    return palmier::json::Value(palmier::json::Number{std::to_string(value), {}});
}

std::uint64_t parseUnsigned(
    const palmier::json::Value& value,
    std::string_view field
) {
    if (value.kind() != palmier::json::Value::Kind::number) {
        fail("invalidRecoveryJournal", "parseJournal", std::string(field) + " must be an unsigned integer");
    }
    const auto& lexeme = value.number().lexeme;
    if (lexeme.empty() || lexeme.front() == '-' || (lexeme.size() > 1 && lexeme.front() == '0')) {
        fail("invalidRecoveryJournal", "parseJournal", std::string(field) + " is not a canonical unsigned integer");
    }
    std::uint64_t result{};
    const auto parsed = std::from_chars(lexeme.data(), lexeme.data() + lexeme.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != lexeme.data() + lexeme.size()) {
        fail("invalidRecoveryJournal", "parseJournal", std::string(field) + " is outside the supported range");
    }
    return result;
}

const palmier::json::Value& requiredField(
    const palmier::json::Object& object,
    std::string_view field
) {
    const auto found = object.find(std::string(field));
    if (found == object.end()) {
        fail("invalidRecoveryJournal", "parseJournal", "recovery journal is missing " + std::string(field));
    }
    return found->second;
}

bool isSha256(std::string_view value) noexcept {
    return value.size() == 64 && std::ranges::all_of(value, [](char character) {
        return (character >= '0' && character <= '9')
            || (character >= 'a' && character <= 'f');
    });
}

std::string requiredString(
    const palmier::json::Object& object,
    std::string_view field
) {
    const auto& value = requiredField(object, field);
    if (value.kind() != palmier::json::Value::Kind::string || value.string().empty()) {
        fail("invalidRecoveryJournal", "parseJournal", std::string(field) + " must be a non-empty string");
    }
    return value.string();
}

std::uint64_t nowUnixMilliseconds() {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    if (elapsed < 0) {
        fail("clockUnavailable", "serializeJournal", "system time precedes the Unix epoch");
    }
    return static_cast<std::uint64_t>(elapsed);
}

struct SerializedJournal final {
    std::string journal;
    std::size_t projectJsonBytes;
};

SerializedJournal serializeJournal(
    ProjectRuntimeSaveSnapshotResult saved,
    const std::filesystem::path& normalizedPackage,
    std::string baselineSha256,
    std::stop_token cancellation
) {
    std::string projectJson;
    try {
        projectJson = palmier::json::canonical(saved.snapshot.source, cancellation);
    } catch (const palmier::json::Error& error) {
        checkCancellation(cancellation, "serializeProject");
        fail("serializationFailed", "serializeProject", error.what());
    }
    if (projectJson.size() > defaultMaximumProjectJsonBytes) {
        fail("projectJsonTooLarge", "serializeProject", "serialized project exceeds the recovery size limit");
    }
    const auto projectJsonBytes = projectJson.size();
    auto projectJsonSha256 = sha256(projectJson);
    std::string{}.swap(projectJson);
    palmier::json::Object journal;
    journal.emplace("contractVersion", unsignedNumber(recoveryContractVersion));
    journal.emplace("packageKey", palmier::json::Value(packageKey(normalizedPackage)));
    journal.emplace("packagePath", palmier::json::Value(utf8Path(normalizedPackage)));
    journal.emplace("projectGeneration", unsignedNumber(saved.projectGeneration));
    journal.emplace("revision", unsignedNumber(saved.snapshot.revision));
    journal.emplace("stateId", unsignedNumber(saved.snapshot.stateId));
    journal.emplace("persistedStateId", unsignedNumber(saved.snapshot.persistedStateId));
    journal.emplace("createdUnixMilliseconds", unsignedNumber(nowUnixMilliseconds()));
    journal.emplace("baselineProjectJsonSha256", palmier::json::Value(std::move(baselineSha256)));
    journal.emplace("projectJsonSha256", palmier::json::Value(std::move(projectJsonSha256)));
    journal.emplace("project", std::move(saved.snapshot.source));
    std::string content;
    try {
        content = palmier::json::canonical(palmier::json::Value(std::move(journal)), cancellation);
    } catch (const palmier::json::Error& error) {
        checkCancellation(cancellation, "serializeJournal");
        fail("serializationFailed", "serializeJournal", error.what());
    }
    if (content.size() > maximumJournalBytes) {
        fail("recoveryFileTooLarge", "serializeJournal", "recovery journal exceeds the size limit");
    }
    return {std::move(content), projectJsonBytes};
}

ProjectRecoveryJournalCandidate parseJournal(
    const std::filesystem::path& path,
    std::string_view content,
    const std::filesystem::path& expectedPackage,
    std::stop_token cancellation
) {
    palmier::json::Value root;
    try {
        root = palmier::json::parse(content, cancellation);
    } catch (const palmier::json::Error& error) {
        checkCancellation(cancellation, "parseJournal");
        fail("invalidRecoveryJournal", "parseJournal", error.what());
    }
    if (root.kind() != palmier::json::Value::Kind::object) {
        fail("invalidRecoveryJournal", "parseJournal", "recovery journal root must be an object");
    }
    const auto& object = root.object();
    if (parseUnsigned(requiredField(object, "contractVersion"), "contractVersion") != recoveryContractVersion) {
        fail("unsupportedRecoveryJournal", "parseJournal", "recovery journal version is unsupported");
    }
    const auto expectedPackageKey = packageKey(expectedPackage);
    if (requiredString(object, "packageKey") != expectedPackageKey) {
        fail("recoveryIdentityMismatch", "parseJournal", "recovery journal package key does not match its target");
    }
    const auto packagePath = requiredString(object, "packagePath");
    try {
        const auto storedPackage = normalizePackage(pathFromUtf8(packagePath));
        if (foldPath(storedPackage) != foldPath(expectedPackage)) {
            fail("recoveryIdentityMismatch", "parseJournal", "recovery journal package path does not match its target");
        }
    } catch (const ProjectRecoveryJournalError& error) {
        if (error.code == "recoveryIdentityMismatch") throw;
        fail("recoveryIdentityMismatch", "parseJournal", "recovery journal package path is not a valid target");
    }
    const auto& project = requiredField(object, "project");
    if (project.kind() != palmier::json::Value::Kind::object) {
        fail("invalidRecoveryJournal", "parseJournal", "recovery project root must be an object");
    }
    std::string projectJson;
    try {
        projectJson = palmier::json::canonical(project, cancellation);
    } catch (const palmier::json::Error& error) {
        checkCancellation(cancellation, "parseJournal");
        fail("invalidRecoveryJournal", "parseJournal", error.what());
    }
    if (projectJson.size() > defaultMaximumProjectJsonBytes) {
        fail("projectJsonTooLarge", "parseJournal", "recovery project exceeds the size limit");
    }
    const auto projectSha = requiredString(object, "projectJsonSha256");
    const auto baselineSha = requiredString(object, "baselineProjectJsonSha256");
    if (!isSha256(projectSha) || !isSha256(baselineSha)) {
        fail("invalidRecoveryJournal", "parseJournal", "recovery SHA-256 values are malformed");
    }
    if (projectSha != sha256(projectJson)) {
        fail("recoveryPayloadMismatch", "parseJournal", "recovery project hash does not match its payload");
    }
    auto candidate = ProjectRecoveryJournalCandidate{
        path,
        packagePath,
        parseUnsigned(requiredField(object, "projectGeneration"), "projectGeneration"),
        parseUnsigned(requiredField(object, "revision"), "revision"),
        parseUnsigned(requiredField(object, "stateId"), "stateId"),
        parseUnsigned(requiredField(object, "persistedStateId"), "persistedStateId"),
        parseUnsigned(requiredField(object, "createdUnixMilliseconds"), "createdUnixMilliseconds"),
        baselineSha,
        projectSha,
        std::move(projectJson),
    };
    if (candidate.stateId == candidate.persistedStateId) {
        fail("invalidRecoveryJournal", "parseJournal", "recovery journal does not contain a dirty state");
    }
    return candidate;
}

class StagingJournal final {
public:
    explicit StagingJournal(const std::filesystem::path& destination) {
        const auto base = destination.filename().native();
        for (std::uint32_t attempt = 0; attempt < 64; ++attempt) {
            const auto serial = stagingSerial.fetch_add(1, std::memory_order_relaxed);
            path_ = destination.parent_path() / (
                base
                + L".palmier-"
                + std::to_wstring(GetCurrentProcessId())
                + L"-"
                + std::to_wstring(serial)
                + L".partial"
            );
            file_.reset(CreateFileW(
                path_.c_str(),
                GENERIC_READ | GENERIC_WRITE | DELETE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr,
                CREATE_NEW,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                nullptr
            ));
            if (file_.valid()) return;
            const auto error = GetLastError();
            if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) {
                fail("stagingFailed", "createStaging", "recovery staging file cannot be created", static_cast<int>(error));
            }
        }
        fail("stagingFailed", "createStaging", "unique recovery staging file limit exceeded");
    }

    ~StagingJournal() { static_cast<void>(cleanup()); }

    StagingJournal(const StagingJournal&) = delete;
    StagingJournal& operator=(const StagingJournal&) = delete;

    void write(std::string_view content, std::stop_token cancellation) {
        std::size_t offset{};
        while (offset < content.size()) {
            checkCancellation(cancellation, "writeStaging");
            const auto requested = static_cast<DWORD>((std::min)(
                content.size() - offset,
                ioChunkBytes
            ));
            DWORD written{};
            if (!WriteFile(file_.get(), content.data() + offset, requested, &written, nullptr)) {
                fail("stagingFailed", "writeStaging", "recovery staging write failed", static_cast<int>(GetLastError()));
            }
            if (written == 0 || written > requested) {
                fail("stagingFailed", "writeStaging", "recovery staging write made invalid progress");
            }
            offset += written;
        }
    }

    void flushAndVerify(std::string_view expected, std::stop_token cancellation) {
        if (!FlushFileBuffers(file_.get())) {
            fail("stagingFailed", "flushStaging", "recovery staging data could not be flushed", static_cast<int>(GetLastError()));
        }
        const auto actual = readHandle(
            file_.get(),
            maximumJournalBytes,
            cancellation,
            "verifyStaging"
        );
        if (actual != expected) {
            fail("verificationFailed", "verifyStaging", "recovery staging content verification failed");
        }
    }

    void install(const std::filesystem::path& destination) {
        const auto name = destination.native();
        constexpr auto maximumInfoBytes = (std::numeric_limits<DWORD>::max)();
        if (name.size() > maximumInfoBytes / sizeof(wchar_t)) {
            fail("installFailed", "installJournal", "recovery destination path exceeds the rename contract");
        }
        const auto nameBytes = name.size() * sizeof(wchar_t);
        if (nameBytes > maximumInfoBytes - sizeof(FILE_RENAME_INFO)) {
            fail("installFailed", "installJournal", "recovery destination path exceeds the rename contract");
        }
        std::vector<std::byte> storage(sizeof(FILE_RENAME_INFO) + nameBytes);
        auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
        rename->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
        rename->RootDirectory = nullptr;
        rename->FileNameLength = static_cast<DWORD>(nameBytes);
        std::memcpy(rename->FileName, name.data(), nameBytes);
        if (!SetFileInformationByHandle(
                file_.get(),
                FileRenameInfoEx,
                rename,
                static_cast<DWORD>(storage.size())
            )) {
            fail("installFailed", "installJournal", "recovery journal atomic replacement failed", static_cast<int>(GetLastError()));
        }
        installed_ = true;
    }

    bool cleanup() noexcept {
        if (installed_ || removed_ || !file_.valid()) return true;
        FILE_DISPOSITION_INFO disposition{static_cast<BOOLEAN>(TRUE)};
        if (!SetFileInformationByHandle(
                file_.get(),
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
    HandleOwner file_;
    bool installed_{};
    bool removed_{};
    DWORD cleanupCode_{};
};

ProjectRuntimeSaveSnapshotResult snapshotRuntime(
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

ProjectRecoveryJournalWriteReceipt writeImpl(
    const ProjectRecoveryJournal& journal,
    ProjectRuntime& runtime,
    const std::filesystem::path& packagePath,
    std::optional<std::uint64_t> expectedProjectGeneration,
    std::stop_token cancellation,
    testing::ProjectRecoveryJournalCheckpoints* checkpoints
) {
    checkCancellation(cancellation, "beforeSnapshot");
    RecoveryMutationLease lease(cancellation);
    checkCancellation(cancellation, "afterAdmission");
    const auto normalizedPackage = normalizePackage(packagePath);
    auto saved = snapshotRuntime(runtime, expectedProjectGeneration, cancellation);
    arrive(checkpoints, testing::ProjectRecoveryJournalCheckpoint::afterSnapshot);
    checkCancellation(cancellation, "afterSnapshot");
    if (!saved.snapshot.dirty()) {
        fail("projectClean", "validateSnapshot", "clean project state does not need a recovery journal");
    }
    const auto baseline = readFile(
        normalizedPackage / L"project.json",
        defaultMaximumProjectJsonBytes,
        false,
        cancellation,
        "readBaseline"
    );
    arrive(checkpoints, testing::ProjectRecoveryJournalCheckpoint::afterBaselineRead);
    checkCancellation(cancellation, "afterBaselineRead");
    const auto generation = saved.projectGeneration;
    const auto revision = saved.snapshot.revision;
    const auto stateId = saved.snapshot.stateId;
    auto serialized = serializeJournal(
        std::move(saved),
        normalizedPackage,
        sha256(*baseline),
        cancellation
    );
    arrive(checkpoints, testing::ProjectRecoveryJournalCheckpoint::afterSerialization);
    checkCancellation(cancellation, "afterSerialization");
    const auto root = ensureRecoveryRoot(journal.configuredRecoveryRoot());
    const auto destination = journalPath(root, normalizedPackage);
    StagingJournal staging(destination);
    try {
        arrive(checkpoints, testing::ProjectRecoveryJournalCheckpoint::afterStagingCreation);
        checkCancellation(cancellation, "afterStagingCreation");
        staging.write(serialized.journal, cancellation);
        arrive(checkpoints, testing::ProjectRecoveryJournalCheckpoint::afterWrite);
        checkCancellation(cancellation, "afterWrite");
        staging.flushAndVerify(serialized.journal, cancellation);
        arrive(checkpoints, testing::ProjectRecoveryJournalCheckpoint::afterFlush);
        checkCancellation(cancellation, "afterFlush");
        arrive(checkpoints, testing::ProjectRecoveryJournalCheckpoint::beforeCommit);
        checkCancellation(cancellation, "beforeCommit");
        staging.install(destination);
    } catch (...) {
        const auto failure = std::current_exception();
        if (!staging.cleanup()) {
            fail("cleanupFailed", "cleanupStaging", "recovery staging cleanup failed", static_cast<int>(staging.cleanupCode()));
        }
        std::rethrow_exception(failure);
    }
    arrive(checkpoints, testing::ProjectRecoveryJournalCheckpoint::afterCommit);
    return {
        destination,
        generation,
        revision,
        stateId,
        serialized.projectJsonBytes,
        serialized.journal.size(),
    };
}

}

ProjectRecoveryJournalError::ProjectRecoveryJournalError(
    std::string codeValue,
    std::string stageValue,
    std::string detail,
    int nativeCodeValue
) : std::runtime_error(std::move(detail)),
    code(std::move(codeValue)),
    stage(std::move(stageValue)),
    nativeCode(nativeCodeValue) {}

ProjectRecoveryJournal::ProjectRecoveryJournal() = default;

ProjectRecoveryJournal::ProjectRecoveryJournal(std::filesystem::path recoveryRoot)
    : recoveryRoot_(std::move(recoveryRoot)) {}

const std::filesystem::path& ProjectRecoveryJournal::configuredRecoveryRoot() const noexcept {
    return recoveryRoot_;
}

ProjectRecoveryJournalWriteReceipt ProjectRecoveryJournal::write(
    ProjectRuntime& runtime,
    const std::filesystem::path& packagePath,
    std::optional<std::uint64_t> expectedProjectGeneration,
    std::stop_token cancellation
) const {
    return writeImpl(
        *this,
        runtime,
        packagePath,
        expectedProjectGeneration,
        cancellation,
        nullptr
    );
}

ProjectRecoveryJournalInspection ProjectRecoveryJournal::inspect(
    const std::filesystem::path& packagePath,
    std::stop_token cancellation
) const {
    RecoveryMutationLease lease(cancellation);
    const auto normalizedPackage = normalizePackage(packagePath);
    const auto root = ensureRecoveryRoot(recoveryRoot_);
    const auto path = journalPath(root, normalizedPackage);
    const auto journal = readFile(
        path,
        maximumJournalBytes,
        true,
        cancellation,
        "readJournal"
    );
    if (!journal) return {ProjectRecoveryJournalStatus::missing, std::nullopt};
    auto candidate = parseJournal(path, *journal, normalizedPackage, cancellation);
    const auto baseline = readFile(
        normalizedPackage / L"project.json",
        defaultMaximumProjectJsonBytes,
        false,
        cancellation,
        "readBaseline"
    );
    const auto baselineSha = sha256(*baseline);
    auto status = ProjectRecoveryJournalStatus::staleBaseline;
    if (baselineSha == candidate.baselineProjectJsonSha256) {
        status = baselineSha == candidate.projectJsonSha256
            ? ProjectRecoveryJournalStatus::redundant
            : ProjectRecoveryJournalStatus::recoverable;
    }
    return {status, std::move(candidate)};
}

std::optional<ProjectRecoveryJournalFingerprint> ProjectRecoveryJournal::fingerprint(
    const std::filesystem::path& packagePath,
    std::stop_token cancellation
) const {
    RecoveryMutationLease lease(cancellation);
    const auto normalizedPackage = normalizePackage(packagePath);
    const auto root = ensureRecoveryRoot(recoveryRoot_);
    const auto path = journalPath(root, normalizedPackage);
    const auto content = readFile(
        path,
        maximumJournalBytes,
        true,
        cancellation,
        "fingerprintJournal"
    );
    if (!content) return std::nullopt;
    return ProjectRecoveryJournalFingerprint{
        path,
        sha256(*content),
        content->size(),
    };
}

bool ProjectRecoveryJournal::discard(
    const std::filesystem::path& packagePath,
    std::string_view expectedJournalSha256,
    std::stop_token cancellation
) const {
    if (!isSha256(expectedJournalSha256)) {
        fail(
            "invalidRecoveryFingerprint",
            "discardJournal",
            "expected recovery journal SHA-256 is malformed"
        );
    }
    RecoveryMutationLease lease(cancellation);
    const auto normalizedPackage = normalizePackage(packagePath);
    const auto root = ensureRecoveryRoot(recoveryRoot_);
    const auto path = journalPath(root, normalizedPackage);
    checkCancellation(cancellation, "discardJournal");
    HandleOwner file(CreateFileW(
        path.c_str(),
        GENERIC_READ | FILE_READ_ATTRIBUTES | DELETE,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr
    ));
    if (!file.valid()) {
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return false;
        fail(
            "fileUnavailable",
            "discardJournal",
            "recovery journal cannot be opened",
            static_cast<int>(error)
        );
    }
    static_cast<void>(fileSnapshot(file.get(), "discardJournal"));
    const auto content = readHandle(
        file.get(),
        maximumJournalBytes,
        cancellation,
        "discardJournal"
    );
    if (sha256(content) != expectedJournalSha256) {
        fail(
            "recoveryCandidateChanged",
            "discardJournal",
            "recovery journal changed after the choice was presented"
        );
    }
    checkCancellation(cancellation, "discardJournal");
    FILE_DISPOSITION_INFO disposition{static_cast<BOOLEAN>(TRUE)};
    if (!SetFileInformationByHandle(
            file.get(),
            FileDispositionInfo,
            &disposition,
            static_cast<DWORD>(sizeof(disposition))
        )) {
        fail(
            "discardFailed",
            "discardJournal",
            "recovery journal could not be discarded",
            static_cast<int>(GetLastError())
        );
    }
    return true;
}

bool ProjectRecoveryJournal::retire(
    const std::filesystem::path& packagePath,
    std::uint64_t expectedProjectGeneration,
    std::uint64_t committedRevision,
    std::stop_token cancellation
) const {
    RecoveryMutationLease lease(cancellation);
    const auto normalizedPackage = normalizePackage(packagePath);
    const auto root = ensureRecoveryRoot(recoveryRoot_);
    const auto path = journalPath(root, normalizedPackage);
    checkCancellation(cancellation, "retireJournal");
    HandleOwner file(CreateFileW(
        path.c_str(),
        GENERIC_READ | FILE_READ_ATTRIBUTES | DELETE,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr
    ));
    if (!file.valid()) {
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return false;
        fail("fileUnavailable", "retireJournal", "recovery journal cannot be opened", static_cast<int>(error));
    }
    static_cast<void>(fileSnapshot(file.get(), "retireJournal"));
    const auto content = readHandle(
        file.get(),
        maximumJournalBytes,
        cancellation,
        "retireJournal"
    );
    const auto candidate = parseJournal(path, content, normalizedPackage, cancellation);
    if (
        candidate.projectGeneration != expectedProjectGeneration
        || candidate.revision > committedRevision
    ) {
        return false;
    }
    checkCancellation(cancellation, "retireJournal");
    FILE_DISPOSITION_INFO disposition{static_cast<BOOLEAN>(TRUE)};
    if (!SetFileInformationByHandle(
            file.get(),
            FileDispositionInfo,
            &disposition,
            static_cast<DWORD>(sizeof(disposition))
        )) {
        fail("retireFailed", "retireJournal", "recovery journal could not be retired", static_cast<int>(GetLastError()));
    }
    return true;
}

namespace testing {

ProjectRecoveryJournalWriteReceipt writeProjectRecoveryJournal(
    const ProjectRecoveryJournal& journal,
    ProjectRuntime& runtime,
    const std::filesystem::path& packagePath,
    std::optional<std::uint64_t> expectedProjectGeneration,
    std::stop_token cancellation,
    ProjectRecoveryJournalCheckpoints* checkpoints
) {
    return writeImpl(
        journal,
        runtime,
        packagePath,
        expectedProjectGeneration,
        cancellation,
        checkpoints
    );
}

}

}
