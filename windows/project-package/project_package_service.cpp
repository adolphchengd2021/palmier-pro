#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0A00
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include "palmier/project/project_package_service.hpp"

#define NOMINMAX
#include <Windows.h>

#include <condition_variable>
#include <deque>
#include <future>
#include <iomanip>
#include <limits>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <utility>

namespace palmier::project {
namespace {

void checkCancellation(std::stop_token cancellation) {
    if (cancellation.stop_requested()) {
        throw ProjectPackageServiceError("cancelled", "project package operation was cancelled");
    }
}

bool hasPalmierExtension(const std::filesystem::path& path) {
    return _wcsicmp(path.extension().c_str(), L".palmier") == 0;
}

std::filesystem::path normalizedExistingPackage(const std::filesystem::path& path) {
    if (path.empty() || !hasPalmierExtension(path)) {
        throw ProjectPackageServiceError(
            "invalidPackagePath",
            "project package path must end in .palmier"
        );
    }
    std::error_code error;
    auto absolute = std::filesystem::absolute(path, error);
    if (error) {
        throw ProjectPackageServiceError(
            "invalidPackagePath",
            "project package path could not be made absolute",
            error.value()
        );
    }
    const DWORD requestedAttributes = GetFileAttributesW(absolute.c_str());
    if (
        requestedAttributes == INVALID_FILE_ATTRIBUTES
        || (requestedAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0
    ) {
        throw ProjectPackageServiceError(
            "unsupportedPackageEntry",
            "project package cannot be a reparse point",
            static_cast<int>(
                requestedAttributes == INVALID_FILE_ATTRIBUTES
                    ? GetLastError()
                    : ERROR_REPARSE_TAG_INVALID
            )
        );
    }
    auto normalized = std::filesystem::weakly_canonical(absolute, error);
    if (error || !std::filesystem::is_directory(normalized, error) || error) {
        throw ProjectPackageServiceError(
            "invalidPackagePath",
            "project package path is not an existing directory",
            error.value()
        );
    }
    const auto projectJson = normalized / L"project.json";
    const DWORD projectJsonAttributes = GetFileAttributesW(projectJson.c_str());
    if (
        projectJsonAttributes == INVALID_FILE_ATTRIBUTES
        || (projectJsonAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0
        || (projectJsonAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0
    ) {
        throw ProjectPackageServiceError(
            "invalidPackagePath",
            "project package must contain a regular non-reparse project.json",
            static_cast<int>(
                projectJsonAttributes == INVALID_FILE_ATTRIBUTES
                    ? GetLastError()
                    : ERROR_REPARSE_TAG_INVALID
            )
        );
    }
    return normalized;
}

std::filesystem::path normalizedNewPackage(const std::filesystem::path& path) {
    if (path.empty() || !hasPalmierExtension(path)) {
        throw ProjectPackageServiceError(
            "invalidPackagePath",
            "Save As destination must end in .palmier"
        );
    }
    std::error_code error;
    auto absolute = std::filesystem::absolute(path, error).lexically_normal();
    if (error || absolute.filename().empty()) {
        throw ProjectPackageServiceError(
            "invalidPackagePath",
            "Save As destination could not be normalized",
            error.value()
        );
    }
    const DWORD requestedParentAttributes = GetFileAttributesW(absolute.parent_path().c_str());
    if (
        requestedParentAttributes == INVALID_FILE_ATTRIBUTES
        || (requestedParentAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0
    ) {
        throw ProjectPackageServiceError(
            "unsupportedPackageEntry",
            "Save As destination parent cannot be a reparse point",
            static_cast<int>(
                requestedParentAttributes == INVALID_FILE_ATTRIBUTES
                    ? GetLastError()
                    : ERROR_REPARSE_TAG_INVALID
            )
        );
    }
    auto parent = std::filesystem::weakly_canonical(absolute.parent_path(), error);
    if (error || !std::filesystem::is_directory(parent, error) || error) {
        throw ProjectPackageServiceError(
            "invalidPackagePath",
            "Save As destination parent is not an existing directory",
            error.value()
        );
    }
    return parent / absolute.filename();
}

std::wstring foldedPath(const std::filesystem::path& path) {
    const auto source = path.native();
    if (source.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        throw ProjectPackageServiceError("invalidPackagePath", "project package path is too long");
    }
    std::wstring result(source.size(), L'\0');
    if (!source.empty()) {
        const int converted = LCMapStringEx(
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
            throw ProjectPackageServiceError(
                "invalidPackagePath",
                "project package path could not be case-normalized",
                static_cast<int>(GetLastError())
            );
        }
    }
    return result;
}

std::wstring lockName(const std::filesystem::path& path) {
    constexpr std::uint64_t offset = 14'695'981'039'346'656'037ULL;
    constexpr std::uint64_t prime = 1'099'511'628'211ULL;
    std::uint64_t hash = offset;
    for (const auto character : foldedPath(path)) {
        hash ^= static_cast<std::uint16_t>(character);
        hash *= prime;
    }
    std::wostringstream name;
    name << L"Global\\PalmierPro.ProjectPackage."
         << std::hex << std::setw(16) << std::setfill(L'0') << hash;
    return name.str();
}

struct LockAcquisition final {
    std::uint64_t token{};
    DWORD nativeCode{};
};

class NamedLockManager final {
public:
    static NamedLockManager& instance() {
        static NamedLockManager manager;
        return manager;
    }

    std::uint64_t acquire(const std::wstring& name) {
        auto promise = std::make_shared<std::promise<LockAcquisition>>();
        auto future = promise->get_future();
        {
            std::scoped_lock lock(mutex_);
            requests_.push_back(Request{RequestKind::acquire, name, 0, promise});
        }
        condition_.notify_one();
        const auto result = future.get();
        if (result.token == 0) {
            throw ProjectPackageServiceError(
                "projectPackageBusy",
                "another Palmier Pro process owns this project package",
                static_cast<int>(result.nativeCode)
            );
        }
        return result.token;
    }

    void release(std::uint64_t token) noexcept {
        if (token == 0) return;
        {
            std::scoped_lock lock(mutex_);
            requests_.push_back(Request{RequestKind::release, {}, token, {}});
        }
        condition_.notify_one();
    }

private:
    enum class RequestKind { acquire, release };
    struct Request final {
        RequestKind kind;
        std::wstring name;
        std::uint64_t token;
        std::shared_ptr<std::promise<LockAcquisition>> promise;
    };
    struct HeldLock final {
        HANDLE handle{INVALID_HANDLE_VALUE};
        std::uint64_t references{};
    };

    NamedLockManager() : worker_([this](std::stop_token stop) { run(stop); }) {}

    ~NamedLockManager() {
        worker_.request_stop();
        condition_.notify_one();
        worker_.join();
    }

    void run(std::stop_token stop) noexcept {
        while (true) {
            Request request{};
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, stop, [&] { return !requests_.empty(); });
                if (requests_.empty()) {
                    if (stop.stop_requested()) break;
                    continue;
                }
                request = std::move(requests_.front());
                requests_.pop_front();
            }
            if (request.kind == RequestKind::release) {
                releaseOnOwner(request.token);
                continue;
            }
            acquireOnOwner(std::move(request));
        }
        while (true) {
            Request request{};
            {
                std::scoped_lock lock(mutex_);
                if (requests_.empty()) break;
                request = std::move(requests_.front());
                requests_.pop_front();
            }
            if (request.kind == RequestKind::release) {
                releaseOnOwner(request.token);
            } else if (request.promise) {
                request.promise->set_value({0, ERROR_OPERATION_ABORTED});
            }
        }
        for (auto& [name, held] : held_) {
            static_cast<void>(name);
            while (held.references > 0) {
                static_cast<void>(ReleaseMutex(held.handle));
                --held.references;
            }
            CloseHandle(held.handle);
        }
        held_.clear();
        tokens_.clear();
    }

    void acquireOnOwner(Request request) noexcept {
        LockAcquisition result{};
        HANDLE acquiredHandle = nullptr;
        try {
            auto held = held_.find(request.name);
            if (held != held_.end()) {
                result.nativeCode = ERROR_SHARING_VIOLATION;
                request.promise->set_value(result);
                return;
            }
            if (nextToken_ == 0) {
                result.nativeCode = ERROR_ARITHMETIC_OVERFLOW;
                request.promise->set_value(result);
                return;
            }
            acquiredHandle = CreateMutexW(nullptr, FALSE, request.name.c_str());
            if (acquiredHandle == nullptr) {
                result.nativeCode = GetLastError();
                request.promise->set_value(result);
                return;
            }
            const DWORD wait = WaitForSingleObject(acquiredHandle, 0);
            if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
                result.nativeCode = wait == WAIT_TIMEOUT ? ERROR_SHARING_VIOLATION : GetLastError();
                CloseHandle(acquiredHandle);
                acquiredHandle = nullptr;
                request.promise->set_value(result);
                return;
            }
            held = held_.emplace(request.name, HeldLock{acquiredHandle, 1}).first;
            acquiredHandle = nullptr;
            const auto token = nextToken_;
            try {
                tokens_.emplace(token, request.name);
            } catch (...) {
                static_cast<void>(ReleaseMutex(held->second.handle));
                CloseHandle(held->second.handle);
                held_.erase(held);
                throw;
            }
            ++nextToken_;
            result.token = token;
        } catch (...) {
            if (acquiredHandle != nullptr) {
                static_cast<void>(ReleaseMutex(acquiredHandle));
                CloseHandle(acquiredHandle);
            }
            result.nativeCode = ERROR_NOT_ENOUGH_MEMORY;
        }
        request.promise->set_value(result);
    }

    void releaseOnOwner(std::uint64_t token) noexcept {
        const auto tokenEntry = tokens_.find(token);
        if (tokenEntry == tokens_.end()) return;
        const auto held = held_.find(tokenEntry->second);
        if (held != held_.end()) {
            static_cast<void>(ReleaseMutex(held->second.handle));
            if (held->second.references > 0) --held->second.references;
            if (held->second.references == 0) {
                CloseHandle(held->second.handle);
                held_.erase(held);
            }
        }
        tokens_.erase(tokenEntry);
    }

    std::mutex mutex_;
    std::condition_variable_any condition_;
    std::deque<Request> requests_;
    std::unordered_map<std::wstring, HeldLock> held_;
    std::unordered_map<std::uint64_t, std::wstring> tokens_;
    std::uint64_t nextToken_{1};
    std::jthread worker_;
};

}

struct ProjectPackageActivation::Lease final {
    explicit Lease(std::uint64_t value) : token(value) {}
    ~Lease() { NamedLockManager::instance().release(token); }

    const std::uint64_t token;
};

ProjectPackageServiceError::ProjectPackageServiceError(
    std::string codeValue,
    std::string detail,
    int nativeCodeValue
) : std::runtime_error(std::move(detail)),
    code(std::move(codeValue)),
    nativeCode(nativeCodeValue) {}

ProjectPackageActivation::ProjectPackageActivation(
    std::shared_ptr<const ProjectPackageIdentity> identity,
    std::shared_ptr<Lease> lease
) : identity_(std::move(identity)), lease_(std::move(lease)) {}

bool ProjectPackageActivation::valid() const noexcept {
    return identity_ != nullptr && lease_ != nullptr && !identity_->path.empty();
}

const std::filesystem::path& ProjectPackageActivation::path() const noexcept {
    static const std::filesystem::path empty;
    return identity_ ? identity_->path : empty;
}

ProjectPackageService::ProjectPackageService()
    : ProjectPackageService(writeProjectPackageAs) {}

ProjectPackageService::ProjectPackageService(SaveAsWriter saveAsWriter)
    : saveAsWriter_(std::move(saveAsWriter)) {
    if (!saveAsWriter_) {
        throw ProjectPackageServiceError(
            "invalidSaveAsWriter",
            "project package Save As writer is missing"
        );
    }
}
ProjectPackageService::~ProjectPackageService() { close(); }

ProjectPackageActivation ProjectPackageService::prepareActivation(
    const std::filesystem::path& packagePath,
    std::uint64_t projectGeneration,
    std::stop_token cancellation
) {
    checkCancellation(cancellation);
    if (projectGeneration == 0) {
        throw ProjectPackageServiceError(
            "invalidPackageActivation",
            "project package generation must be nonzero"
        );
    }
    const auto normalized = normalizedExistingPackage(packagePath);
    {
        std::scoped_lock lock(mutex_);
        if (identity_ && activeLease_ && identity_->path == normalized) {
            if (nextSerial_ == 0) {
                throw ProjectPackageServiceError(
                    "packageIdentityOverflow",
                    "project package identity cannot advance"
                );
            }
            auto identity = std::make_shared<const ProjectPackageIdentity>(
                ProjectPackageIdentity{normalized, projectGeneration, nextSerial_++}
            );
            return ProjectPackageActivation(std::move(identity), activeLease_);
        }
    }
    checkCancellation(cancellation);
    auto lease = std::make_shared<ProjectPackageActivation::Lease>(
        NamedLockManager::instance().acquire(lockName(normalized))
    );
    checkCancellation(cancellation);
    std::shared_ptr<const ProjectPackageIdentity> identity;
    {
        std::scoped_lock lock(mutex_);
        if (nextSerial_ == 0) {
            throw ProjectPackageServiceError(
                "packageIdentityOverflow",
                "project package identity cannot advance"
            );
        }
        identity = std::make_shared<const ProjectPackageIdentity>(
            ProjectPackageIdentity{normalized, projectGeneration, nextSerial_++}
        );
    }
    return ProjectPackageActivation(std::move(identity), std::move(lease));
}

void ProjectPackageService::activate(ProjectPackageActivation activation) noexcept {
    if (!activation.valid()) return;
    std::scoped_lock lock(mutex_);
    activeLease_ = std::move(activation.lease_);
    identity_ = std::move(activation.identity_);
}

std::optional<ProjectPackageIdentity> ProjectPackageService::identity() const {
    std::scoped_lock lock(mutex_);
    if (!identity_) return std::nullopt;
    return *identity_;
}

ProjectPackageWriteReceipt ProjectPackageService::save(
    ProjectRuntime& runtime,
    std::optional<std::uint64_t> expectedProjectGeneration,
    std::stop_token cancellation
) const {
    ProjectPackageIdentity active;
    std::shared_ptr<ProjectPackageActivation::Lease> lease;
    {
        std::scoped_lock lock(mutex_);
        if (!identity_ || !activeLease_) {
            throw ProjectPackageServiceError("noActiveProject", "no project package is active");
        }
        active = *identity_;
        lease = activeLease_;
    }
    static_cast<void>(lease);
    if (expectedProjectGeneration && *expectedProjectGeneration != active.projectGeneration) {
        throw ProjectPackageServiceError(
            "staleProjectGeneration",
            "project package generation changed before Save"
        );
    }
    return writeProjectPackage(
        runtime,
        active.path,
        active.projectGeneration,
        cancellation
    );
}

ProjectPackageServiceSaveAsReceipt ProjectPackageService::saveAs(
    ProjectRuntime& runtime,
    const std::filesystem::path& destinationPackagePath,
    std::optional<std::uint64_t> expectedProjectGeneration,
    std::stop_token cancellation
) {
    ProjectPackageIdentity source;
    std::shared_ptr<const ProjectPackageIdentity> sourceIdentity;
    std::shared_ptr<ProjectPackageActivation::Lease> sourceLease;
    {
        std::scoped_lock lock(mutex_);
        if (!identity_ || !activeLease_) {
            throw ProjectPackageServiceError("noActiveProject", "no project package is active");
        }
        sourceIdentity = identity_;
        source = *identity_;
        sourceLease = activeLease_;
    }
    if (expectedProjectGeneration && *expectedProjectGeneration != source.projectGeneration) {
        throw ProjectPackageServiceError(
            "staleProjectGeneration",
            "project package generation changed before Save As"
        );
    }
    checkCancellation(cancellation);
    const auto destination = normalizedNewPackage(destinationPackagePath);
    auto targetLease = std::make_shared<ProjectPackageActivation::Lease>(
        NamedLockManager::instance().acquire(lockName(destination))
    );
    checkCancellation(cancellation);
    std::shared_ptr<const ProjectPackageIdentity> proposedIdentity;
    std::optional<ProjectPackageIdentity> preparedReceiptIdentity;
    {
        std::scoped_lock lock(mutex_);
        if (identity_ != sourceIdentity || activeLease_ != sourceLease) {
            throw ProjectPackageServiceError(
                "stalePackageIdentity",
                "project package identity changed before Save As"
            );
        }
        if (nextSerial_ == 0) {
            throw ProjectPackageServiceError(
                "packageIdentityOverflow",
                "project package identity cannot advance"
            );
        }
        proposedIdentity = std::make_shared<const ProjectPackageIdentity>(
            ProjectPackageIdentity{destination, source.projectGeneration, nextSerial_++}
        );
        preparedReceiptIdentity = *proposedIdentity;
    }
    auto package = saveAsWriter_(
        runtime,
        source.path,
        destination,
        source.projectGeneration,
        cancellation
    );
    bool adopted{};
    std::optional<ProjectPackageIdentity> adoptedIdentity;
    ProjectPackageWriteReceipt write{
        package.projectGeneration,
        package.revision,
        package.stateId,
        package.projectJsonBytes,
        false,
        true,
        ProjectPackageWriteWarning::runtimeReplacedAfterSave,
    };
    std::scoped_lock lock(mutex_);
    if (
        identity_ == sourceIdentity
        && activeLease_ == sourceLease
    ) {
        activeLease_ = targetLease;
        identity_ = proposedIdentity;
        adoptedIdentity = std::move(preparedReceiptIdentity);
        try {
            const auto acknowledged = runtime.markPersisted(
                package.stateId,
                package.projectGeneration
            );
            write.runtimeAcknowledged = true;
            write.runtimeDirty = acknowledged.session->dirty();
            write.warning = ProjectPackageWriteWarning::none;
            adopted = true;
        } catch (const ProjectRuntimeError& error) {
            activeLease_ = sourceLease;
            identity_ = sourceIdentity;
            adoptedIdentity.reset();
            if (error.code == "runtimeClosed") {
                write.warning = ProjectPackageWriteWarning::runtimeClosedAfterSave;
            }
        } catch (...) {
            activeLease_ = sourceLease;
            identity_ = sourceIdentity;
            adoptedIdentity.reset();
            write.warning = ProjectPackageWriteWarning::runtimeAcknowledgementFailed;
        }
    }
    return {std::move(package), std::move(write), adopted, std::move(adoptedIdentity)};
}

void ProjectPackageService::close() noexcept {
    std::shared_ptr<ProjectPackageActivation::Lease> lease;
    {
        std::scoped_lock lock(mutex_);
        lease = std::move(activeLease_);
        identity_.reset();
    }
}

}
