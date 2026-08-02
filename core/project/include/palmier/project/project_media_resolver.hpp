#pragma once

#include "palmier/project/media_manifest_reader.hpp"

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>

namespace palmier::project {

struct ResolvedProjectMediaReference final {
    std::filesystem::path path;
    std::optional<bool> hasAudio;
    MediaSourceKind sourceKind{MediaSourceKind::project};
};

class ProjectMediaResolveError final : public std::runtime_error {
public:
    ProjectMediaResolveError(std::string code, std::string detail);

    const std::string code;
};

// Caller must run this synchronous filesystem operation off the UI thread.
ResolvedProjectMediaReference resolveProjectMediaReference(
    const MediaManifest& manifest,
    std::string_view mediaId,
    std::string_view requiredType,
    const std::filesystem::path& packagePath,
    std::stop_token cancellation = {}
);

}
