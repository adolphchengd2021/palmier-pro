#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <vector>

namespace palmier::project {

inline constexpr std::size_t defaultMaximumMediaJsonBytes = 16U * 1024U * 1024U;

enum class MediaSourceKind {
    external,
    project,
};

struct MediaManifestSource final {
    MediaSourceKind kind;
    std::string path;
};

struct MediaManifestEntry final {
    std::string id;
    std::string type;
    MediaManifestSource source;
    std::optional<bool> hasAudio;
};

struct MediaManifest final {
    std::vector<MediaManifestEntry> entries;
};

struct MediaManifestReadOptions final {
    std::size_t maximumMediaJsonBytes{defaultMaximumMediaJsonBytes};
    std::stop_token cancellation{};
};

class MediaManifestReadError final : public std::runtime_error {
public:
    MediaManifestReadError(std::string code, std::string jsonPointer, std::string detail);

    const std::string code;
    const std::string jsonPointer;
};

// Caller must run this synchronous file operation off the UI thread.
std::optional<MediaManifest> readMediaManifest(
    const std::filesystem::path& packagePath,
    MediaManifestReadOptions options = {}
);

}
