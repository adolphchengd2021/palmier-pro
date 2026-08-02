#pragma once

#include "palmier/json/json_document.hpp"
#include "palmier/project/project_reader.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <vector>

namespace palmier::project {

struct TimelineQuery final {
    std::optional<std::int64_t> startFrame;
    std::optional<std::int64_t> endFrame;
    bool captionDetail = false;
};

struct SplitPoint final {
    std::string clipId;
    std::int64_t atFrame;
};

struct SplitClipsCommand final {
    std::optional<std::vector<SplitPoint>> splits;
    std::optional<std::size_t> trackIndex;
    std::optional<std::vector<std::int64_t>> frames;
};

struct ClipMove final {
    std::string clipId;
    std::optional<std::size_t> toTrack;
    std::optional<std::int64_t> toFrame;
};

struct MoveClipsCommand final {
    std::vector<ClipMove> moves;
};

struct RemoveClipsCommand final {
    std::vector<std::string> clipIds;
};

struct SetClipPropertiesCommand final {
    std::vector<std::string> clipIds;
    std::optional<std::int64_t> durationFrames;
    std::optional<std::int64_t> trimStartFrame;
    std::optional<std::int64_t> trimEndFrame;
    std::optional<double> speed;
};

struct ProjectSessionSnapshot final {
    ProjectDocument document;
    std::uint64_t revision;
    std::uint64_t stateId;
    std::uint64_t persistedStateId;
    std::size_t undoDepth;
    std::size_t redoDepth;

    bool dirty() const noexcept { return stateId != persistedStateId; }
};

using ProjectSessionPublicationFactory = std::function<
    std::shared_ptr<const ProjectSessionSnapshot>(ProjectSessionSnapshot)
>;

struct CommandResult final {
    bool changed;
    std::uint64_t revisionBefore;
    std::uint64_t revisionAfter;
    std::string actionId;
    std::unique_ptr<palmier::json::Value> payload;
    std::shared_ptr<const ProjectSessionSnapshot> publication;
};

struct ProjectSaveSnapshot final {
    palmier::json::Value source;
    std::uint64_t revision;
    std::uint64_t stateId;
};

class CommandError final : public std::runtime_error {
public:
    CommandError(std::string code, std::string detail);

    const std::string code;
};

class ProjectSession final {
public:
    ProjectSession(
        const ProjectDocument& document,
        IdGenerator idGenerator,
        ProjectSessionPublicationFactory publicationFactory = {}
    );

    ProjectSession(const ProjectSession&) = delete;
    ProjectSession& operator=(const ProjectSession&) = delete;

    palmier::json::Value getTimeline(
        const TimelineQuery& query = {},
        std::stop_token cancellation = {}
    ) const;
    CommandResult splitClips(
        const SplitClipsCommand& command,
        std::stop_token cancellation = {}
    );
    CommandResult moveClips(
        const MoveClipsCommand& command,
        std::stop_token cancellation = {}
    );
    CommandResult removeClips(
        const RemoveClipsCommand& command,
        std::stop_token cancellation = {}
    );
    CommandResult setClipProperties(
        const SetClipPropertiesCommand& command,
        std::stop_token cancellation = {}
    );
    CommandResult undo(std::stop_token cancellation = {});
    CommandResult redo(std::stop_token cancellation = {});

    ProjectSessionSnapshot snapshot(std::stop_token cancellation = {}) const;
    ProjectSaveSnapshot saveSnapshot(std::stop_token cancellation = {}) const;
    std::shared_ptr<const ProjectSessionSnapshot> markPersisted(std::uint64_t stateId);

    std::uint64_t revision() const;
    std::uint64_t stateId() const;
    std::uint64_t persistedStateId() const;
    bool dirty() const;

private:
    struct TrackSnapshot final {
        std::size_t timelineIndex;
        std::size_t trackIndex;
        std::vector<Clip> clips;
        palmier::json::Value sourceClips;
    };

    struct TimelineSnapshot final {
        std::size_t timelineIndex;
        Timeline timeline;
        palmier::json::Value sourceTimeline;
    };

    struct UndoEntry final {
        std::string actionId;
        std::vector<TrackSnapshot> tracks;
        std::unique_ptr<TimelineSnapshot> timeline;
        std::vector<std::string> createdClipIds;
        std::uint64_t beforeStateId;

        UndoEntry(
            std::string actionId,
            std::vector<TrackSnapshot> tracks,
            std::unique_ptr<TimelineSnapshot> timeline,
            std::vector<std::string> createdClipIds,
            std::uint64_t beforeStateId
        );
        UndoEntry(const UndoEntry& other);
        UndoEntry(UndoEntry&&) noexcept = default;
        UndoEntry& operator=(UndoEntry&&) noexcept = default;
    };

    struct RedoEntry final {
        UndoEntry undo;
        std::vector<TrackSnapshot> tracks;
        std::unique_ptr<TimelineSnapshot> timeline;
        std::uint64_t afterStateId;
    };

    std::shared_ptr<const ProjectSessionSnapshot> preparePublication(
        ProjectSessionSnapshot snapshot
    ) const;

    std::unique_ptr<palmier::json::Value> source_;
    RootKind rootKind_;
    Project project_;
    std::vector<Diagnostic> diagnostics_;
    IdGenerator idGenerator_;
    ProjectSessionPublicationFactory publicationFactory_;
    std::vector<std::string> unsafeClipIds_;
    std::set<std::string, std::less<>> sessionGeneratedClipIds_;
    std::vector<UndoEntry> undoJournal_;
    std::vector<RedoEntry> redoJournal_;
    std::uint64_t revision_ = 0;
    std::uint64_t stateId_ = 0;
    std::uint64_t persistedStateId_ = 0;
    std::uint64_t nextStateId_ = 1;
    mutable std::mutex mutex_;
};

}
