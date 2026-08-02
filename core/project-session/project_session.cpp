#include "palmier/project/project_session.hpp"

#include <algorithm>
#include <array>
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

Object& requireSourceObject(Value& value, std::string_view label) {
    if (value.kind() != Value::Kind::object) {
        throw CommandError("sourceModelMismatch", std::string(label) + " is not an object");
    }
    return value.object();
}

const Object& requireSourceObject(const Value& value, std::string_view label) {
    if (value.kind() != Value::Kind::object) {
        throw CommandError("sourceModelMismatch", std::string(label) + " is not an object");
    }
    return value.object();
}

Array& requireSourceArray(Object& object, std::string_view key) {
    const auto field = object.find(std::string(key));
    if (field == object.end() || field->second.kind() != Value::Kind::array) {
        throw CommandError(
            "sourceModelMismatch",
            "project source is missing array field: " + std::string(key)
        );
    }
    return field->second.array();
}

Value& uniqueSourceEntity(Array& values, std::string_view id, std::string_view label) {
    Value* found = nullptr;
    for (auto& value : values) {
        auto& object = requireSourceObject(value, label);
        const auto idField = object.find("id");
        if (
            idField == object.end()
            || idField->second.kind() != Value::Kind::string
            || idField->second.string() != id
        ) {
            continue;
        }
        if (found != nullptr) {
            throw CommandError(
                "ambiguousSourceId",
                std::string(label) + " ID is duplicated in project source: " + std::string(id)
            );
        }
        found = &value;
    }
    if (found == nullptr) {
        throw CommandError(
            "sourceModelMismatch",
            std::string(label) + " is missing from project source: " + std::string(id)
        );
    }
    return *found;
}

Array& sourceClipsForTrack(
    Value& source,
    RootKind rootKind,
    std::string_view timelineId,
    std::string_view trackId
) {
    if (rootKind != RootKind::current) {
        throw CommandError(
            "unsupportedProjectWriteRoot",
            "the Windows mutation slice does not write legacy project roots"
        );
    }
    auto& root = requireSourceObject(source, "project root");
    auto& timelines = requireSourceArray(root, "timelines");
    auto& timeline = requireSourceObject(
        uniqueSourceEntity(timelines, timelineId, "timeline"),
        "timeline"
    );
    auto& tracks = requireSourceArray(timeline, "tracks");
    auto& track = requireSourceObject(
        uniqueSourceEntity(tracks, trackId, "track"),
        "track"
    );
    return requireSourceArray(track, "clips");
}

Value& sourceTimelineFor(
    Value& source,
    RootKind rootKind,
    std::string_view timelineId
) {
    if (rootKind != RootKind::current) {
        throw CommandError(
            "unsupportedProjectWriteRoot",
            "the Windows mutation slice does not write legacy project roots"
        );
    }
    auto& root = requireSourceObject(source, "project root");
    auto& timelines = requireSourceArray(root, "timelines");
    return uniqueSourceEntity(timelines, timelineId, "timeline");
}

Array& sourceTracksForTimeline(
    Value& source,
    RootKind rootKind,
    std::string_view timelineId
) {
    auto& timeline = requireSourceObject(
        sourceTimelineFor(source, rootKind, timelineId),
        "timeline"
    );
    return requireSourceArray(timeline, "tracks");
}

struct TimelineSourceIndex final {
    Value timeline;
    std::map<std::string, Value, std::less<>> tracks;
    std::map<std::string, Value, std::less<>> clips;
    std::map<std::string, std::size_t, std::less<>> clipCounts;
};

TimelineSourceIndex indexTimelineSource(
    Value& source,
    RootKind rootKind,
    std::string_view timelineId
) {
    TimelineSourceIndex result{
        sourceTimelineFor(source, rootKind, timelineId),
        {},
        {},
        {},
    };
    const auto& sourceTracks = sourceTracksForTimeline(source, rootKind, timelineId);
    for (const auto& sourceTrack : sourceTracks) {
        const auto& trackObject = requireSourceObject(sourceTrack, "track");
        const auto trackId = trackObject.find("id");
        if (trackId == trackObject.end() || trackId->second.kind() != Value::Kind::string) {
            throw CommandError("sourceModelMismatch", "source track is missing a stable ID");
        }
        if (!result.tracks.emplace(trackId->second.string(), sourceTrack).second) {
            throw CommandError("ambiguousSourceId", "track ID is duplicated in project source");
        }
        const auto clips = trackObject.find("clips");
        if (clips == trackObject.end()) {
            result.clipCounts.emplace(trackId->second.string(), 0);
            continue;
        }
        if (clips->second.kind() != Value::Kind::array) {
            throw CommandError("sourceModelMismatch", "source track clips is not an array");
        }
        result.clipCounts.emplace(trackId->second.string(), clips->second.array().size());
        for (const auto& sourceClip : clips->second.array()) {
            if (sourceClip.kind() != Value::Kind::object) continue;
            const auto id = sourceClip.object().find("id");
            if (id == sourceClip.object().end() || id->second.kind() != Value::Kind::string) {
                continue;
            }
            if (!result.clips.emplace(id->second.string(), sourceClip).second) {
                throw CommandError("ambiguousSourceId", "clip ID is duplicated in project source");
            }
        }
    }
    return result;
}

void requireSourceTracksMatch(
    const Timeline& timeline,
    const std::set<std::string, std::less<>>& trackIds,
    const TimelineSourceIndex& sourceIndex
) {
    for (const auto& track : timeline.tracks) {
        if (!trackIds.contains(track.id.value)) continue;
        const auto count = sourceIndex.clipCounts.find(track.id.value);
        if (
            !sourceIndex.tracks.contains(track.id.value)
            || count == sourceIndex.clipCounts.end()
            || count->second != track.clips.size()
        ) {
            throw CommandError(
                "sourceModelMismatch",
                "affected track source does not match the Windows clip model"
            );
        }
        for (const auto& clip : track.clips) {
            if (!sourceIndex.clips.contains(clip.id.value)) {
                throw CommandError(
                    "sourceModelMismatch",
                    "affected clip is missing from project source: " + clip.id.value
                );
            }
        }
    }
}

bool trackTypesCompatible(std::string_view source, std::string_view destination) {
    const bool sourceIsVisual = source != "audio";
    const bool destinationIsVisual = destination != "audio";
    return source == destination || (sourceIsVisual && destinationIsVisual);
}

std::int64_t translatedFrame(std::int64_t frame, std::int64_t delta) {
    if (frame < 0) {
        throw CommandError("unsafeFrameRange", "clip start frame is negative");
    }
    if (delta >= 0) {
        if (delta > std::numeric_limits<std::int64_t>::max() - frame) {
            throw CommandError("unsafeTiming", "linked clip move exceeds the integer frame domain");
        }
        return frame + delta;
    }
    const auto magnitude = static_cast<std::uint64_t>(-(delta + 1)) + 1;
    if (magnitude > static_cast<std::uint64_t>(frame)) {
        throw CommandError("invalidMoveFrame", "linked clip move would start before frame zero");
    }
    return frame - static_cast<std::int64_t>(magnitude);
}

Value sourceClipPiece(const Value& original, const Clip& piece) {
    if (original.kind() != Value::Kind::object) {
        throw CommandError("sourceModelMismatch", "source clip is not an object");
    }
    auto object = original.object();
    object["id"] = Value(piece.id.value);
    object["startFrame"] = Value(integerNumber(piece.startFrame));
    object["durationFrames"] = Value(integerNumber(piece.durationFrames));
    object["trimStartFrame"] = Value(integerNumber(piece.trimStartFrame));
    object["trimEndFrame"] = Value(integerNumber(piece.trimEndFrame));
    if (piece.linkGroupId) {
        object["linkGroupId"] = Value(*piece.linkGroupId);
    } else {
        object.erase("linkGroupId");
    }
    return Value(std::move(object));
}

Array splitSourceClips(
    const Array& sourceClips,
    const std::map<std::string, std::vector<Clip>, std::less<>>& piecesByClip
) {
    std::set<std::string, std::less<>> replaced;
    Array result;
    result.reserve(sourceClips.size() + piecesByClip.size());
    for (const auto& sourceClip : sourceClips) {
        if (sourceClip.kind() != Value::Kind::object) {
            result.push_back(sourceClip);
            continue;
        }
        const auto id = sourceClip.object().find("id");
        if (id == sourceClip.object().end() || id->second.kind() != Value::Kind::string) {
            result.push_back(sourceClip);
            continue;
        }
        const auto pieces = piecesByClip.find(id->second.string());
        if (pieces == piecesByClip.end()) {
            result.push_back(sourceClip);
            continue;
        }
        if (!replaced.insert(id->second.string()).second) {
            throw CommandError(
                "ambiguousSourceId",
                "clip ID is duplicated in project source: " + id->second.string()
            );
        }
        for (const auto& piece : pieces->second) {
            result.push_back(sourceClipPiece(sourceClip, piece));
        }
    }
    if (replaced.size() != piecesByClip.size()) {
        throw CommandError("sourceModelMismatch", "a split clip is missing from project source");
    }
    return result;
}

std::int64_t roundedFrame(double value, std::string_view label) {
    const auto maximum = std::nextafter(
        static_cast<double>(std::numeric_limits<std::int64_t>::max()),
        0.0
    );
    if (!std::isfinite(value) || value < 0.0 || value > maximum) {
        throw CommandError("unsafeTiming", std::string(label) + " exceeds the integer frame domain");
    }
    return static_cast<std::int64_t>(std::llround(value));
}

std::int64_t optionalSourceInteger(
    const Object& object,
    std::string_view key,
    std::int64_t fallback
) {
    const auto field = object.find(std::string(key));
    if (field == object.end()) return fallback;
    if (field->second.kind() != Value::Kind::number || !field->second.number().integer) {
        throw CommandError(
            "unsafeDurationSemantics",
            "clip " + std::string(key) + " is not an integer"
        );
    }
    return *field->second.number().integer;
}

void clampSourceKeyframeTrack(Object& clip, std::string_view key, std::int64_t duration) {
    const auto field = clip.find(std::string(key));
    if (field == clip.end() || field->second.kind() == Value::Kind::nullValue) return;
    if (field->second.kind() != Value::Kind::object) {
        throw CommandError(
            "unsafeDurationSemantics",
            "clip " + std::string(key) + " is not a keyframe track"
        );
    }
    auto track = field->second.object();
    const auto keyframes = track.find("keyframes");
    if (keyframes == track.end() || keyframes->second.kind() != Value::Kind::array) {
        throw CommandError(
            "unsafeDurationSemantics",
            "clip " + std::string(key) + " has no keyframes array"
        );
    }

    std::map<std::int64_t, Object> normalized;
    for (const auto& keyframe : keyframes->second.array()) {
        if (keyframe.kind() != Value::Kind::object) {
            throw CommandError(
                "unsafeDurationSemantics",
                "clip " + std::string(key) + " contains a malformed keyframe"
            );
        }
        const auto& object = keyframe.object();
        const auto frame = object.find("frame");
        if (
            frame == object.end()
            || frame->second.kind() != Value::Kind::number
            || !frame->second.number().integer
        ) {
            throw CommandError(
                "unsafeDurationSemantics",
                "clip " + std::string(key) + " contains a non-integer frame"
            );
        }
        const auto offset = *frame->second.number().integer;
        if (offset < 0 || offset > duration) continue;
        auto [entry, inserted] = normalized.emplace(offset, object);
        if (!inserted) {
            for (const auto& [name, value] : object) entry->second.insert_or_assign(name, value);
        }
    }

    if (normalized.empty()) {
        clip.erase(std::string(key));
        return;
    }
    Array values;
    values.reserve(normalized.size());
    for (auto& [frame, object] : normalized) {
        static_cast<void>(frame);
        values.emplace_back(std::move(object));
    }
    track["keyframes"] = Value(std::move(values));
    clip[std::string(key)] = Value(std::move(track));
}

void rescaleSourceWordTimings(
    Object& clip,
    std::int64_t oldDuration,
    std::int64_t newDuration
) {
    const auto field = clip.find("wordTimings");
    if (field == clip.end() || field->second.kind() == Value::Kind::nullValue) return;
    if (field->second.kind() != Value::Kind::array) {
        throw CommandError("unsafeDurationSemantics", "clip wordTimings is not an array");
    }
    const auto scale = static_cast<double>(newDuration) / static_cast<double>(oldDuration);
    Array timings;
    timings.reserve(field->second.array().size());
    for (const auto& timing : field->second.array()) {
        if (timing.kind() != Value::Kind::object) {
            throw CommandError("unsafeDurationSemantics", "clip contains a malformed word timing");
        }
        auto object = timing.object();
        const auto start = optionalSourceInteger(object, "startFrame", -1);
        const auto end = optionalSourceInteger(object, "endFrame", -1);
        if (start < 0 || end < 0) {
            throw CommandError("unsafeDurationSemantics", "clip word timing has an invalid frame");
        }
        const auto scaledStart = std::min(
            roundedFrame(static_cast<double>(start) * scale, "word timing start"),
            newDuration - 1
        );
        const auto minimumEnd = scaledStart + 1;
        const auto scaledEnd = std::min(
            std::max(
                minimumEnd,
                roundedFrame(static_cast<double>(end) * scale, "word timing end")
            ),
            newDuration
        );
        object["startFrame"] = Value(integerNumber(scaledStart));
        object["endFrame"] = Value(integerNumber(scaledEnd));
        timings.emplace_back(std::move(object));
    }
    clip["wordTimings"] = Value(std::move(timings));
}

void applyDurationSourceSemantics(
    Object& sourceClip,
    const Clip& clip,
    std::int64_t duration
) {
    const auto fadeIn = optionalSourceInteger(sourceClip, "fadeInFrames", 0);
    const auto fadeOut = optionalSourceInteger(sourceClip, "fadeOutFrames", 0);
    if (fadeIn < 0 || fadeOut < 0) {
        throw CommandError("unsafeDurationSemantics", "clip fade duration is negative");
    }
    const auto clampedFadeIn = std::min(fadeIn, duration);
    const auto clampedFadeOut = std::min(fadeOut, duration - clampedFadeIn);
    if (sourceClip.contains("fadeInFrames") || clampedFadeIn != 0) {
        sourceClip["fadeInFrames"] = Value(integerNumber(clampedFadeIn));
    }
    if (sourceClip.contains("fadeOutFrames") || clampedFadeOut != 0) {
        sourceClip["fadeOutFrames"] = Value(integerNumber(clampedFadeOut));
    }

    static constexpr std::array keyframeTracks{
        "opacityTrack",
        "positionTrack",
        "scaleTrack",
        "rotationTrack",
        "cropTrack",
        "volumeTrack",
    };
    for (const auto key : keyframeTracks) clampSourceKeyframeTrack(sourceClip, key, duration);
    if (clip.mediaType == "text") {
        rescaleSourceWordTimings(sourceClip, clip.durationFrames, duration);
    }
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

ProjectSession::UndoEntry::UndoEntry(
    std::string actionIdValue,
    std::vector<TrackSnapshot> trackSnapshots,
    std::unique_ptr<TimelineSnapshot> timelineSnapshot,
    std::vector<std::string> createdIds,
    std::uint64_t stateId
) : actionId(std::move(actionIdValue)),
    tracks(std::move(trackSnapshots)),
    timeline(std::move(timelineSnapshot)),
    createdClipIds(std::move(createdIds)),
    beforeStateId(stateId) {}

ProjectSession::UndoEntry::UndoEntry(const UndoEntry& other)
    : actionId(other.actionId),
      tracks(other.tracks),
      timeline(other.timeline
          ? std::make_unique<TimelineSnapshot>(*other.timeline)
          : nullptr),
      createdClipIds(other.createdClipIds),
      beforeStateId(other.beforeStateId) {}

ProjectSession::ProjectSession(
    const ProjectDocument& document,
    IdGenerator idGenerator,
    ProjectSessionPublicationFactory publicationFactory
)
    : source_(std::make_unique<Value>(document.source())),
      rootKind_(document.rootKind()),
      project_(document.project()),
      diagnostics_(document.diagnostics()),
      idGenerator_(std::move(idGenerator)),
      publicationFactory_(std::move(publicationFactory)),
      unsafeClipIds_(unsafeClipIds(document)) {
    if (!idGenerator_) {
        throw CommandError("invalidIdGenerator", "project session requires an ID generator");
    }
}

std::shared_ptr<const ProjectSessionSnapshot> ProjectSession::preparePublication(
    ProjectSessionSnapshot snapshot
) const {
    auto publication = publicationFactory_
        ? publicationFactory_(std::move(snapshot))
        : std::make_shared<const ProjectSessionSnapshot>(std::move(snapshot));
    if (!publication) {
        throw CommandError(
            "invalidPublicationFactory",
            "project session publication factory returned no snapshot"
        );
    }
    return publication;
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

    if (rootKind_ != RootKind::current) {
        throw CommandError(
            "unsupportedProjectWriteRoot",
            "the Windows mutation slice does not write legacy project roots"
        );
    }
    if (timeline.id.origin != EntityIdOrigin::persisted) {
        throw CommandError(
            "unstableTimelineId",
            "split requires a persisted active timeline ID"
        );
    }
    for (const auto trackIndex : affectedTracks) {
        if (timeline.tracks[trackIndex].id.origin != EntityIdOrigin::persisted) {
            throw CommandError(
                "unstableTrackId",
                "split requires a persisted affected track ID"
            );
        }
    }

    auto plannedSource = std::make_unique<Value>(*source_);
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
    std::map<
        std::size_t,
        std::map<std::string, std::vector<Clip>, std::less<>>
    > sourcePiecesByTrack;
    const auto timelineIndex = static_cast<std::size_t>(
        std::distance(project_.timelines.begin(), std::find_if(
            project_.timelines.begin(),
            project_.timelines.end(),
            [&](const Timeline& entry) { return entry.id.value == project_.activeTimelineId; }
        ))
    );
    for (const auto trackIndex : affectedTracks) {
        checkCancellation(cancellation);
        const auto& track = timeline.tracks[trackIndex];
        auto& sourceClips = sourceClipsForTrack(
            *source_,
            rootKind_,
            timeline.id.value,
            track.id.value
        );
        snapshots.push_back({
            timelineIndex,
            trackIndex,
            track.clips,
            Value(sourceClips),
        });
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
            std::vector<Clip> sourcePieces;
            sourcePieces.reserve(boundaries.size() - 1);
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
                sourcePieces.push_back(piece);
                replacement.push_back(std::move(piece));
            }
            sourcePiecesByTrack[trackIndex].emplace(
                clip.id.value,
                std::move(sourcePieces)
            );
        }
        clips = std::move(replacement);
    }
    for (const auto trackIndex : affectedTracks) {
        const auto& track = timeline.tracks[trackIndex];
        auto& sourceClips = sourceClipsForTrack(
            *plannedSource,
            rootKind_,
            timeline.id.value,
            track.id.value
        );
        sourceClips = splitSourceClips(sourceClips, sourcePiecesByTrack.at(trackIndex));
    }
    checkCancellation(cancellation);

    const auto revisionBefore = revision_;
    if (revision_ >= maximumRevision) {
        throw CommandError("revisionOverflow", "project revision cannot advance");
    }
    if (nextStateId_ == (std::numeric_limits<std::uint64_t>::max)()) {
        throw CommandError("stateIdentityOverflow", "project state identity cannot advance");
    }
    const auto revisionAfter = revisionBefore + 1;
    const auto stateAfter = nextStateId_;
    auto plannedGeneratedClipIds = sessionGeneratedClipIds_;
    plannedGeneratedClipIds.insert(createdClipIds.begin(), createdClipIds.end());
    undoJournal_.reserve(undoJournal_.size() + 1);
    UndoEntry undoEntry{
        actionId,
        std::move(snapshots),
        nullptr,
        std::move(createdClipIds),
        stateId_,
    };
    auto payload = receiptBase(true, revisionBefore, revisionAfter, actionId);
    payload.emplace("clips", Value(std::move(changedClips)));
    auto publication = preparePublication({
        ProjectDocument(*plannedSource, rootKind_, planned, diagnostics_),
        revisionAfter,
        stateAfter,
        persistedStateId_,
        undoJournal_.size() + 1,
        0,
    });
    CommandResult result{
        true,
        revisionBefore,
        revisionAfter,
        actionId,
        std::make_unique<Value>(std::move(payload)),
        std::move(publication),
    };
    checkCancellation(cancellation);

    static_assert(std::is_nothrow_move_assignable_v<Project>);
    static_assert(std::is_nothrow_move_constructible_v<UndoEntry>);
    static_assert(std::is_nothrow_move_constructible_v<CommandResult>);
    static_assert(noexcept(sessionGeneratedClipIds_.swap(plannedGeneratedClipIds)));
    static_assert(noexcept(source_.swap(plannedSource)));
    project_ = std::move(planned);
    source_.swap(plannedSource);
    sessionGeneratedClipIds_.swap(plannedGeneratedClipIds);
    revision_ = revisionAfter;
    stateId_ = stateAfter;
    ++nextStateId_;
    undoJournal_.push_back(std::move(undoEntry));
    redoJournal_.clear();
    return result;
}

CommandResult ProjectSession::moveClips(
    const MoveClipsCommand& command,
    std::stop_token cancellation
) {
    std::scoped_lock lock(mutex_);
    checkCancellation(cancellation);
    if (command.moves.empty()) {
        throw CommandError("invalidMoves", "moves must be a non-empty array");
    }

    auto& timeline = activeTimeline(project_);
    if (rootKind_ != RootKind::current) {
        throw CommandError(
            "unsupportedProjectWriteRoot",
            "the Windows mutation slice does not write legacy project roots"
        );
    }
    if (timeline.id.origin != EntityIdOrigin::persisted) {
        throw CommandError("unstableTimelineId", "move requires a persisted active timeline ID");
    }

    struct ResolvedMove final {
        Clip clip;
        std::string sourceTrackId;
        std::string destinationTrackId;
        std::int64_t destinationFrame;
    };
    std::map<std::string, ResolvedMove, std::less<>> resolved;
    std::map<std::string, std::int64_t, std::less<>> linkedDeltas;
    for (const auto& request : command.moves) {
        checkCancellation(cancellation);
        if (request.clipId.empty()) {
            throw CommandError("invalidClipId", "move clipId must not be empty");
        }
        if (!request.toTrack && !request.toFrame) {
            throw CommandError(
                "invalidMoveDestination",
                "each move requires toTrack or toFrame"
            );
        }
        if (resolved.contains(request.clipId)) {
            throw CommandError("duplicateMove", "clip appears more than once: " + request.clipId);
        }
        const auto location = uniqueClipLocation(timeline, request.clipId);
        const auto& sourceTrack = timeline.tracks[location.trackIndex];
        const auto& clip = sourceTrack.clips[location.clipIndex];
        if (clip.startFrame < 0) {
            throw CommandError("unsafeFrameRange", "clip start frame is negative");
        }
        const auto destinationTrackIndex = request.toTrack.value_or(location.trackIndex);
        if (destinationTrackIndex >= timeline.tracks.size()) {
            throw CommandError("trackNotFound", "toTrack is outside the active timeline");
        }
        const auto& destinationTrack = timeline.tracks[destinationTrackIndex];
        if (!trackTypesCompatible(sourceTrack.type, destinationTrack.type)) {
            throw CommandError(
                "incompatibleTrackType",
                "destination track is incompatible with clip " + request.clipId
            );
        }
        const auto destinationFrame = request.toFrame.value_or(clip.startFrame);
        if (destinationFrame < 0) {
            throw CommandError("invalidMoveFrame", "toFrame must not be negative");
        }
        Clip moved = clip;
        moved.startFrame = destinationFrame;
        static_cast<void>(checkedEnd(moved));
        resolved.emplace(request.clipId, ResolvedMove{
            clip,
            sourceTrack.id.value,
            destinationTrack.id.value,
            destinationFrame,
        });
        if (request.toFrame && clip.linkGroupId) {
            if (clip.linkGroupId->empty()) {
                throw CommandError("invalidLinkGroupId", "linkGroupId must not be empty");
            }
            const auto delta = destinationFrame - clip.startFrame;
            const auto [entry, inserted] = linkedDeltas.emplace(*clip.linkGroupId, delta);
            if (!inserted && entry->second != delta) {
                throw CommandError(
                    "linkedMoveMismatch",
                    "linked clips must move by the same frame delta"
                );
            }
        }
    }

    for (std::size_t trackIndex = 0; trackIndex < timeline.tracks.size(); ++trackIndex) {
        const auto& track = timeline.tracks[trackIndex];
        for (const auto& clip : track.clips) {
            checkCancellation(cancellation);
            if (!clip.linkGroupId) continue;
            const auto delta = linkedDeltas.find(*clip.linkGroupId);
            if (delta == linkedDeltas.end()) continue;
            const auto destinationFrame = translatedFrame(clip.startFrame, delta->second);
            auto existing = resolved.find(clip.id.value);
            if (existing != resolved.end()) {
                if (existing->second.destinationFrame != destinationFrame) {
                    throw CommandError(
                        "linkedMoveMismatch",
                        "linked clips must preserve their frame offsets"
                    );
                }
                continue;
            }
            Clip moved = clip;
            moved.startFrame = destinationFrame;
            static_cast<void>(checkedEnd(moved));
            resolved.emplace(clip.id.value, ResolvedMove{
                clip,
                track.id.value,
                track.id.value,
                destinationFrame,
            });
        }
    }

    bool changed{};
    for (const auto& [clipId, move] : resolved) {
        static_cast<void>(clipId);
        changed = changed
            || move.sourceTrackId != move.destinationTrackId
            || move.clip.startFrame != move.destinationFrame;
    }
    if (!changed) {
        Array clips;
        for (const auto& [clipId, move] : resolved) {
            static_cast<void>(move);
            const auto location = uniqueClipLocation(timeline, clipId);
            clips.push_back(clipValue(
                timeline.tracks[location.trackIndex].clips[location.clipIndex],
                location.trackIndex
            ));
        }
        auto payload = receiptBase(false, revision_, revision_, "");
        payload.emplace("clips", Value(std::move(clips)));
        auto publication = preparePublication({
            ProjectDocument(*source_, rootKind_, project_, diagnostics_),
            revision_,
            stateId_,
            persistedStateId_,
            undoJournal_.size(),
            redoJournal_.size(),
        });
        checkCancellation(cancellation);
        return CommandResult{
            false,
            revision_,
            revision_,
            {},
            std::make_unique<Value>(std::move(payload)),
            std::move(publication),
        };
    }

    std::set<std::string, std::less<>> changedTrackIds;
    for (const auto& [clipId, move] : resolved) {
        if (
            move.clip.id.origin != EntityIdOrigin::persisted
            && !sessionGeneratedClipIds_.contains(clipId)
        ) {
            throw CommandError("unstableClipId", "move requires a stable clip ID: " + clipId);
        }
        if (std::binary_search(unsafeClipIds_.begin(), unsafeClipIds_.end(), clipId)) {
            throw CommandError(
                "unsupportedClipSemantics",
                "clip has fields not represented by the Windows move model: " + clipId
            );
        }
        if (move.clip.captionGroupId || move.clip.multicamGroupId) {
            throw CommandError(
                "unsupportedClipSemantics",
                "caption and multicam clips are not supported by the Windows move slice"
            );
        }
        changedTrackIds.insert(move.sourceTrackId);
        changedTrackIds.insert(move.destinationTrackId);
    }
    for (const auto& track : timeline.tracks) {
        if (
            changedTrackIds.contains(track.id.value)
            && track.id.origin != EntityIdOrigin::persisted
        ) {
            throw CommandError("unstableTrackId", "move requires persisted affected track IDs");
        }
    }

    const auto timelineIndex = static_cast<std::size_t>(
        std::distance(project_.timelines.begin(), std::find_if(
            project_.timelines.begin(),
            project_.timelines.end(),
            [&](const Timeline& entry) { return entry.id.value == project_.activeTimelineId; }
        ))
    );
    auto sourceIndex = indexTimelineSource(*source_, rootKind_, timeline.id.value);
    const Value sourceTimelineSnapshot = sourceIndex.timeline;
    requireSourceTracksMatch(timeline, changedTrackIds, sourceIndex);
    auto& sourceTrackValues = sourceIndex.tracks;
    auto sourceClipValues = std::move(sourceIndex.clips);

    if (revision_ >= maximumRevision) {
        throw CommandError("revisionOverflow", "project revision cannot advance");
    }
    if (nextStateId_ == (std::numeric_limits<std::uint64_t>::max)()) {
        throw CommandError("stateIdentityOverflow", "project state identity cannot advance");
    }
    std::set<std::string, std::less<>> usedIds;
    for (const auto& projectTimeline : project_.timelines) {
        usedIds.insert(projectTimeline.id.value);
        for (const auto& track : projectTimeline.tracks) {
            usedIds.insert(track.id.value);
            for (const auto& clip : track.clips) {
                usedIds.insert(clip.id.value);
                if (clip.linkGroupId) usedIds.insert(*clip.linkGroupId);
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
    struct FrameRange final {
        std::int64_t start;
        std::int64_t end;
    };
    Project planned = project_;
    auto& plannedTimeline = activeTimeline(planned);
    for (auto& track : plannedTimeline.tracks) {
        track.clips.erase(
            std::remove_if(
                track.clips.begin(),
                track.clips.end(),
                [&](const Clip& clip) { return resolved.contains(clip.id.value); }
            ),
            track.clips.end()
        );
    }
    std::map<std::string, std::vector<FrameRange>, std::less<>> destinationRanges;
    for (const auto& [clipId, move] : resolved) {
        static_cast<void>(clipId);
        Clip moved = move.clip;
        moved.startFrame = move.destinationFrame;
        destinationRanges[move.destinationTrackId].push_back({
            moved.startFrame,
            checkedEnd(moved),
        });
    }
    for (auto& [trackId, ranges] : destinationRanges) {
        static_cast<void>(trackId);
        std::sort(ranges.begin(), ranges.end(), [](const FrameRange& left, const FrameRange& right) {
            return left.start < right.start || (left.start == right.start && left.end < right.end);
        });
        for (std::size_t index = 1; index < ranges.size(); ++index) {
            if (ranges[index].start < ranges[index - 1].end) {
                throw CommandError("overlappingMoves", "moved clips overlap on a destination track");
            }
        }
    }
    const auto actionId = newId();

    std::vector<std::string> createdClipIds;
    std::set<std::string, std::less<>> touchedClipIds;
    std::set<std::string, std::less<>> removedClipIds;
    for (auto& track : plannedTimeline.tracks) {
        const auto ranges = destinationRanges.find(track.id.value);
        if (ranges == destinationRanges.end()) continue;
        std::vector<Clip> replacement;
        for (const auto& clip : track.clips) {
            checkCancellation(cancellation);
            const auto clipEnd = checkedEnd(clip);
            std::vector<FrameRange> survivors;
            std::int64_t cursor = clip.startFrame;
            bool overlaps{};
            for (const auto& range : ranges->second) {
                if (range.end <= cursor || range.start >= clipEnd) continue;
                overlaps = true;
                if (range.start > cursor) {
                    survivors.push_back({cursor, std::min(range.start, clipEnd)});
                }
                cursor = std::max(cursor, range.end);
                if (cursor >= clipEnd) break;
            }
            if (!overlaps) {
                replacement.push_back(clip);
                continue;
            }
            changedTrackIds.insert(track.id.value);
            if (
                std::binary_search(unsafeClipIds_.begin(), unsafeClipIds_.end(), clip.id.value)
                || clip.captionGroupId
                || clip.multicamGroupId
            ) {
                throw CommandError(
                    "unsupportedClipSemantics",
                    "destination overlap touches unsupported clip semantics: " + clip.id.value
                );
            }
            if (clip.linkGroupId) {
                throw CommandError(
                    "unsupportedLinkedOverwrite",
                    "destination overlap would change a linked clip: " + clip.id.value
                );
            }
            if (cursor < clipEnd) survivors.push_back({cursor, clipEnd});
            const auto sourceOriginal = sourceClipValues.find(clip.id.value);
            if (sourceOriginal == sourceClipValues.end()) {
                throw CommandError(
                    "sourceModelMismatch",
                    "overwritten clip is missing from project source: " + clip.id.value
                );
            }
            const Value originalSource = sourceOriginal->second;
            sourceClipValues.erase(sourceOriginal);
            if (survivors.empty()) {
                removedClipIds.insert(clip.id.value);
                continue;
            }
            for (std::size_t index = 0; index < survivors.size(); ++index) {
                Clip piece = clip;
                if (index > 0) {
                    piece.id = {newId(), EntityIdOrigin::synthesized};
                    createdClipIds.push_back(piece.id.value);
                }
                piece.startFrame = survivors[index].start;
                piece.durationFrames = survivors[index].end - survivors[index].start;
                piece.trimStartFrame = checkedFrameAdd(
                    clip.trimStartFrame,
                    scaledFrameOffset(piece.startFrame - clip.startFrame, clip.speed)
                );
                piece.trimEndFrame = checkedFrameAdd(
                    clip.trimEndFrame,
                    scaledFrameOffset(clipEnd - survivors[index].end, clip.speed)
                );
                sourceClipValues.emplace(piece.id.value, sourceClipPiece(originalSource, piece));
                touchedClipIds.insert(piece.id.value);
                replacement.push_back(std::move(piece));
            }
        }
        track.clips = std::move(replacement);
    }

    for (const auto& [clipId, move] : resolved) {
        auto destination = std::find_if(
            plannedTimeline.tracks.begin(),
            plannedTimeline.tracks.end(),
            [&](const Track& track) { return track.id.value == move.destinationTrackId; }
        );
        if (destination == plannedTimeline.tracks.end()) {
            throw CommandError("staleMoveDestination", "destination track disappeared while planning move");
        }
        Clip moved = move.clip;
        moved.startFrame = move.destinationFrame;
        destination->clips.push_back(moved);
        const Value originalSource = sourceClipValues.at(clipId);
        sourceClipValues.insert_or_assign(clipId, sourceClipPiece(originalSource, moved));
        touchedClipIds.insert(clipId);
    }
    for (auto& track : plannedTimeline.tracks) {
        std::sort(track.clips.begin(), track.clips.end(), [](const Clip& left, const Clip& right) {
            return left.startFrame < right.startFrame
                || (left.startFrame == right.startFrame && left.id.value < right.id.value);
        });
    }
    std::set<std::string, std::less<>> originallyNonemptyTracks;
    for (const auto& track : timeline.tracks) {
        if (!track.clips.empty()) originallyNonemptyTracks.insert(track.id.value);
    }
    plannedTimeline.tracks.erase(
        std::remove_if(
            plannedTimeline.tracks.begin(),
            plannedTimeline.tracks.end(),
            [&](const Track& track) {
                return track.clips.empty() && originallyNonemptyTracks.contains(track.id.value);
            }
        ),
        plannedTimeline.tracks.end()
    );

    auto plannedSource = std::make_unique<Value>(*source_);
    Array plannedSourceTracks;
    plannedSourceTracks.reserve(plannedTimeline.tracks.size());
    for (const auto& track : plannedTimeline.tracks) {
        const auto existing = sourceTrackValues.find(track.id.value);
        if (existing == sourceTrackValues.end()) {
            throw CommandError(
                "sourceModelMismatch",
                "active track is missing from project source: " + track.id.value
            );
        }
        Value sourceTrack = existing->second;
        if (changedTrackIds.contains(track.id.value)) {
            Array sourceClips;
            sourceClips.reserve(track.clips.size());
            for (const auto& clip : track.clips) {
                const auto sourceClip = sourceClipValues.find(clip.id.value);
                if (sourceClip == sourceClipValues.end()) {
                    throw CommandError(
                        "sourceModelMismatch",
                        "planned clip is missing from project source: " + clip.id.value
                    );
                }
                sourceClips.push_back(sourceClip->second);
            }
            requireSourceObject(sourceTrack, "track")["clips"] = Value(std::move(sourceClips));
        }
        plannedSourceTracks.push_back(std::move(sourceTrack));
    }
    sourceTracksForTimeline(*plannedSource, rootKind_, timeline.id.value) =
        std::move(plannedSourceTracks);
    checkCancellation(cancellation);

    Array changedClips;
    for (std::size_t trackIndex = 0; trackIndex < plannedTimeline.tracks.size(); ++trackIndex) {
        for (const auto& clip : plannedTimeline.tracks[trackIndex].clips) {
            if (touchedClipIds.contains(clip.id.value)) {
                changedClips.push_back(clipValue(clip, trackIndex));
            }
        }
    }
    Array removed;
    for (const auto& id : removedClipIds) removed.emplace_back(id);
    const auto revisionBefore = revision_;
    const auto revisionAfter = revisionBefore + 1;
    const auto stateAfter = nextStateId_;
    auto plannedGeneratedClipIds = sessionGeneratedClipIds_;
    plannedGeneratedClipIds.insert(createdClipIds.begin(), createdClipIds.end());
    undoJournal_.reserve(undoJournal_.size() + 1);
    UndoEntry undoEntry{
        actionId,
        {},
        std::make_unique<TimelineSnapshot>(TimelineSnapshot{
            timelineIndex,
            timeline,
            sourceTimelineSnapshot,
        }),
        std::move(createdClipIds),
        stateId_,
    };
    auto payload = receiptBase(true, revisionBefore, revisionAfter, actionId);
    payload["removedClipIds"] = Value(std::move(removed));
    payload.emplace("clips", Value(std::move(changedClips)));
    auto publication = preparePublication({
        ProjectDocument(*plannedSource, rootKind_, planned, diagnostics_),
        revisionAfter,
        stateAfter,
        persistedStateId_,
        undoJournal_.size() + 1,
        0,
    });
    CommandResult result{
        true,
        revisionBefore,
        revisionAfter,
        actionId,
        std::make_unique<Value>(std::move(payload)),
        std::move(publication),
    };
    checkCancellation(cancellation);

    static_assert(std::is_nothrow_move_assignable_v<Project>);
    static_assert(std::is_nothrow_move_constructible_v<UndoEntry>);
    static_assert(std::is_nothrow_move_constructible_v<CommandResult>);
    static_assert(noexcept(sessionGeneratedClipIds_.swap(plannedGeneratedClipIds)));
    static_assert(noexcept(source_.swap(plannedSource)));
    project_ = std::move(planned);
    source_.swap(plannedSource);
    sessionGeneratedClipIds_.swap(plannedGeneratedClipIds);
    revision_ = revisionAfter;
    stateId_ = stateAfter;
    ++nextStateId_;
    undoJournal_.push_back(std::move(undoEntry));
    redoJournal_.clear();
    return result;
}

CommandResult ProjectSession::removeClips(
    const RemoveClipsCommand& command,
    std::stop_token cancellation
) {
    std::scoped_lock lock(mutex_);
    checkCancellation(cancellation);
    if (command.clipIds.empty()) {
        throw CommandError("invalidClipIds", "clipIds must be a non-empty array");
    }

    auto& timeline = activeTimeline(project_);
    if (rootKind_ != RootKind::current) {
        throw CommandError(
            "unsupportedProjectWriteRoot",
            "the Windows mutation slice does not write legacy project roots"
        );
    }
    if (timeline.id.origin != EntityIdOrigin::persisted) {
        throw CommandError("unstableTimelineId", "remove requires a persisted active timeline ID");
    }

    std::set<std::string, std::less<>> removedIds;
    std::set<std::string, std::less<>> removedLinkGroups;
    for (const auto& clipId : command.clipIds) {
        checkCancellation(cancellation);
        if (clipId.empty()) {
            throw CommandError("invalidClipId", "remove clipId must not be empty");
        }
        const auto location = uniqueClipLocation(timeline, clipId);
        const auto& clip = timeline.tracks[location.trackIndex].clips[location.clipIndex];
        if (
            clip.id.origin != EntityIdOrigin::persisted
            && !sessionGeneratedClipIds_.contains(clipId)
        ) {
            throw CommandError("unstableClipId", "remove requires a stable clip ID: " + clipId);
        }
        removedIds.insert(clipId);
        if (clip.linkGroupId) {
            if (clip.linkGroupId->empty()) {
                throw CommandError("invalidLinkGroupId", "linkGroupId must not be empty");
            }
            removedLinkGroups.insert(*clip.linkGroupId);
        }
    }
    for (const auto& track : timeline.tracks) {
        for (const auto& clip : track.clips) {
            checkCancellation(cancellation);
            if (clip.linkGroupId && removedLinkGroups.contains(*clip.linkGroupId)) {
                if (
                    clip.id.origin != EntityIdOrigin::persisted
                    && !sessionGeneratedClipIds_.contains(clip.id.value)
                ) {
                    throw CommandError(
                        "unstableClipId",
                        "remove requires a stable linked clip ID: " + clip.id.value
                    );
                }
                removedIds.insert(clip.id.value);
            }
        }
    }

    const auto timelineIndex = static_cast<std::size_t>(
        std::distance(project_.timelines.begin(), std::find_if(
            project_.timelines.begin(),
            project_.timelines.end(),
            [&](const Timeline& entry) { return entry.id.value == project_.activeTimelineId; }
        ))
    );
    std::set<std::string, std::less<>> activeTrackIds;
    for (const auto& track : timeline.tracks) {
        if (track.id.origin != EntityIdOrigin::persisted) {
            throw CommandError("unstableTrackId", "remove requires persisted track IDs");
        }
        activeTrackIds.insert(track.id.value);
    }
    auto sourceIndex = indexTimelineSource(*source_, rootKind_, timeline.id.value);
    const Value sourceTimelineSnapshot = sourceIndex.timeline;
    requireSourceTracksMatch(timeline, activeTrackIds, sourceIndex);
    const auto& sourceTrackValues = sourceIndex.tracks;
    const auto& sourceClipValues = sourceIndex.clips;

    if (revision_ >= maximumRevision) {
        throw CommandError("revisionOverflow", "project revision cannot advance");
    }
    if (nextStateId_ == (std::numeric_limits<std::uint64_t>::max)()) {
        throw CommandError("stateIdentityOverflow", "project state identity cannot advance");
    }
    std::set<std::string, std::less<>> usedIds;
    for (const auto& projectTimeline : project_.timelines) {
        usedIds.insert(projectTimeline.id.value);
        for (const auto& track : projectTimeline.tracks) {
            usedIds.insert(track.id.value);
            for (const auto& clip : track.clips) {
                usedIds.insert(clip.id.value);
                if (clip.linkGroupId) usedIds.insert(*clip.linkGroupId);
            }
        }
    }
    const auto actionId = idGenerator_();
    if (actionId.empty() || !usedIds.insert(actionId).second) {
        throw CommandError("invalidGeneratedId", "ID generator returned an empty or duplicate value");
    }

    Project planned = project_;
    auto& plannedTimeline = activeTimeline(planned);
    for (auto& track : plannedTimeline.tracks) {
        track.clips.erase(
            std::remove_if(
                track.clips.begin(),
                track.clips.end(),
                [&](const Clip& clip) { return removedIds.contains(clip.id.value); }
            ),
            track.clips.end()
        );
    }
    const auto trackCountBefore = plannedTimeline.tracks.size();
    plannedTimeline.tracks.erase(
        std::remove_if(
            plannedTimeline.tracks.begin(),
            plannedTimeline.tracks.end(),
            [](const Track& track) { return track.clips.empty(); }
        ),
        plannedTimeline.tracks.end()
    );
    const auto prunedTrackCount = trackCountBefore - plannedTimeline.tracks.size();

    auto plannedSource = std::make_unique<Value>(*source_);
    Array plannedSourceTracks;
    plannedSourceTracks.reserve(plannedTimeline.tracks.size());
    for (const auto& track : plannedTimeline.tracks) {
        Value sourceTrack = sourceTrackValues.at(track.id.value);
        Array sourceClips;
        sourceClips.reserve(track.clips.size());
        for (const auto& clip : track.clips) {
            sourceClips.push_back(sourceClipValues.at(clip.id.value));
        }
        requireSourceObject(sourceTrack, "track")["clips"] = Value(std::move(sourceClips));
        plannedSourceTracks.push_back(std::move(sourceTrack));
    }
    sourceTracksForTimeline(*plannedSource, rootKind_, timeline.id.value) =
        std::move(plannedSourceTracks);
    checkCancellation(cancellation);

    Array removed;
    for (const auto& track : timeline.tracks) {
        for (const auto& clip : track.clips) {
            if (removedIds.contains(clip.id.value)) removed.emplace_back(clip.id.value);
        }
    }
    const auto revisionBefore = revision_;
    const auto revisionAfter = revisionBefore + 1;
    const auto stateAfter = nextStateId_;
    undoJournal_.reserve(undoJournal_.size() + 1);
    UndoEntry undoEntry{
        actionId,
        {},
        std::make_unique<TimelineSnapshot>(TimelineSnapshot{
            timelineIndex,
            timeline,
            sourceTimelineSnapshot,
        }),
        {},
        stateId_,
    };
    auto payload = receiptBase(true, revisionBefore, revisionAfter, actionId);
    payload["removedClipIds"] = Value(std::move(removed));
    payload.emplace("clips", Value(Array{}));
    if (prunedTrackCount > 0) {
        payload["notes"] = Value(Array{Value(
            "Track indices shifted - re-read get_timeline before the next index-based call."
        )});
    }
    auto publication = preparePublication({
        ProjectDocument(*plannedSource, rootKind_, planned, diagnostics_),
        revisionAfter,
        stateAfter,
        persistedStateId_,
        undoJournal_.size() + 1,
        0,
    });
    CommandResult result{
        true,
        revisionBefore,
        revisionAfter,
        actionId,
        std::make_unique<Value>(std::move(payload)),
        std::move(publication),
    };
    checkCancellation(cancellation);

    static_assert(std::is_nothrow_move_assignable_v<Project>);
    static_assert(std::is_nothrow_move_constructible_v<UndoEntry>);
    static_assert(std::is_nothrow_move_constructible_v<CommandResult>);
    static_assert(noexcept(source_.swap(plannedSource)));
    project_ = std::move(planned);
    source_.swap(plannedSource);
    revision_ = revisionAfter;
    stateId_ = stateAfter;
    ++nextStateId_;
    undoJournal_.push_back(std::move(undoEntry));
    redoJournal_.clear();
    return result;
}

CommandResult ProjectSession::setClipProperties(
    const SetClipPropertiesCommand& command,
    std::stop_token cancellation
) {
    std::scoped_lock lock(mutex_);
    checkCancellation(cancellation);
    if (command.clipIds.empty()) {
        throw CommandError("invalidClipIds", "clipIds must be a non-empty array");
    }
    if (
        !command.durationFrames
        && !command.trimStartFrame
        && !command.trimEndFrame
        && !command.speed
    ) {
        throw CommandError(
            "missingClipProperties",
            "set_clip_properties requires at least one property"
        );
    }
    if (command.durationFrames && *command.durationFrames < 1) {
        throw CommandError("invalidDurationFrames", "durationFrames must be at least one");
    }
    if (command.trimStartFrame && *command.trimStartFrame < 0) {
        throw CommandError("invalidTrimStartFrame", "trimStartFrame must not be negative");
    }
    if (command.trimEndFrame && *command.trimEndFrame < 0) {
        throw CommandError("invalidTrimEndFrame", "trimEndFrame must not be negative");
    }
    if (command.speed && (!std::isfinite(*command.speed) || *command.speed <= 0.0)) {
        throw CommandError("invalidSpeed", "speed must be finite and greater than zero");
    }

    auto& timeline = activeTimeline(project_);
    if (rootKind_ != RootKind::current) {
        throw CommandError(
            "unsupportedProjectWriteRoot",
            "the Windows mutation slice does not write legacy project roots"
        );
    }
    if (timeline.id.origin != EntityIdOrigin::persisted) {
        throw CommandError(
            "unstableTimelineId",
            "set_clip_properties requires a persisted active timeline ID"
        );
    }

    std::set<std::string, std::less<>> directClipIds;
    std::set<std::string, std::less<>> linkedGroups;
    for (const auto& clipId : command.clipIds) {
        checkCancellation(cancellation);
        if (clipId.empty()) {
            throw CommandError("invalidClipId", "set_clip_properties clipId must not be empty");
        }
        const auto location = uniqueClipLocation(timeline, clipId);
        const auto& clip = timeline.tracks[location.trackIndex].clips[location.clipIndex];
        if (clip.multicamGroupId) {
            throw CommandError(
                "multicamTimingRefused",
                "timing properties would slip a multicam clip out of sync: " + clipId
            );
        }
        directClipIds.insert(clipId);
        if (clip.linkGroupId) {
            if (clip.linkGroupId->empty()) {
                throw CommandError("invalidLinkGroupId", "linkGroupId must not be empty");
            }
            linkedGroups.insert(*clip.linkGroupId);
        }
    }

    std::set<std::string, std::less<>> resolvedClipIds = directClipIds;
    for (const auto& track : timeline.tracks) {
        for (const auto& clip : track.clips) {
            checkCancellation(cancellation);
            if (clip.linkGroupId && linkedGroups.contains(*clip.linkGroupId)) {
                resolvedClipIds.insert(clip.id.value);
            }
        }
    }

    std::map<std::string, Clip, std::less<>> candidates;
    std::set<std::string, std::less<>> skippedSequenceSpeed;
    std::set<std::size_t> affectedTrackIndices;
    for (const auto& clipId : resolvedClipIds) {
        checkCancellation(cancellation);
        const auto location = uniqueClipLocation(timeline, clipId);
        const auto& clip = timeline.tracks[location.trackIndex].clips[location.clipIndex];
        Clip candidate = clip;
        const bool isTextPartner = !directClipIds.contains(clipId) && clip.mediaType == "text";
        if (command.durationFrames) candidate.durationFrames = *command.durationFrames;
        if (!isTextPartner) {
            if (command.trimStartFrame) candidate.trimStartFrame = *command.trimStartFrame;
            if (command.trimEndFrame) candidate.trimEndFrame = *command.trimEndFrame;
            if (command.speed) {
                if (candidate.sourceClipType == "sequence") {
                    skippedSequenceSpeed.insert(clipId);
                } else {
                    if (!command.durationFrames) {
                        const auto sourceFrames = static_cast<double>(clip.durationFrames) * clip.speed;
                        candidate.durationFrames = std::max<std::int64_t>(
                            1,
                            roundedFrame(sourceFrames / *command.speed, "retimed clip duration")
                        );
                    }
                    candidate.speed = *command.speed;
                }
            }
        }
        static_cast<void>(checkedEnd(candidate));
        const auto consumed = scaledFrameOffset(candidate.durationFrames, candidate.speed);
        static_cast<void>(checkedFrameAdd(
            checkedFrameAdd(candidate.trimStartFrame, consumed),
            candidate.trimEndFrame
        ));
        if (
            candidate.durationFrames == clip.durationFrames
            && candidate.trimStartFrame == clip.trimStartFrame
            && candidate.trimEndFrame == clip.trimEndFrame
            && candidate.speed == clip.speed
        ) {
            continue;
        }
        if (
            clip.id.origin != EntityIdOrigin::persisted
            && !sessionGeneratedClipIds_.contains(clipId)
        ) {
            throw CommandError(
                "unstableClipId",
                "set_clip_properties requires a stable clip ID: " + clipId
            );
        }
        if (timeline.tracks[location.trackIndex].id.origin != EntityIdOrigin::persisted) {
            throw CommandError(
                "unstableTrackId",
                "set_clip_properties requires persisted affected track IDs"
            );
        }
        affectedTrackIndices.insert(location.trackIndex);
        candidates.emplace(clipId, std::move(candidate));
    }

    Array notes;
    for (const auto& clipId : skippedSequenceSpeed) {
        notes.emplace_back("speed skipped for nested timeline clip: " + clipId);
    }
    if (candidates.empty()) {
        Array clips;
        for (std::size_t trackIndex = 0; trackIndex < timeline.tracks.size(); ++trackIndex) {
            for (const auto& clip : timeline.tracks[trackIndex].clips) {
                if (resolvedClipIds.contains(clip.id.value)) {
                    clips.push_back(clipValue(clip, trackIndex));
                }
            }
        }
        auto payload = receiptBase(false, revision_, revision_, "");
        payload["notes"] = Value(std::move(notes));
        payload.emplace("clips", Value(std::move(clips)));
        auto publication = preparePublication({
            ProjectDocument(*source_, rootKind_, project_, diagnostics_),
            revision_,
            stateId_,
            persistedStateId_,
            undoJournal_.size(),
            redoJournal_.size(),
        });
        checkCancellation(cancellation);
        return CommandResult{
            false,
            revision_,
            revision_,
            {},
            std::make_unique<Value>(std::move(payload)),
            std::move(publication),
        };
    }

    if (revision_ >= maximumRevision) {
        throw CommandError("revisionOverflow", "project revision cannot advance");
    }
    if (nextStateId_ == (std::numeric_limits<std::uint64_t>::max)()) {
        throw CommandError("stateIdentityOverflow", "project state identity cannot advance");
    }
    const auto timelineIndex = static_cast<std::size_t>(
        std::distance(project_.timelines.begin(), std::find_if(
            project_.timelines.begin(),
            project_.timelines.end(),
            [&](const Timeline& entry) { return entry.id.value == project_.activeTimelineId; }
        ))
    );
    auto sourceIndex = indexTimelineSource(*source_, rootKind_, timeline.id.value);
    const Value sourceTimelineSnapshot = sourceIndex.timeline;
    std::set<std::string, std::less<>> affectedTrackIds;
    for (const auto trackIndex : affectedTrackIndices) {
        affectedTrackIds.insert(timeline.tracks[trackIndex].id.value);
    }
    requireSourceTracksMatch(timeline, affectedTrackIds, sourceIndex);

    Project planned = project_;
    auto& plannedTimeline = activeTimeline(planned);
    auto plannedSource = std::make_unique<Value>(*source_);
    for (const auto& [clipId, candidate] : candidates) {
        checkCancellation(cancellation);
        const auto location = uniqueClipLocation(plannedTimeline, clipId);
        const auto original = plannedTimeline.tracks[location.trackIndex].clips[location.clipIndex];
        plannedTimeline.tracks[location.trackIndex].clips[location.clipIndex] = candidate;
        auto& sourceClips = sourceClipsForTrack(
            *plannedSource,
            rootKind_,
            plannedTimeline.id.value,
            plannedTimeline.tracks[location.trackIndex].id.value
        );
        auto& sourceObject = requireSourceObject(
            uniqueSourceEntity(sourceClips, clipId, "clip"),
            "clip"
        );
        if (candidate.durationFrames != original.durationFrames) {
            applyDurationSourceSemantics(sourceObject, original, candidate.durationFrames);
            sourceObject["durationFrames"] = Value(integerNumber(candidate.durationFrames));
        }
        if (candidate.trimStartFrame != original.trimStartFrame) {
            sourceObject["trimStartFrame"] = Value(integerNumber(candidate.trimStartFrame));
        }
        if (candidate.trimEndFrame != original.trimEndFrame) {
            sourceObject["trimEndFrame"] = Value(integerNumber(candidate.trimEndFrame));
        }
        if (candidate.speed != original.speed) {
            sourceObject["speed"] = Value(floatingNumber(candidate.speed));
        }
    }
    checkCancellation(cancellation);

    std::set<std::string, std::less<>> usedIds;
    for (const auto& projectTimeline : project_.timelines) {
        usedIds.insert(projectTimeline.id.value);
        for (const auto& track : projectTimeline.tracks) {
            usedIds.insert(track.id.value);
            for (const auto& clip : track.clips) {
                usedIds.insert(clip.id.value);
                if (clip.linkGroupId) usedIds.insert(*clip.linkGroupId);
            }
        }
    }
    const auto actionId = idGenerator_();
    if (actionId.empty() || !usedIds.insert(actionId).second) {
        throw CommandError("invalidGeneratedId", "ID generator returned an empty or duplicate value");
    }

    Array changedClips;
    for (std::size_t trackIndex = 0; trackIndex < plannedTimeline.tracks.size(); ++trackIndex) {
        for (const auto& clip : plannedTimeline.tracks[trackIndex].clips) {
            if (candidates.contains(clip.id.value)) {
                changedClips.push_back(clipValue(clip, trackIndex));
            }
        }
    }
    const auto revisionBefore = revision_;
    const auto revisionAfter = revisionBefore + 1;
    const auto stateAfter = nextStateId_;
    undoJournal_.reserve(undoJournal_.size() + 1);
    UndoEntry undoEntry{
        actionId,
        {},
        std::make_unique<TimelineSnapshot>(TimelineSnapshot{
            timelineIndex,
            timeline,
            sourceTimelineSnapshot,
        }),
        {},
        stateId_,
    };
    auto payload = receiptBase(true, revisionBefore, revisionAfter, actionId);
    payload["notes"] = Value(std::move(notes));
    payload.emplace("clips", Value(std::move(changedClips)));
    auto publication = preparePublication({
        ProjectDocument(*plannedSource, rootKind_, planned, diagnostics_),
        revisionAfter,
        stateAfter,
        persistedStateId_,
        undoJournal_.size() + 1,
        0,
    });
    CommandResult result{
        true,
        revisionBefore,
        revisionAfter,
        actionId,
        std::make_unique<Value>(std::move(payload)),
        std::move(publication),
    };
    checkCancellation(cancellation);

    static_assert(std::is_nothrow_move_assignable_v<Project>);
    static_assert(std::is_nothrow_move_constructible_v<UndoEntry>);
    static_assert(std::is_nothrow_move_constructible_v<CommandResult>);
    static_assert(noexcept(source_.swap(plannedSource)));
    project_ = std::move(planned);
    source_.swap(plannedSource);
    revision_ = revisionAfter;
    stateId_ = stateAfter;
    ++nextStateId_;
    undoJournal_.push_back(std::move(undoEntry));
    redoJournal_.clear();
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
    if (pending.timeline) {
        if (
            pending.timeline->timelineIndex >= project_.timelines.size()
            || project_.timelines[pending.timeline->timelineIndex].id.value
                != pending.timeline->timeline.id.value
        ) {
            throw CommandError("staleUndo", "the project timeline no longer matches the undo action");
        }
    }
    for (const auto& snapshot : pending.tracks) {
        checkCancellation(cancellation);
        if (
            snapshot.timelineIndex >= project_.timelines.size()
            || snapshot.trackIndex >= project_.timelines[snapshot.timelineIndex].tracks.size()
        ) {
            throw CommandError("staleUndo", "the project structure no longer matches the undo action");
        }
    }
    std::vector<TrackSnapshot> redoTracks;
    std::unique_ptr<TimelineSnapshot> redoTimeline;
    if (pending.timeline) {
        const auto timelineIndex = pending.timeline->timelineIndex;
        const auto& timeline = project_.timelines[timelineIndex];
        redoTimeline = std::make_unique<TimelineSnapshot>(TimelineSnapshot{
            timelineIndex,
            timeline,
            sourceTimelineFor(*source_, rootKind_, timeline.id.value),
        });
    } else {
        redoTracks.reserve(pending.tracks.size());
        for (const auto& snapshot : pending.tracks) {
            const auto& timeline = project_.timelines[snapshot.timelineIndex];
            const auto& track = timeline.tracks[snapshot.trackIndex];
            redoTracks.push_back({
                snapshot.timelineIndex,
                snapshot.trackIndex,
                track.clips,
                Value(sourceClipsForTrack(
                    *source_,
                    rootKind_,
                    timeline.id.value,
                    track.id.value
                )),
            });
        }
    }
    RedoEntry redoEntry{
        UndoEntry(pending),
        std::move(redoTracks),
        std::move(redoTimeline),
        stateId_,
    };
    const auto revisionBefore = revision_;
    const auto revisionAfter = revisionBefore + 1;
    Project planned = project_;
    auto plannedSource = std::make_unique<Value>(*source_);
    if (pending.timeline) {
        planned.timelines[pending.timeline->timelineIndex] = pending.timeline->timeline;
        sourceTimelineFor(
            *plannedSource,
            rootKind_,
            pending.timeline->timeline.id.value
        ) = pending.timeline->sourceTimeline;
    } else {
        for (const auto& snapshot : pending.tracks) {
            const auto& timeline = project_.timelines[snapshot.timelineIndex];
            const auto& track = timeline.tracks[snapshot.trackIndex];
            if (snapshot.sourceClips.kind() != Value::Kind::array) {
                throw CommandError("staleUndo", "undo source snapshot is not a clip array");
            }
            planned.timelines[snapshot.timelineIndex].tracks[snapshot.trackIndex].clips =
                snapshot.clips;
            sourceClipsForTrack(
                *plannedSource,
                rootKind_,
                timeline.id.value,
                track.id.value
            ) = snapshot.sourceClips.array();
        }
    }
    auto plannedGeneratedClipIds = sessionGeneratedClipIds_;
    for (const auto& clipId : pending.createdClipIds) {
        plannedGeneratedClipIds.erase(clipId);
    }
    redoJournal_.reserve(redoJournal_.size() + 1);
    auto payload = receiptBase(true, revisionBefore, revisionAfter, pending.actionId);
    payload.emplace("clips", Value(Array{}));
    payload["notes"] = Value(Array{Value("Re-read get_timeline after undo.")});
    auto publication = preparePublication({
        ProjectDocument(*plannedSource, rootKind_, planned, diagnostics_),
        revisionAfter,
        pending.beforeStateId,
        persistedStateId_,
        undoJournal_.size() - 1,
        redoJournal_.size() + 1,
    });
    CommandResult result{
        true,
        revisionBefore,
        revisionAfter,
        pending.actionId,
        std::make_unique<Value>(std::move(payload)),
        std::move(publication),
    };
    checkCancellation(cancellation);

    static_assert(std::is_nothrow_move_assignable_v<Project>);
    static_assert(std::is_nothrow_move_constructible_v<RedoEntry>);
    static_assert(noexcept(source_.swap(plannedSource)));
    project_ = std::move(planned);
    source_.swap(plannedSource);
    sessionGeneratedClipIds_.swap(plannedGeneratedClipIds);
    revision_ = revisionAfter;
    stateId_ = pending.beforeStateId;
    redoJournal_.push_back(std::move(redoEntry));
    undoJournal_.pop_back();
    return result;
}

CommandResult ProjectSession::redo(std::stop_token cancellation) {
    std::scoped_lock lock(mutex_);
    checkCancellation(cancellation);
    if (redoJournal_.empty()) {
        throw CommandError("nothingToRedo", "there is no Windows project action to redo");
    }
    if (revision_ >= maximumRevision) {
        throw CommandError("revisionOverflow", "project revision cannot advance");
    }
    const auto& pending = redoJournal_.back();
    if (pending.timeline) {
        if (
            pending.timeline->timelineIndex >= project_.timelines.size()
            || project_.timelines[pending.timeline->timelineIndex].id.value
                != pending.timeline->timeline.id.value
        ) {
            throw CommandError("staleRedo", "the project timeline no longer matches the redo action");
        }
    }
    for (const auto& snapshot : pending.tracks) {
        checkCancellation(cancellation);
        if (
            snapshot.timelineIndex >= project_.timelines.size()
            || snapshot.trackIndex >= project_.timelines[snapshot.timelineIndex].tracks.size()
        ) {
            throw CommandError("staleRedo", "the project structure no longer matches the redo action");
        }
    }

    Project planned = project_;
    auto plannedSource = std::make_unique<Value>(*source_);
    if (pending.timeline) {
        planned.timelines[pending.timeline->timelineIndex] = pending.timeline->timeline;
        sourceTimelineFor(
            *plannedSource,
            rootKind_,
            pending.timeline->timeline.id.value
        ) = pending.timeline->sourceTimeline;
    } else {
        for (const auto& snapshot : pending.tracks) {
            const auto& timeline = project_.timelines[snapshot.timelineIndex];
            const auto& track = timeline.tracks[snapshot.trackIndex];
            if (snapshot.sourceClips.kind() != Value::Kind::array) {
                throw CommandError("staleRedo", "redo source snapshot is not a clip array");
            }
            planned.timelines[snapshot.timelineIndex].tracks[snapshot.trackIndex].clips =
                snapshot.clips;
            sourceClipsForTrack(
                *plannedSource,
                rootKind_,
                timeline.id.value,
                track.id.value
            ) = snapshot.sourceClips.array();
        }
    }
    auto plannedGeneratedClipIds = sessionGeneratedClipIds_;
    plannedGeneratedClipIds.insert(
        pending.undo.createdClipIds.begin(),
        pending.undo.createdClipIds.end()
    );
    UndoEntry undoEntry(pending.undo);
    undoJournal_.reserve(undoJournal_.size() + 1);
    const auto revisionBefore = revision_;
    const auto revisionAfter = revisionBefore + 1;
    auto payload = receiptBase(true, revisionBefore, revisionAfter, pending.undo.actionId);
    payload.emplace("clips", Value(Array{}));
    payload["notes"] = Value(Array{Value("Re-read get_timeline after redo.")});
    auto publication = preparePublication({
        ProjectDocument(*plannedSource, rootKind_, planned, diagnostics_),
        revisionAfter,
        pending.afterStateId,
        persistedStateId_,
        undoJournal_.size() + 1,
        redoJournal_.size() - 1,
    });
    CommandResult result{
        true,
        revisionBefore,
        revisionAfter,
        pending.undo.actionId,
        std::make_unique<Value>(std::move(payload)),
        std::move(publication),
    };
    checkCancellation(cancellation);

    static_assert(std::is_nothrow_move_assignable_v<Project>);
    static_assert(std::is_nothrow_move_constructible_v<UndoEntry>);
    static_assert(noexcept(source_.swap(plannedSource)));
    project_ = std::move(planned);
    source_.swap(plannedSource);
    sessionGeneratedClipIds_.swap(plannedGeneratedClipIds);
    revision_ = revisionAfter;
    stateId_ = pending.afterStateId;
    undoJournal_.push_back(std::move(undoEntry));
    redoJournal_.pop_back();
    return result;
}

ProjectSessionSnapshot ProjectSession::snapshot(std::stop_token cancellation) const {
    std::scoped_lock lock(mutex_);
    checkCancellation(cancellation);
    return {
        ProjectDocument(*source_, rootKind_, project_, diagnostics_),
        revision_,
        stateId_,
        persistedStateId_,
        undoJournal_.size(),
        redoJournal_.size(),
    };
}

ProjectSaveSnapshot ProjectSession::saveSnapshot(std::stop_token cancellation) const {
    std::scoped_lock lock(mutex_);
    checkCancellation(cancellation);
    return {*source_, revision_, stateId_};
}

std::shared_ptr<const ProjectSessionSnapshot> ProjectSession::markPersisted(
    std::uint64_t stateId
) {
    std::scoped_lock lock(mutex_);
    if (stateId >= nextStateId_) {
        throw CommandError(
            "unknownProjectState",
            "persisted state identity was not produced by this project session"
        );
    }
    auto publication = preparePublication({
        ProjectDocument(*source_, rootKind_, project_, diagnostics_),
        revision_,
        stateId_,
        stateId,
        undoJournal_.size(),
        redoJournal_.size(),
    });
    persistedStateId_ = stateId;
    return publication;
}

std::uint64_t ProjectSession::revision() const {
    std::scoped_lock lock(mutex_);
    return revision_;
}

std::uint64_t ProjectSession::stateId() const {
    std::scoped_lock lock(mutex_);
    return stateId_;
}

std::uint64_t ProjectSession::persistedStateId() const {
    std::scoped_lock lock(mutex_);
    return persistedStateId_;
}

bool ProjectSession::dirty() const {
    std::scoped_lock lock(mutex_);
    return stateId_ != persistedStateId_;
}

}
