#pragma once

#include "palmier/project/windows_project_package_writer.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <stdexcept>
#include <string>

namespace palmier::project {

class ProjectPackageServiceError final : public std::runtime_error {
public:
    ProjectPackageServiceError(std::string code, std::string detail, int nativeCode = 0);

    const std::string code;
    const int nativeCode;
};

struct ProjectPackageIdentity final {
    std::filesystem::path path;
    std::uint64_t projectGeneration;
    std::uint64_t serial;
};

class ProjectPackageActivation final {
public:
    ProjectPackageActivation() = default;
    ProjectPackageActivation(ProjectPackageActivation&&) noexcept = default;
    ProjectPackageActivation& operator=(ProjectPackageActivation&&) noexcept = default;

    ProjectPackageActivation(const ProjectPackageActivation&) = delete;
    ProjectPackageActivation& operator=(const ProjectPackageActivation&) = delete;

    bool valid() const noexcept;
    const std::filesystem::path& path() const noexcept;

private:
    struct Lease;
    ProjectPackageActivation(
        std::shared_ptr<const ProjectPackageIdentity> identity,
        std::shared_ptr<Lease> lease
    );

    std::shared_ptr<const ProjectPackageIdentity> identity_;
    std::shared_ptr<Lease> lease_;

    friend class ProjectPackageService;
};

struct ProjectPackageServiceSaveAsReceipt final {
    ProjectPackageSaveAsReceipt package;
    ProjectPackageWriteReceipt write;
    bool identityAdopted;
    std::optional<ProjectPackageIdentity> identity;
};

class ProjectPackageService final {
public:
    using SaveAsWriter = std::function<ProjectPackageSaveAsReceipt(
        ProjectRuntime&,
        const std::filesystem::path&,
        const std::filesystem::path&,
        std::optional<std::uint64_t>,
        std::stop_token
    )>;

    ProjectPackageService();
    explicit ProjectPackageService(SaveAsWriter saveAsWriter);
    ~ProjectPackageService();

    ProjectPackageService(const ProjectPackageService&) = delete;
    ProjectPackageService& operator=(const ProjectPackageService&) = delete;

    ProjectPackageActivation prepareActivation(
        const std::filesystem::path& packagePath,
        std::uint64_t projectGeneration,
        std::stop_token cancellation = {}
    );
    void activate(ProjectPackageActivation activation) noexcept;
    std::optional<ProjectPackageIdentity> identity() const;
    ProjectPackageWriteReceipt save(
        ProjectRuntime& runtime,
        std::optional<std::uint64_t> expectedProjectGeneration = {},
        std::stop_token cancellation = {}
    ) const;
    ProjectPackageServiceSaveAsReceipt saveAs(
        ProjectRuntime& runtime,
        const std::filesystem::path& destinationPackagePath,
        std::optional<std::uint64_t> expectedProjectGeneration = {},
        std::stop_token cancellation = {}
    );
    void close() noexcept;

private:
    mutable std::mutex mutex_;
    std::shared_ptr<ProjectPackageActivation::Lease> activeLease_;
    std::shared_ptr<const ProjectPackageIdentity> identity_;
    SaveAsWriter saveAsWriter_;
    std::uint64_t nextSerial_{1};
};

}
