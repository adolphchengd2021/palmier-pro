#pragma once

#include "palmier/project/project_reader.hpp"

#include <cstddef>
#include <filesystem>
#include <stop_token>
#include <stdexcept>
#include <string>

namespace palmier::project {

inline constexpr std::size_t defaultMaximumProjectJsonBytes = 64U * 1024U * 1024U;
inline constexpr std::size_t defaultMaximumProjectJsonValues = 500'000;
inline constexpr std::size_t defaultMaximumProjectJsonStringBytes = 64U * 1024U * 1024U;

struct ProjectPackageReadOptions final {
    std::size_t maximumProjectJsonBytes{defaultMaximumProjectJsonBytes};
    std::size_t maximumProjectJsonValues{defaultMaximumProjectJsonValues};
    std::size_t maximumProjectJsonStringBytes{defaultMaximumProjectJsonStringBytes};
    std::stop_token cancellation{};
};

class ProjectPackageReadError final : public std::runtime_error {
public:
    ProjectPackageReadError(std::string code, std::string detail);

    const std::string code;
};

// Caller must run this synchronous file operation off the UI thread.
ProjectDocument readProjectPackage(
    const std::filesystem::path& packagePath,
    const IdGenerator& idGenerator,
    ProjectPackageReadOptions options = {}
);

}
