#pragma once

#include "palmier/json/json_document.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace palmier::project {

class ProjectSession;

enum class EntityIdOrigin {
    persisted,
    synthesized,
};

struct EntityId final {
    std::string value;
    EntityIdOrigin origin;
};

struct Clip final {
    EntityId id;
    std::string mediaRef;
    std::string mediaType;
    std::string sourceClipType;
    std::int64_t startFrame;
    std::int64_t durationFrames;
    std::int64_t trimStartFrame;
    std::int64_t trimEndFrame;
    double speed;
    double volume;
    double opacity;
    std::optional<std::string> blendMode;
    std::optional<std::string> linkGroupId;
    std::optional<std::string> captionGroupId;
    std::optional<std::string> multicamGroupId;
};

struct Track final {
    EntityId id;
    std::string type;
    bool muted;
    bool hidden;
    bool syncLocked;
    double displayHeight;
    std::vector<Clip> clips;
};

struct Timeline final {
    EntityId id;
    std::string name;
    std::int64_t fps;
    std::int64_t width;
    std::int64_t height;
    bool settingsConfigured;
    std::optional<std::string> folderId;
    std::vector<Track> tracks;
};

struct Project final {
    std::vector<Timeline> timelines;
    std::string activeTimelineId;
    std::vector<std::string> openTimelineIds;
};

enum class RootKind {
    current,
    legacy,
};

struct Diagnostic final {
    std::string code;
    std::string jsonPointer;
};

enum class ProjectDocumentDisposition {
    safeEdits,
};

class ProjectDocument final {
public:
    ProjectDocument(const ProjectDocument&) = default;
    ProjectDocument(ProjectDocument&&) noexcept = default;
    ProjectDocument& operator=(const ProjectDocument&) = default;
    ProjectDocument& operator=(ProjectDocument&&) noexcept = default;

    const palmier::json::Value& source() const noexcept;
    RootKind rootKind() const noexcept;
    const Project& project() const noexcept;
    const std::vector<Diagnostic>& diagnostics() const noexcept;
    ProjectDocumentDisposition disposition() const noexcept;

private:
    ProjectDocument(
        palmier::json::Value source,
        RootKind rootKind,
        Project project,
        std::vector<Diagnostic> diagnostics
    );

    friend ProjectDocument readProject(
        palmier::json::Value source,
        const std::function<std::string()>& idGenerator
    );
    friend ProjectDocument readProject(
        palmier::json::Value source,
        const std::function<std::string()>& idGenerator,
        std::stop_token cancellation
    );
    friend class ProjectSession;

    palmier::json::Value source_;
    RootKind rootKind_;
    Project project_;
    std::vector<Diagnostic> diagnostics_;
};

}
