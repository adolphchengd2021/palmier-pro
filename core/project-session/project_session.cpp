#include "palmier/project/project_session.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <type_traits>
#include <utility>

namespace palmier::project {
namespace {

using palmier::json::Array;
using palmier::json::Number;
using palmier::json::Object;
using palmier::json::Value;

Number integerNumber(std::int64_t value) {
    return {std::to_string(value), value};
}

Number unsignedNumber(std::uint64_t value) {
    if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        throw CommandError("revisionOverflow", "revision exceeds the MCP integer domain");
    }
    return integerNumber(static_cast<std::int64_t>(value));
}

constexpr auto maximumRevision =
    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());

Number floatingNumber(double value) {
    char buffer[64]{};
    const auto result = std::to_chars(
        std::begin(buffer),
        std::end(buffer),
        value,
        std::chars_format::general,
        std::numeric_limits<double>::max_digits10
    );
    if (result.ec != std::errc{}) {
        throw CommandError("encodeFailed", "cannot encode a floating-point value");
    }
    std::string lexeme(buffer, static_cast<std::size_t>(result.ptr - buffer));
    if (lexeme.find_first_of(".eE") == std::string::npos) {
        lexeme += ".0";
    }
    return {std::move(lexeme), std::nullopt};
}

void checkCancellation(std::stop_token cancellation) {
    if (cancellation.stop_requested()) {
        throw CommandError("cancelled", "project command was cancelled");
    }
}

std::int64_t checkedEnd(const Clip& clip) {
    if (
        clip.startFrame < 0
        || clip.durationFrames <= 0
        || clip.durationFrames > std::numeric_limits<std::int64_t>::max() - clip.startFrame
    ) {
        throw CommandError("unsafeFrameRange", "clip has an unsafe frame range: " + clip.id.value);
    }
    return clip.startFrame + clip.durationFrames;
}

std::int64_t scaledFrameOffset(std::int64_t frames, double speed) {
    if (frames < 0 || !std::isfinite(speed) || speed <= 0.0) {
        throw CommandError("unsafeTiming", "clip timing cannot be represented safely");
    }
    const auto scaled = static_cast<double>(frames) * speed;
    const auto safeMaximum = std::nextafter(
        static_cast<double>(std::numeric_limits<std::int64_t>::max()),
        0.0
    );
    if (!std::isfinite(scaled) || scaled > safeMaximum) {
        throw CommandError("unsafeTiming", "clip timing exceeds the integer frame domain");
    }
    return static_cast<std::int64_t>(std::llround(scaled));
}

std::int64_t checkedFrameAdd(std::int64_t base, std::int64_t offset) {
    if (
        base < 0
        || offset < 0
        || offset > std::numeric_limits<std::int64_t>::max() - base
    ) {
        throw CommandError("unsafeTiming", "clip trim exceeds the integer frame domain");
    }
    return base + offset;
}

const Timeline& activeTimeline(const Project& project) {
    const auto timeline = std::find_if(
        project.timelines.begin(),
        project.timelines.end(),
        [&](const Timeline& candidate) { return candidate.id.value == project.activeTimelineId; }
    );
    if (timeline == project.timelines.end()) {
        throw CommandError("missingActiveTimeline", "the active timeline does not exist");
    }
    return *timeline;
}

Timeline& activeTimeline(Project& project) {
    const auto timeline = std::find_if(
        project.timelines.begin(),
        project.timelines.end(),
        [&](const Timeline& candidate) { return candidate.id.value == project.activeTimelineId; }
    );
    if (timeline == project.timelines.end()) {
        throw CommandError("missingActiveTimeline", "the active timeline does not exist");
    }
    return *timeline;
}

Value optionalString(const std::optional<std::string>& value) {
    return value ? Value(*value) : Value();
}

Value clipValue(const Clip& clip, std::size_t trackIndex) {
    const auto endFrame = checkedEnd(clip);
    return Value(Object{
        {"blendMode", optionalString(clip.blendMode)},
        {"captionGroupId", optionalString(clip.captionGroupId)},
        {"durationFrames", Value(integerNumber(clip.durationFrames))},
        {"frames", Value(Array{
            Value(integerNumber(clip.startFrame)),
            Value(integerNumber(endFrame)),
        })},
        {"id", Value(clip.id.value)},
        {"linkGroupId", optionalString(clip.linkGroupId)},
        {"mediaRef", Value(clip.mediaRef)},
        {"mediaType", Value(clip.mediaType)},
        {"multicamGroupId", optionalString(clip.multicamGroupId)},
        {"opacity", Value(floatingNumber(clip.opacity))},
        {"sourceClipType", Value(clip.sourceClipType)},
        {"speed", Value(floatingNumber(clip.speed))},
        {"startFrame", Value(integerNumber(clip.startFrame))},
        {"track", Value(integerNumber(static_cast<std::int64_t>(trackIndex)))},
        {"trimEndFrame", Value(integerNumber(clip.trimEndFrame))},
        {"trimStartFrame", Value(integerNumber(clip.trimStartFrame))},
        {"volume", Value(floatingNumber(clip.volume))},
    });
}

bool overlapsWindow(const Clip& clip, const TimelineQuery& query) {
    const auto end = checkedEnd(clip);
    const auto start = query.startFrame.value_or(std::numeric_limits<std::int64_t>::min());
    const auto windowEnd = query.endFrame.value_or(std::numeric_limits<std::int64_t>::max());
    return clip.startFrame < windowEnd && end > start;
}

std::vector<std::string> unsafeClipIds(const ProjectDocument& document) {
    static const std::set<std::string, std::less<>> safeKeys{
        "id", "mediaRef", "mediaType", "sourceClipType", "startFrame", "durationFrames",
        "trimStartFrame", "trimEndFrame", "speed", "volume", "opacity", "blendMode",
        "linkGroupId", "captionGroupId", "multicamGroupId",
    };
    std::vector<std::string> result;
    const auto inspectTimeline = [&](const Value& timelineValue) {
        const auto* tracks = timelineValue.find("tracks");
        if (!tracks || tracks->kind() != Value::Kind::array) {
            return;
        }
        for (const auto& track : tracks->array()) {
            const auto* clips = track.find("clips");
            if (!clips || clips->kind() != Value::Kind::array) {
                continue;
            }
            for (const auto& clip : clips->array()) {
                if (clip.kind() != Value::Kind::object) {
                    continue;
                }
                const auto* id = clip.find("id");
                if (!id || id->kind() != Value::Kind::string) {
                    continue;
                }
                const auto hasUnsupportedField = std::any_of(
                    clip.object().begin(),
                    clip.object().end(),
                    [&](const auto& field) { return !safeKeys.contains(field.first); }
                );
                if (hasUnsupportedField) {
                    result.push_back(id->string());
                }
            }
        }
    };

    const auto* timelines = document.source().find("timelines");
    if (timelines && timelines->kind() == Value::Kind::array) {
        for (const auto& timeline : timelines->array()) {
            inspectTimeline(timeline);
        }
    } else {
        inspectTimeline(document.source());
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

struct ClipLocation final {
    std::size_t trackIndex;
    std::size_t clipIndex;
};

ClipLocation uniqueClipLocation(const Timeline& timeline, std::string_view id) {
    std::optional<ClipLocation> found;
    for (std::size_t trackIndex = 0; trackIndex < timeline.tracks.size(); ++trackIndex) {
        const auto& clips = timeline.tracks[trackIndex].clips;
        for (std::size_t clipIndex = 0; clipIndex < clips.size(); ++clipIndex) {
            if (clips[clipIndex].id.value != id) {
                continue;
            }
            if (found) {
                throw CommandError("ambiguousClipId", "clip ID is duplicated: " + std::string(id));
            }
            found = ClipLocation{trackIndex, clipIndex};
        }
    }
    if (!found) {
        throw CommandError("clipNotFound", "clip does not exist: " + std::string(id));
    }
    return *found;
}

Object receiptBase(
    bool changed,
    std::uint64_t revisionBefore,
    std::uint64_t revisionAfter,
    std::string_view actionId
) {
    return Object{
        {"actionId", Value(std::string(actionId))},
        {"changed", Value(changed)},
        {"createdTracks", Value(Array{})},
        {"notes", Value(Array{})},
        {"removedClipIds", Value(Array{})},
        {"revisionAfter", Value(unsignedNumber(revisionAfter))},
        {"revisionBefore", Value(unsignedNumber(revisionBefore))},
        {"shifted", Value(Array{})},
    };
}

}

CommandError::CommandError(std::string codeValue, std::string detail)
    : std::runtime_error(std::move(detail)), code(std::move(codeValue)) {}

ProjectSession::ProjectSession(const ProjectDocument& document, IdGenerator idGenerator)
    : project_(document.project()),
      idGenerator_(std::move(idGenerator)),
      unsafeClipIds_(unsafeClipIds(document)) {
    if (!idGenerator_) {
        throw CommandError("invalidIdGenerator", "project session requires an ID generator");
    }
}

Value ProjectSession::getTimeline(
    const TimelineQuery& query,
    std::stop_token cancellation
) const {
    std::scoped_lock lock(mutex_);
    checkCancellation(cancellation);
    if (query.startFrame && query.endFrame && *query.startFrame >= *query.endFrame) {
        throw CommandError("invalidWindow", "startFrame must be less than endFrame");
    }
    if (query.captionDetail) {
        throw CommandError(
            "unsupportedTimelineQuery",
            "captionDetail is not available in the Windows technical MVP"
        );
    }
    const auto& timeline = activeTimeline(project_);
    Array tracks;
    std::int64_t totalFrames = 0;
    for (std::size_t trackIndex = 0; trackIndex < timeline.tracks.size(); ++trackIndex) {
        checkCancellation(cancellation);
        const auto& track = timeline.tracks[trackIndex];
        Array clips;
        for (const auto& clip : track.clips) {
            totalFrames = std::max(totalFrames, checkedEnd(clip));
            if (overlapsWindow(clip, query)) {
                clips.push_back(clipValue(clip, trackIndex));
            }
        }
        tracks.emplace_back(Object{
            {"clips", Value(std::move(clips))},
            {"hidden", Value(track.hidden)},
            {"index", Value(integerNumber(static_cast<std::int64_t>(trackIndex)))},
            {"muted", Value(track.muted)},
            {"syncLocked", Value(track.syncLocked)},
            {"trackId", Value(track.id.value)},
            {"type", Value(track.type)},
        });
    }
    Array timelines;
    for (const auto& entry : project_.timelines) {
        Object item{
            {"name", Value(entry.name)},
            {"timelineId", Value(entry.id.value)},
        };
        if (entry.id.value == project_.activeTimelineId) {
            item.emplace("active", Value(true));
        }
        timelines.emplace_back(std::move(item));
    }
    Object result{
        {"canGenerate", Value(false)},
        {"currentFrame", Value(integerNumber(0))},
        {"durationSeconds", Value(floatingNumber(
            static_cast<double>(totalFrames) / static_cast<double>(std::max<std::int64_t>(timeline.fps, 1))
        ))},
        {"fps", Value(integerNumber(timeline.fps))},
        {"height", Value(integerNumber(timeline.height))},
        {"id", Value(timeline.id.value)},
        {"name", Value(timeline.name)},
        {"timelines", Value(std::move(timelines))},
        {"totalFrames", Value(integerNumber(totalFrames))},
        {"tracks", Value(std::move(tracks))},
        {"width", Value(integerNumber(timeline.width))},
    };
    if (query.startFrame || query.endFrame) {
        result.emplace("window", Value(Array{
            Value(integerNumber(query.startFrame.value_or(0))),
            Value(integerNumber(std::min(query.endFrame.value_or(totalFrames), totalFrames))),
        }));
    }
    return Value(std::move(result));
}

CommandResult ProjectSession::splitClips(
    const SplitClipsCommand& command,
    std::stop_token cancellation
) {
    std::scoped_lock lock(mutex_);
    checkCancellation(cancellation);
    const auto explicitMode = command.splits && !command.splits->empty();
    const auto trackMode = command.trackIndex.has_value()
        || (command.frames && !command.frames->empty());
    if (explicitMode == trackMode) {
        throw CommandError(
            "invalidSplitMode",
            "pass exactly one of splits or trackIndex with frames"
        );
    }
    if (trackMode && (!command.trackIndex || !command.frames || command.frames->empty())) {
        throw CommandError(
            "invalidSplitMode",
            "track mode requires trackIndex and a non-empty frames array"
        );
    }

    auto& timeline = activeTimeline(project_);
    std::map<std::string, std::set<std::int64_t>, std::less<>> cutsByClip;
    if (explicitMode) {
        for (const auto& split : *command.splits) {
            checkCancellation(cancellation);
            if (split.clipId.empty()) {
                throw CommandError("invalidClipId", "clipId must not be empty");
            }
            const auto location = uniqueClipLocation(timeline, split.clipId);
            const auto& clip = timeline.tracks[location.trackIndex].clips[location.clipIndex];
            const auto end = checkedEnd(clip);
            if (split.atFrame <= clip.startFrame || split.atFrame >= end) {
                throw CommandError("invalidSplitFrame", "split frame must be strictly inside clip " + split.clipId);
            }
            cutsByClip[split.clipId].insert(split.atFrame);
        }
    } else {
        if (*command.trackIndex >= timeline.tracks.size()) {
            throw CommandError("trackNotFound", "trackIndex is outside the active timeline");
        }
        const auto& track = timeline.tracks[*command.trackIndex];
        for (const auto frame : *command.frames) {
            checkCancellation(cancellation);
            std::vector<std::string> matches;
            for (const auto& clip : track.clips) {
                if (frame > clip.startFrame && frame < checkedEnd(clip)) {
                    matches.push_back(clip.id.value);
                }
            }
            if (matches.size() != 1) {
                throw CommandError("invalidSplitFrame", "frame must resolve to exactly one clip on the track");
            }
            cutsByClip[matches.front()].insert(frame);
        }
    }

    if (cutsByClip.empty()) {
        throw CommandError("invalidSplitFrame", "split command has no valid cut points");
    }

    std::map<std::string, std::set<std::int64_t>, std::less<>> linkCuts;
    for (const auto& [clipId, cuts] : cutsByClip) {
        const auto location = uniqueClipLocation(timeline, clipId);
        const auto& clip = timeline.tracks[location.trackIndex].clips[location.clipIndex];
        if (clip.linkGroupId) {
            linkCuts[*clip.linkGroupId].insert(cuts.begin(), cuts.end());
        }
    }
    for (const auto& [linkGroupId, cuts] : linkCuts) {
        for (const auto& track : timeline.tracks) {
            for (const auto& clip : track.clips) {
                if (clip.linkGroupId != linkGroupId) {
                    continue;
                }
                const auto end = checkedEnd(clip);
                for (const auto frame : cuts) {
                    if (frame <= clip.startFrame || frame >= end) {
                        throw CommandError(
                            "linkedSplitMismatch",
                            "linked clip cannot be split at the requested frame: " + clip.id.value
                        );
                    }
                    cutsByClip[clip.id.value].insert(frame);
                }
            }
        }
    }

    std::set<std::size_t> affectedTracks;
    for (const auto& [clipId, cuts] : cutsByClip) {
        checkCancellation(cancellation);
        const auto location = uniqueClipLocation(timeline, clipId);
        const auto& clip = timeline.tracks[location.trackIndex].clips[location.clipIndex];
        if (
            clip.id.origin != EntityIdOrigin::persisted
            && !sessionGeneratedClipIds_.contains(clipId)
        ) {
            throw CommandError("unstableClipId", "split requires a persisted clip ID: " + clipId);
        }
        if (std::binary_search(unsafeClipIds_.begin(), unsafeClipIds_.end(), clipId)) {
            throw CommandError(
                "unsupportedClipSemantics",
                "clip has fields not represented by the Windows split model: " + clipId
            );
        }
        if (clip.captionGroupId || clip.multicamGroupId) {
            throw CommandError(
                "unsupportedClipSemantics",
                "caption and multicam clips are not supported by the Windows split slice"
            );
        }
        const auto end = checkedEnd(clip);
        for (const auto frame : cuts) {
            if (frame <= clip.startFrame || frame >= end) {
                throw CommandError("invalidSplitFrame", "split frame must be strictly inside clip " + clipId);
            }
        }
        affectedTracks.insert(location.trackIndex);
    }

    Project planned = project_;
    auto& plannedTimeline = activeTimeline(planned);
    std::set<std::string, std::less<>> usedIds;
    for (const auto& projectTimeline : project_.timelines) {
        usedIds.insert(projectTimeline.id.value);
        for (const auto& track : projectTimeline.tracks) {
            usedIds.insert(track.id.value);
            for (const auto& clip : track.clips) {
                usedIds.insert(clip.id.value);
                if (clip.linkGroupId) {
                    usedIds.insert(*clip.linkGroupId);
                }
            }
        }
    }
    const auto newId = [&]() {
        const auto generated = idGenerator_();
        if (generated.empty() || !usedIds.insert(generated).second) {
            throw CommandError("invalidGeneratedId", "ID generator returned an empty or duplicate value");
        }
        return generated;
    };
    const auto actionId = newId();
    std::map<std::pair<std::string, std::int64_t>, std::string> rightLinkGroups;
    std::vector<std::string> createdClipIds;
    Array changedClips;
    std::vector<TrackSnapshot> snapshots;
    snapshots.reserve(affectedTracks.size());
    const auto timelineIndex = static_cast<std::size_t>(
        std::distance(project_.timelines.begin(), std::find_if(
            project_.timelines.begin(),
            project_.timelines.end(),
            [&](const Timeline& entry) { return entry.id.value == project_.activeTimelineId; }
        ))
    );
    for (const auto trackIndex : affectedTracks) {
        checkCancellation(cancellation);
        snapshots.push_back({timelineIndex, trackIndex, timeline.tracks[trackIndex].clips});
        auto& clips = plannedTimeline.tracks[trackIndex].clips;
        std::vector<Clip> replacement;
        for (const auto& clip : clips) {
            const auto cuts = cutsByClip.find(clip.id.value);
            if (cuts == cutsByClip.end()) {
                replacement.push_back(clip);
                continue;
            }
            std::vector<std::int64_t> boundaries;
            boundaries.push_back(clip.startFrame);
            boundaries.insert(boundaries.end(), cuts->second.begin(), cuts->second.end());
            boundaries.push_back(checkedEnd(clip));
            for (std::size_t pieceIndex = 0; pieceIndex + 1 < boundaries.size(); ++pieceIndex) {
                Clip piece = clip;
                piece.startFrame = boundaries[pieceIndex];
                piece.durationFrames = boundaries[pieceIndex + 1] - boundaries[pieceIndex];
                piece.trimStartFrame = checkedFrameAdd(
                    clip.trimStartFrame,
                    scaledFrameOffset(piece.startFrame - clip.startFrame, clip.speed)
                );
                piece.trimEndFrame = checkedFrameAdd(
                    clip.trimEndFrame,
                    scaledFrameOffset(checkedEnd(clip) - boundaries[pieceIndex + 1], clip.speed)
                );
                if (pieceIndex > 0) {
                    piece.id = {newId(), EntityIdOrigin::synthesized};
                    createdClipIds.push_back(piece.id.value);
                    if (clip.linkGroupId) {
                        const auto key = std::make_pair(*clip.linkGroupId, boundaries[pieceIndex]);
                        auto [group, inserted] = rightLinkGroups.emplace(key, std::string{});
                        if (inserted) {
                            group->second = newId();
                        }
                        piece.linkGroupId = group->second;
                    }
                }
                changedClips.push_back(clipValue(piece, trackIndex));
                replacement.push_back(std::move(piece));
            }
        }
        clips = std::move(replacement);
    }
    checkCancellation(cancellation);

    const auto revisionBefore = revision_;
    if (revision_ >= maximumRevision) {
        throw CommandError("revisionOverflow", "project revision cannot advance");
    }
    const auto revisionAfter = revisionBefore + 1;
    auto plannedGeneratedClipIds = sessionGeneratedClipIds_;
    plannedGeneratedClipIds.insert(createdClipIds.begin(), createdClipIds.end());
    undoJournal_.reserve(undoJournal_.size() + 1);
    UndoEntry undoEntry{
        actionId,
        std::move(snapshots),
        std::move(createdClipIds),
    };
    auto payload = receiptBase(true, revisionBefore, revisionAfter, actionId);
    payload.emplace("clips", Value(std::move(changedClips)));
    CommandResult result{
        true,
        revisionBefore,
        revisionAfter,
        actionId,
        Value(std::move(payload)),
    };
    checkCancellation(cancellation);

    static_assert(std::is_nothrow_move_assignable_v<Project>);
    static_assert(std::is_nothrow_move_constructible_v<UndoEntry>);
    static_assert(std::is_nothrow_move_constructible_v<CommandResult>);
    static_assert(noexcept(sessionGeneratedClipIds_.swap(plannedGeneratedClipIds)));
    project_ = std::move(planned);
    sessionGeneratedClipIds_.swap(plannedGeneratedClipIds);
    revision_ = revisionAfter;
    dirty_ = true;
    undoJournal_.push_back(std::move(undoEntry));
    return result;
}

CommandResult ProjectSession::undo(std::stop_token cancellation) {
    std::scoped_lock lock(mutex_);
    checkCancellation(cancellation);
    if (undoJournal_.empty()) {
        throw CommandError("nothingToUndo", "there is no Windows project action to undo");
    }
    if (revision_ >= maximumRevision) {
        throw CommandError("revisionOverflow", "project revision cannot advance");
    }
    const auto& pending = undoJournal_.back();
    for (const auto& snapshot : pending.tracks) {
        checkCancellation(cancellation);
        if (
            snapshot.timelineIndex >= project_.timelines.size()
            || snapshot.trackIndex >= project_.timelines[snapshot.timelineIndex].tracks.size()
        ) {
            throw CommandError("staleUndo", "the project structure no longer matches the undo action");
        }
    }
    const auto revisionBefore = revision_;
    const auto revisionAfter = revisionBefore + 1;
    auto plannedGeneratedClipIds = sessionGeneratedClipIds_;
    for (const auto& clipId : pending.createdClipIds) {
        plannedGeneratedClipIds.erase(clipId);
    }
    auto payload = receiptBase(true, revisionBefore, revisionAfter, pending.actionId);
    payload.emplace("clips", Value(Array{}));
    payload["notes"] = Value(Array{Value("Re-read get_timeline after undo.")});
    CommandResult result{
        true,
        revisionBefore,
        revisionAfter,
        pending.actionId,
        Value(std::move(payload)),
    };
    checkCancellation(cancellation);

    auto entry = std::move(undoJournal_.back());
    undoJournal_.pop_back();
    static_assert(std::is_nothrow_move_assignable_v<std::vector<Clip>>);
    for (auto& snapshot : entry.tracks) {
        project_.timelines[snapshot.timelineIndex].tracks[snapshot.trackIndex].clips =
            std::move(snapshot.clips);
    }
    sessionGeneratedClipIds_.swap(plannedGeneratedClipIds);
    revision_ = revisionAfter;
    dirty_ = !undoJournal_.empty();
    return result;
}

std::uint64_t ProjectSession::revision() const {
    std::scoped_lock lock(mutex_);
    return revision_;
}

bool ProjectSession::dirty() const {
    std::scoped_lock lock(mutex_);
    return dirty_;
}

}
