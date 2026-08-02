#pragma once

#include "palmier/json/json_document.hpp"
#include "palmier/project/project_reader.hpp"

#include <cstdint>
#include <functional>
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

struct CommandResult final {
    bool changed;
    std::uint64_t revisionBefore;
    std::uint64_t revisionAfter;
    std::string actionId;
    palmier::json::Value payload;
};

class CommandError final : public std::runtime_error {
public:
    CommandError(std::string code, std::string detail);

    const std::string code;
};

class ProjectSession final {
public:
    ProjectSession(const ProjectDocument& document, IdGenerator idGenerator);

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
    CommandResult undo(std::stop_token cancellation = {});

    std::uint64_t revision() const;
    bool dirty() const;

private:
    struct TrackSnapshot final {
        std::size_t timelineIndex;
        std::size_t trackIndex;
        std::vector<Clip> clips;
    };

    struct UndoEntry final {
        std::string actionId;
        std::vector<TrackSnapshot> tracks;
        std::vector<std::string> createdClipIds;
    };

    Project project_;
    IdGenerator idGenerator_;
    std::vector<std::string> unsafeClipIds_;
    std::set<std::string, std::less<>> sessionGeneratedClipIds_;
    std::vector<UndoEntry> undoJournal_;
    std::uint64_t revision_ = 0;
    bool dirty_ = false;
    mutable std::mutex mutex_;
};

}
