#include "palmier/project/project_reader.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iterator>
#include <limits>
#include <set>
#include <utility>

namespace palmier::project {
namespace {

using JsonKind = palmier::json::Value::Kind;

class Decoder final {
public:
    Decoder(const IdGenerator& idGenerator, std::vector<Diagnostic>& diagnostics)
        : idGenerator_(idGenerator), diagnostics_(diagnostics) {}

    Project decode(const palmier::json::Value& source, RootKind& rootKind) {
        const auto& root = requireObject(source, "");
        const auto timelines = root.find("timelines");
        if (timelines != root.end()) {
            rootKind = RootKind::current;
            return decodeCurrentProject(root, timelines->second);
        }

        rootKind = RootKind::legacy;
        auto timeline = decodeTimeline(source, "");
        const auto id = timeline.id.value;
        return {{std::move(timeline)}, id, {id}};
    }

private:
    Project decodeCurrentProject(
        const palmier::json::Object& root,
        const palmier::json::Value& timelinesValue
    ) {
        const auto& values = requireArray(timelinesValue, "/timelines");
        if (values.empty()) {
            fail("emptyTimelines", "/timelines", "project has no timelines");
        }

        std::vector<Timeline> timelines;
        timelines.reserve(values.size());
        for (std::size_t index = 0; index < values.size(); ++index) {
            timelines.push_back(decodeTimeline(
                values[index],
                "/timelines/" + std::to_string(index)
            ));
        }
        diagnoseDuplicateIds(timelines, "/timelines");

        std::set<std::string> ids;
        for (const auto& timeline : timelines) {
            ids.insert(timeline.id.value);
        }

        auto active = optionalNullableString(root, "activeTimelineId", "");
        if (!active || !ids.contains(*active)) {
            if (active) {
                diagnose("invalidActiveTimelineId", "/activeTimelineId");
            }
            active = timelines.front().id.value;
        }

        std::vector<std::string> open;
        const auto openField = root.find("openTimelineIds");
        if (openField != root.end() && openField->second.kind() != JsonKind::nullValue) {
            const auto& openValues = requireArray(openField->second, "/openTimelineIds");
            for (std::size_t index = 0; index < openValues.size(); ++index) {
                if (openValues[index].kind() != JsonKind::string) {
                    fail(
                        "wrongRequiredType",
                        "/openTimelineIds/" + std::to_string(index),
                        "open timeline ID must be a string"
                    );
                }
                const auto& candidate = openValues[index].string();
                if (ids.contains(candidate)) {
                    open.push_back(candidate);
                } else {
                    diagnose(
                        "invalidOpenTimelineId",
                        "/openTimelineIds/" + std::to_string(index)
                    );
                }
            }
        }
        if (std::find(open.begin(), open.end(), *active) == open.end()) {
            open.push_back(*active);
        }
        return {std::move(timelines), *active, std::move(open)};
    }

    Timeline decodeTimeline(const palmier::json::Value& value, const std::string& pointer) {
        const auto& object = requireObject(value, pointer);
        auto timeline = Timeline{
            decodeId(object, pointer),
            optionalString(object, "name", pointer, "Timeline 1"),
            requirePositiveInteger(object, "fps", pointer),
            requirePositiveInteger(object, "width", pointer),
            requirePositiveInteger(object, "height", pointer),
            optionalBoolean(object, "settingsConfigured", pointer, false),
            optionalLooseString(object, "folderId", pointer),
            {},
        };
        const auto& tracks = requireArray(
            requireField(object, "tracks", pointer),
            pointer + "/tracks"
        );
        timeline.tracks.reserve(tracks.size());
        for (std::size_t index = 0; index < tracks.size(); ++index) {
            timeline.tracks.push_back(decodeTrack(
                tracks[index],
                pointer + "/tracks/" + std::to_string(index)
            ));
        }
        diagnoseDuplicateIds(timeline.tracks, pointer + "/tracks");
        return timeline;
    }

    Track decodeTrack(const palmier::json::Value& value, const std::string& pointer) {
        const auto& object = requireObject(value, pointer);
        auto id = decodeId(object, pointer);
        const auto type = requireString(object, "type", pointer);
        if (!clipTypes().contains(type)) {
            fail("unsupportedRequiredEnum", pointer + "/type", "unsupported track type");
        }
        const auto muted = optionalBoolean(object, "muted", pointer, false);
        const auto hidden = optionalBoolean(object, "hidden", pointer, false);
        const auto syncLocked = optionalBoolean(object, "syncLocked", pointer, true);

        std::vector<Clip> decodedClips;
        const auto clips = object.find("clips");
        if (clips != object.end()) {
            const auto diagnosticsCheckpoint = diagnostics_.size();
            try {
                const auto& values = requireArray(clips->second, pointer + "/clips");
                decodedClips.reserve(values.size());
                for (std::size_t index = 0; index < values.size(); ++index) {
                    decodedClips.push_back(decodeClip(
                        values[index],
                        pointer + "/clips/" + std::to_string(index)
                    ));
                }
                diagnoseDuplicateIds(decodedClips, pointer + "/clips");
            } catch (const ReadError& error) {
                if (error.code == "invalidGeneratedId") {
                    throw;
                }
                decodedClips.clear();
                diagnostics_.resize(diagnosticsCheckpoint);
                diagnose("invalidOptionalDefaulted", pointer + "/clips");
            }
        }
        const auto displayHeight = std::clamp(
            optionalNumber(object, "displayHeight", pointer, 50),
            32.0,
            200.0
        );
        return {
            std::move(id),
            type,
            muted,
            hidden,
            syncLocked,
            displayHeight,
            std::move(decodedClips),
        };
    }

    Clip decodeClip(const palmier::json::Value& value, const std::string& pointer) {
        const auto& object = requireObject(value, pointer);
        auto id = decodeId(object, pointer);
        auto mediaRef = requireString(object, "mediaRef", pointer);
        auto mediaType = optionalClipType(object, "mediaType", pointer);
        auto sourceClipType = optionalClipType(object, "sourceClipType", pointer);
        const auto startFrame = requireInteger(object, "startFrame", pointer);
        const auto durationFrames = requireInteger(object, "durationFrames", pointer);
        const auto trimStartFrame = optionalInteger(object, "trimStartFrame", pointer, 0);
        const auto trimEndFrame = optionalInteger(object, "trimEndFrame", pointer, 0);
        const auto speed = optionalNumber(object, "speed", pointer, 1);
        const auto volume = optionalNumber(object, "volume", pointer, 1);
        const auto opacity = optionalNumber(object, "opacity", pointer, 1);
        auto linkGroupId = optionalLooseString(object, "linkGroupId", pointer);
        auto captionGroupId = optionalLooseString(object, "captionGroupId", pointer);
        auto multicamGroupId = optionalLooseString(object, "multicamGroupId", pointer);
        auto blendMode = optionalBlendMode(object, pointer);
        Clip clip{
            std::move(id),
            std::move(mediaRef),
            std::move(mediaType),
            std::move(sourceClipType),
            startFrame,
            durationFrames,
            trimStartFrame,
            trimEndFrame,
            speed,
            volume,
            opacity,
            std::move(blendMode),
            std::move(linkGroupId),
            std::move(captionGroupId),
            std::move(multicamGroupId),
        };
        if (
            startFrame < 0
            || durationFrames <= 0
            || (startFrame > 0 && durationFrames > std::numeric_limits<std::int64_t>::max() - startFrame)
        ) {
            diagnose("unsafeFrameRange", pointer);
        }
        return clip;
    }

    EntityId decodeId(const palmier::json::Object& object, const std::string& pointer) {
        const auto field = object.find("id");
        if (field != object.end() && field->second.kind() == JsonKind::string) {
            return {field->second.string(), EntityIdOrigin::persisted};
        }
        const auto generated = idGenerator_();
        if (generated.empty()) {
            fail("invalidGeneratedId", pointer + "/id", "ID generator returned an empty value");
        }
        diagnose("synthesizedId", pointer + "/id");
        return {generated, EntityIdOrigin::synthesized};
    }

    template<typename Entity>
    void diagnoseDuplicateIds(
        const std::vector<Entity>& entities,
        const std::string& pointer
    ) {
        std::set<std::string> ids;
        for (std::size_t index = 0; index < entities.size(); ++index) {
            if (!ids.insert(entities[index].id.value).second) {
                diagnose("duplicateStableId", pointer + "/" + std::to_string(index) + "/id");
            }
        }
    }

    static const std::set<std::string>& clipTypes() {
        static const std::set<std::string> values{
            "video", "audio", "image", "text", "lottie", "sequence",
        };
        return values;
    }

    static const std::set<std::string>& blendModes() {
        static const std::set<std::string> values{
            "normal", "darken", "multiply", "colorBurn", "lighten", "screen",
            "colorDodge", "overlay", "softLight", "hardLight", "difference",
            "exclusion", "hue", "saturation", "color", "luminosity",
        };
        return values;
    }

    std::string optionalClipType(
        const palmier::json::Object& object,
        const std::string& key,
        const std::string& pointer
    ) {
        const auto field = object.find(key);
        if (
            field != object.end()
            && field->second.kind() == JsonKind::string
            && clipTypes().contains(field->second.string())
        ) {
            return field->second.string();
        }
        if (field != object.end()) {
            diagnose("invalidOptionalDefaulted", pointer + "/" + key);
        }
        return "video";
    }

    std::optional<std::string> optionalBlendMode(
        const palmier::json::Object& object,
        const std::string& pointer
    ) {
        const auto field = object.find("blendMode");
        if (field == object.end() || field->second.kind() == JsonKind::nullValue) {
            return std::nullopt;
        }
        if (
            field->second.kind() == JsonKind::string
            && blendModes().contains(field->second.string())
        ) {
            return field->second.string();
        }
        diagnose("invalidOptionalDefaulted", pointer + "/blendMode");
        return std::nullopt;
    }

    static const palmier::json::Object& requireObject(
        const palmier::json::Value& value,
        const std::string& pointer
    ) {
        if (value.kind() != JsonKind::object) {
            fail("wrongRequiredType", pointer, "expected object");
        }
        return value.object();
    }

    static const palmier::json::Array& requireArray(
        const palmier::json::Value& value,
        const std::string& pointer
    ) {
        if (value.kind() != JsonKind::array) {
            fail("wrongRequiredType", pointer, "expected array");
        }
        return value.array();
    }

    static const palmier::json::Value& requireField(
        const palmier::json::Object& object,
        const std::string& key,
        const std::string& pointer
    ) {
        const auto field = object.find(key);
        if (field == object.end()) {
            fail("missingRequiredField", pointer + "/" + key, "missing required field");
        }
        return field->second;
    }

    static std::string requireString(
        const palmier::json::Object& object,
        const std::string& key,
        const std::string& pointer
    ) {
        const auto& value = requireField(object, key, pointer);
        if (value.kind() != JsonKind::string) {
            fail("wrongRequiredType", pointer + "/" + key, "expected string");
        }
        return value.string();
    }

    static std::int64_t requireInteger(
        const palmier::json::Object& object,
        const std::string& key,
        const std::string& pointer
    ) {
        const auto& value = requireField(object, key, pointer);
        if (value.kind() != JsonKind::number) {
            fail("wrongRequiredType", pointer + "/" + key, "expected integer");
        }
        if (!value.number().integer) {
            const auto& lexeme = value.number().lexeme;
            const auto code = lexeme.find_first_of(".eE") == std::string::npos
                ? "integerOutOfRange"
                : "wrongRequiredType";
            fail(code, pointer + "/" + key, "expected signed 64-bit integer");
        }
        return *value.number().integer;
    }

    static std::int64_t requirePositiveInteger(
        const palmier::json::Object& object,
        const std::string& key,
        const std::string& pointer
    ) {
        const auto value = requireInteger(object, key, pointer);
        if (value <= 0) {
            fail("invalidRequiredValue", pointer + "/" + key, "expected positive integer");
        }
        return value;
    }

    std::string optionalString(
        const palmier::json::Object& object,
        const std::string& key,
        const std::string& pointer,
        std::string fallback
    ) {
        const auto field = object.find(key);
        if (field != object.end() && field->second.kind() == JsonKind::string) {
            return field->second.string();
        }
        if (field != object.end()) {
            diagnose("invalidOptionalDefaulted", pointer + "/" + key);
        }
        return fallback;
    }

    std::optional<std::string> optionalLooseString(
        const palmier::json::Object& object,
        const std::string& key,
        const std::string& pointer
    ) {
        const auto field = object.find(key);
        if (field == object.end() || field->second.kind() == JsonKind::nullValue) {
            return std::nullopt;
        }
        if (field->second.kind() == JsonKind::string) {
            return field->second.string();
        }
        diagnose("invalidOptionalDefaulted", pointer + "/" + key);
        return std::nullopt;
    }

    static std::optional<std::string> optionalNullableString(
        const palmier::json::Object& object,
        const std::string& key,
        const std::string& pointer
    ) {
        const auto field = object.find(key);
        if (field == object.end() || field->second.kind() == JsonKind::nullValue) {
            return std::nullopt;
        }
        if (field->second.kind() != JsonKind::string) {
            fail("wrongRequiredType", pointer + "/" + key, "expected nullable string");
        }
        return field->second.string();
    }

    bool optionalBoolean(
        const palmier::json::Object& object,
        const std::string& key,
        const std::string& pointer,
        bool fallback
    ) {
        const auto field = object.find(key);
        if (field != object.end() && field->second.kind() == JsonKind::boolean) {
            return field->second.boolean();
        }
        if (field != object.end()) {
            diagnose("invalidOptionalDefaulted", pointer + "/" + key);
        }
        return fallback;
    }

    std::int64_t optionalInteger(
        const palmier::json::Object& object,
        const std::string& key,
        const std::string& pointer,
        std::int64_t fallback
    ) {
        const auto field = object.find(key);
        if (
            field != object.end()
            && field->second.kind() == JsonKind::number
            && field->second.number().integer
        ) {
            return *field->second.number().integer;
        }
        if (field != object.end()) {
            diagnose("invalidOptionalDefaulted", pointer + "/" + key);
        }
        return fallback;
    }

    double optionalNumber(
        const palmier::json::Object& object,
        const std::string& key,
        const std::string& pointer,
        double fallback
    ) {
        const auto field = object.find(key);
        if (field != object.end() && field->second.kind() == JsonKind::number) {
            double parsed = 0;
            const auto& lexeme = field->second.number().lexeme;
            const auto conversion = std::from_chars(
                lexeme.data(),
                lexeme.data() + lexeme.size(),
                parsed
            );
            if (
                conversion.ec == std::errc{}
                && conversion.ptr == lexeme.data() + lexeme.size()
                && std::isfinite(parsed)
            ) {
                return parsed;
            }
        }
        if (field != object.end()) {
            diagnose("invalidOptionalDefaulted", pointer + "/" + key);
        }
        return fallback;
    }

    void diagnose(std::string code, std::string pointer) {
        diagnostics_.push_back({std::move(code), std::move(pointer)});
    }

    [[noreturn]] static void fail(
        std::string code,
        std::string pointer,
        std::string detail
    ) {
        throw ReadError(std::move(code), std::move(pointer), std::move(detail));
    }

    const IdGenerator& idGenerator_;
    std::vector<Diagnostic>& diagnostics_;
};

palmier::json::Number integerNumber(std::int64_t value) {
    return {std::to_string(value), value};
}

palmier::json::Number floatingNumber(double value) {
    char buffer[64]{};
    const auto result = std::to_chars(
        std::begin(buffer),
        std::end(buffer),
        value,
        std::chars_format::general,
        std::numeric_limits<double>::max_digits10
    );
    if (result.ec != std::errc{}) {
        throw std::runtime_error("cannot encode normalized floating-point value");
    }
    std::string lexeme(buffer, static_cast<std::size_t>(result.ptr - buffer));
    if (lexeme.find_first_of(".eE") == std::string::npos) {
        lexeme += ".0";
    }
    return {std::move(lexeme), std::nullopt};
}

palmier::json::Value optionalStringValue(const std::optional<std::string>& value) {
    return value ? palmier::json::Value(*value) : palmier::json::Value();
}

palmier::json::Value entityIdValue(const EntityId& id) {
    return palmier::json::Value(palmier::json::Object{
        {"origin", palmier::json::Value(
            id.origin == EntityIdOrigin::persisted ? "persisted" : "synthesized"
        )},
        {"value", palmier::json::Value(id.value)},
    });
}

palmier::json::Value clipValue(const Clip& clip) {
    return palmier::json::Value(palmier::json::Object{
        {"blendMode", optionalStringValue(clip.blendMode)},
        {"captionGroupId", optionalStringValue(clip.captionGroupId)},
        {"durationFrames", palmier::json::Value(integerNumber(clip.durationFrames))},
        {"id", entityIdValue(clip.id)},
        {"linkGroupId", optionalStringValue(clip.linkGroupId)},
        {"mediaRef", palmier::json::Value(clip.mediaRef)},
        {"mediaType", palmier::json::Value(clip.mediaType)},
        {"multicamGroupId", optionalStringValue(clip.multicamGroupId)},
        {"opacity", palmier::json::Value(floatingNumber(clip.opacity))},
        {"sourceClipType", palmier::json::Value(clip.sourceClipType)},
        {"speed", palmier::json::Value(floatingNumber(clip.speed))},
        {"startFrame", palmier::json::Value(integerNumber(clip.startFrame))},
        {"trimEndFrame", palmier::json::Value(integerNumber(clip.trimEndFrame))},
        {"trimStartFrame", palmier::json::Value(integerNumber(clip.trimStartFrame))},
        {"volume", palmier::json::Value(floatingNumber(clip.volume))},
    });
}

palmier::json::Value trackValue(const Track& track) {
    palmier::json::Array clips;
    for (const auto& clip : track.clips) {
        clips.push_back(clipValue(clip));
    }
    return palmier::json::Value(palmier::json::Object{
        {"clips", palmier::json::Value(std::move(clips))},
        {"displayHeight", palmier::json::Value(floatingNumber(track.displayHeight))},
        {"hidden", palmier::json::Value(track.hidden)},
        {"id", entityIdValue(track.id)},
        {"muted", palmier::json::Value(track.muted)},
        {"syncLocked", palmier::json::Value(track.syncLocked)},
        {"type", palmier::json::Value(track.type)},
    });
}

palmier::json::Value timelineValue(const Timeline& timeline) {
    palmier::json::Array tracks;
    for (const auto& track : timeline.tracks) {
        tracks.push_back(trackValue(track));
    }
    return palmier::json::Value(palmier::json::Object{
        {"folderId", optionalStringValue(timeline.folderId)},
        {"fps", palmier::json::Value(integerNumber(timeline.fps))},
        {"height", palmier::json::Value(integerNumber(timeline.height))},
        {"id", entityIdValue(timeline.id)},
        {"name", palmier::json::Value(timeline.name)},
        {"settingsConfigured", palmier::json::Value(timeline.settingsConfigured)},
        {"tracks", palmier::json::Value(std::move(tracks))},
        {"width", palmier::json::Value(integerNumber(timeline.width))},
    });
}

}

ReadError::ReadError(std::string codeValue, std::string pointerValue, std::string detail)
    : std::runtime_error(std::move(detail)),
      code(std::move(codeValue)),
      jsonPointer(std::move(pointerValue)) {}

ProjectDocument::ProjectDocument(
    palmier::json::Value source,
    RootKind rootKind,
    Project project,
    std::vector<Diagnostic> diagnostics
) : source_(std::move(source)),
    rootKind_(rootKind),
    project_(std::move(project)),
    diagnostics_(std::move(diagnostics)) {}

const palmier::json::Value& ProjectDocument::source() const noexcept { return source_; }
RootKind ProjectDocument::rootKind() const noexcept { return rootKind_; }
const Project& ProjectDocument::project() const noexcept { return project_; }
const std::vector<Diagnostic>& ProjectDocument::diagnostics() const noexcept {
    return diagnostics_;
}
ProjectDocumentDisposition ProjectDocument::disposition() const noexcept {
    return ProjectDocumentDisposition::readOnly;
}

ProjectDocument readProject(std::string_view source, const IdGenerator& idGenerator) {
    return readProject(palmier::json::parse(source), idGenerator);
}

ProjectDocument readProject(palmier::json::Value source, const IdGenerator& idGenerator) {
    std::vector<Diagnostic> diagnostics;
    RootKind rootKind = RootKind::current;
    Decoder decoder(idGenerator, diagnostics);
    auto project = decoder.decode(source, rootKind);
    return ProjectDocument(
        std::move(source),
        rootKind,
        std::move(project),
        std::move(diagnostics)
    );
}

std::string normalizedModelJson(const ProjectDocument& document) {
    palmier::json::Array timelines;
    for (const auto& timeline : document.project().timelines) {
        timelines.push_back(timelineValue(timeline));
    }
    palmier::json::Array open;
    for (const auto& id : document.project().openTimelineIds) {
        open.emplace_back(id);
    }
    palmier::json::Array diagnostics;
    for (const auto& diagnostic : document.diagnostics()) {
        diagnostics.emplace_back(palmier::json::Object{
            {"code", palmier::json::Value(diagnostic.code)},
            {"jsonPointer", palmier::json::Value(diagnostic.jsonPointer)},
        });
    }
    const palmier::json::Value normalized(palmier::json::Object{
        {"contractVersion", palmier::json::Value(integerNumber(1))},
        {"diagnostics", palmier::json::Value(std::move(diagnostics))},
        {"disposition", palmier::json::Value("readOnly")},
        {"project", palmier::json::Value(palmier::json::Object{
            {"activeTimelineId", palmier::json::Value(document.project().activeTimelineId)},
            {"openTimelineIds", palmier::json::Value(std::move(open))},
            {"timelines", palmier::json::Value(std::move(timelines))},
        })},
        {"rootKind", palmier::json::Value(
            document.rootKind() == RootKind::current ? "current" : "legacy"
        )},
    });
    return palmier::json::canonical(normalized);
}

}
