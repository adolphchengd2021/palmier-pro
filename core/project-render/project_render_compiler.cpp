#include "palmier/project_render/project_render_compiler.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace palmier::project_render {
namespace {

using JsonKind = json::Value::Kind;

struct RawNode final {
    const json::Object* object;
    std::string pointer;
};

struct StaticTransform final {
    double centerX{0.5};
    double centerY{0.5};
    double width{1};
    double height{1};
    double rotation{};
    bool flipHorizontal{};
    bool flipVertical{};
};

struct StaticCrop final {
    double left{};
    double top{};
    double right{};
    double bottom{};
};

struct StaticEffect final {
    std::string type;
    bool enabled{true};
    std::optional<double> value;
    bool hasActiveTrack{};
};

[[noreturn]] void fail(
    std::string code,
    std::string pointer,
    std::string detail
) {
    throw ProjectRenderCompileError(
        std::move(code),
        std::move(pointer),
        std::move(detail)
    );
}

void checkCancellation(std::stop_token cancellation) {
    if (cancellation.stop_requested()) {
        fail("cancelled", "", "project render compilation was cancelled");
    }
}

const json::Object* object(const json::Value& value) noexcept {
    return value.kind() == JsonKind::object ? &value.object() : nullptr;
}

const json::Array* array(const json::Value& value) noexcept {
    return value.kind() == JsonKind::array ? &value.array() : nullptr;
}

const json::Value* field(const json::Object& value, std::string_view key) {
    const auto found = value.find(std::string(key));
    return found == value.end() ? nullptr : &found->second;
}

std::optional<double> number(const json::Value& value) noexcept {
    if (value.kind() != JsonKind::number) return std::nullopt;
    const auto& lexeme = value.number().lexeme;
    double parsed{};
    const auto conversion = std::from_chars(
        lexeme.data(),
        lexeme.data() + lexeme.size(),
        parsed
    );
    if (
        conversion.ec != std::errc{}
        || conversion.ptr != lexeme.data() + lexeme.size()
        || !std::isfinite(parsed)
    ) {
        return std::nullopt;
    }
    return parsed;
}

double optionalVisualNumber(
    const json::Object& value,
    std::string_view key,
    double fallback,
    const std::string& pointer
) {
    const auto* candidate = field(value, key);
    if (candidate == nullptr) return fallback;
    const auto parsed = number(*candidate);
    if (!parsed) {
        fail(
            "malformedVisualProperty",
            pointer + "/" + std::string(key),
            "visual property must be a finite number"
        );
    }
    return *parsed;
}

std::int64_t optionalVisualInteger(
    const json::Object& value,
    std::string_view key,
    std::int64_t fallback,
    const std::string& pointer
) {
    const auto* candidate = field(value, key);
    if (candidate == nullptr) return fallback;
    if (candidate->kind() != JsonKind::number || !candidate->number().integer) {
        fail(
            "malformedVisualProperty",
            pointer + "/" + std::string(key),
            "visual frame property must be an integer"
        );
    }
    return *candidate->number().integer;
}

bool optionalVisualBoolean(
    const json::Object& value,
    std::string_view key,
    bool fallback,
    const std::string& pointer
) {
    const auto* candidate = field(value, key);
    if (candidate == nullptr) return fallback;
    if (candidate->kind() != JsonKind::boolean) {
        fail(
            "malformedVisualProperty",
            pointer + "/" + std::string(key),
            "visual property must be a boolean"
        );
    }
    return candidate->boolean();
}

StaticTransform transform(const json::Object& clip, const std::string& pointer) {
    const auto* value = field(clip, "transform");
    if (value == nullptr || value->kind() == JsonKind::nullValue) return {};
    const auto* properties = object(*value);
    if (properties == nullptr) {
        fail(
            "malformedVisualProperty",
            pointer + "/transform",
            "transform must be an object"
        );
    }

    StaticTransform result;
    const auto assignNumber = [&](std::string_view key, double fallback, double& output) {
        const auto* candidate = field(*properties, key);
        if (candidate == nullptr) {
            output = fallback;
            return;
        }
        const auto parsed = number(*candidate);
        if (!parsed) {
            fail(
                "malformedVisualProperty",
                pointer + "/transform/" + std::string(key),
                "transform property must be a finite number"
            );
        }
        output = *parsed;
    };
    const auto assignBoolean = [&](std::string_view key, bool fallback, bool& output) {
        const auto* candidate = field(*properties, key);
        if (candidate == nullptr) {
            output = fallback;
            return;
        }
        if (candidate->kind() != JsonKind::boolean) {
            fail(
                "malformedVisualProperty",
                pointer + "/transform/" + std::string(key),
                "transform property must be a boolean"
            );
        }
        output = candidate->boolean();
    };
    assignNumber("width", 1, result.width);
    assignNumber("height", 1, result.height);
    if (const auto* centerX = field(*properties, "centerX")) {
        const auto parsed = number(*centerX);
        if (!parsed) {
            fail(
                "malformedVisualProperty",
                pointer + "/transform/centerX",
                "transform property must be a finite number"
            );
        }
        result.centerX = *parsed;
    } else if (const auto* legacyX = field(*properties, "x")) {
        const auto parsed = number(*legacyX);
        if (!parsed) {
            fail(
                "malformedVisualProperty",
                pointer + "/transform/x",
                "legacy transform property must be a finite number"
            );
        }
        result.centerX = *parsed + result.width - 0.5;
    }
    if (const auto* centerY = field(*properties, "centerY")) {
        const auto parsed = number(*centerY);
        if (!parsed) {
            fail(
                "malformedVisualProperty",
                pointer + "/transform/centerY",
                "transform property must be a finite number"
            );
        }
        result.centerY = *parsed;
    } else if (const auto* legacyY = field(*properties, "y")) {
        const auto parsed = number(*legacyY);
        if (!parsed) {
            fail(
                "malformedVisualProperty",
                pointer + "/transform/y",
                "legacy transform property must be a finite number"
            );
        }
        result.centerY = *parsed + result.height - 0.5;
    }
    assignNumber("rotation", 0, result.rotation);
    assignBoolean("flipHorizontal", false, result.flipHorizontal);
    assignBoolean("flipVertical", false, result.flipVertical);
    return result;
}

StaticCrop crop(const json::Object& clip, const std::string& pointer) {
    const auto* value = field(clip, "crop");
    if (value == nullptr || value->kind() == JsonKind::nullValue) return {};
    const auto* properties = object(*value);
    if (properties == nullptr) {
        fail(
            "malformedVisualProperty",
            pointer + "/crop",
            "crop must be an object"
        );
    }
    const auto* left = field(*properties, "left");
    const auto* top = field(*properties, "top");
    const auto* right = field(*properties, "right");
    const auto* bottom = field(*properties, "bottom");
    if (left == nullptr || top == nullptr || right == nullptr || bottom == nullptr) {
        fail(
            "malformedVisualProperty",
            pointer + "/crop",
            "crop must contain four finite edge values"
        );
    }
    const auto parsedLeft = number(*left);
    const auto parsedTop = number(*top);
    const auto parsedRight = number(*right);
    const auto parsedBottom = number(*bottom);
    if (!parsedLeft || !parsedTop || !parsedRight || !parsedBottom) {
        fail(
            "malformedVisualProperty",
            pointer + "/crop",
            "crop must contain four finite edge values"
        );
    }
    return {*parsedLeft, *parsedTop, *parsedRight, *parsedBottom};
}

bool activeTrack(const json::Value* value, const std::string& pointer) {
    if (value == nullptr || value->kind() == JsonKind::nullValue) return false;
    const auto* properties = object(*value);
    if (properties == nullptr) {
        fail("malformedVisualProperty", pointer, "keyframe track must be an object");
    }
    const auto* keyframes = field(*properties, "keyframes");
    const auto* values = keyframes == nullptr ? nullptr : array(*keyframes);
    if (values == nullptr) {
        fail(
            "malformedVisualProperty",
            pointer + "/keyframes",
            "keyframe track must contain a keyframe array"
        );
    }
    return !values->empty();
}

std::vector<StaticEffect> effects(
    const json::Object& clip,
    const std::string& pointer,
    std::stop_token cancellation
) {
    const auto* value = field(clip, "effects");
    if (value == nullptr || value->kind() == JsonKind::nullValue) return {};
    const auto* values = array(*value);
    if (values == nullptr) {
        fail("malformedVisualProperty", pointer, "effects must be an array");
    }
    if (values->size() > 256) {
        fail("resourceLimitExceeded", pointer, "effect count exceeds 256");
    }

    std::vector<StaticEffect> result;
    result.reserve(values->size());
    for (std::size_t index = 0; index < values->size(); ++index) {
        checkCancellation(cancellation);
        const auto& item = (*values)[index];
        const auto effectPointer = pointer + "/" + std::to_string(index);
        const auto* effect = object(item);
        if (effect == nullptr) {
            fail("malformedVisualProperty", effectPointer, "effect must be an object");
        }
        const auto* type = field(*effect, "type");
        if (type == nullptr || type->kind() != JsonKind::string) {
            fail(
                "malformedVisualProperty",
                effectPointer + "/type",
                "effect type must be a string"
            );
        }
        StaticEffect decoded;
        decoded.type = type->string();
        decoded.enabled = optionalVisualBoolean(*effect, "enabled", true, effectPointer);
        if (!decoded.enabled) {
            result.push_back(std::move(decoded));
            continue;
        }
        const auto* paramsValue = field(*effect, "params");
        const auto* params = paramsValue == nullptr ? nullptr : object(*paramsValue);
        if (
            paramsValue != nullptr
            && paramsValue->kind() != JsonKind::nullValue
            && params == nullptr
        ) {
            fail(
                "malformedVisualProperty",
                effectPointer + "/params",
                "effect params must be an object"
            );
        }
        if (params != nullptr) {
            const auto* evValue = field(*params, "ev");
            const auto* ev = evValue == nullptr ? nullptr : object(*evValue);
            if (evValue != nullptr && evValue->kind() != JsonKind::nullValue && ev == nullptr) {
                fail(
                    "malformedVisualProperty",
                    effectPointer + "/params/ev",
                    "exposure parameter must be an object"
                );
            }
            if (ev != nullptr) {
                if (const auto* staticValue = field(*ev, "value")) {
                    decoded.value = number(*staticValue);
                    if (!decoded.value) {
                        fail(
                            "malformedVisualProperty",
                            effectPointer + "/params/ev/value",
                            "exposure value must be a finite number"
                        );
                    }
                }
                decoded.hasActiveTrack = activeTrack(
                    field(*ev, "track"),
                    effectPointer + "/params/ev/track"
                );
            }
        }
        result.push_back(std::move(decoded));
    }
    return result;
}

template<typename Entity>
const Entity* uniquePersistedEntity(
    const std::vector<Entity>& entities,
    std::string_view id,
    std::string_view pointer,
    std::stop_token cancellation
) {
    const Entity* result = nullptr;
    for (const auto& entity : entities) {
        checkCancellation(cancellation);
        if (entity.id.value != id) continue;
        if (result != nullptr) {
            fail("duplicateStableId", std::string(pointer), "render identity is ambiguous");
        }
        result = &entity;
    }
    if (result == nullptr) {
        fail("entityUnavailable", std::string(pointer), "render entity is missing");
    }
    if (result->id.origin != project::EntityIdOrigin::persisted) {
        fail("unstableEntityId", std::string(pointer), "render entity ID is synthesized");
    }
    return result;
}

RawNode uniqueRawEntity(
    const json::Array& values,
    std::string_view id,
    const std::string& pointer,
    std::stop_token cancellation
) {
    std::optional<RawNode> result;
    for (std::size_t index = 0; index < values.size(); ++index) {
        checkCancellation(cancellation);
        const auto* candidate = object(values[index]);
        if (candidate == nullptr) continue;
        const auto* candidateId = field(*candidate, "id");
        if (
            candidateId == nullptr
            || candidateId->kind() != JsonKind::string
            || candidateId->string() != id
        ) {
            continue;
        }
        if (result) {
            fail("duplicateStableId", pointer, "raw render identity is ambiguous");
        }
        result = RawNode{candidate, pointer + "/" + std::to_string(index)};
    }
    if (!result) fail("entityUnavailable", pointer, "raw render entity is missing");
    return *result;
}

RawNode rawTimeline(
    const project::ProjectDocument& document,
    std::string_view timelineId,
    std::stop_token cancellation
) {
    if (document.rootKind() == project::RootKind::legacy) {
        const auto* timeline = object(document.source());
        if (timeline == nullptr) fail("entityUnavailable", "", "legacy timeline is missing");
        const auto* id = field(*timeline, "id");
        if (id == nullptr || id->kind() != JsonKind::string || id->string() != timelineId) {
            fail("entityUnavailable", "/id", "legacy timeline ID does not match");
        }
        return {timeline, ""};
    }
    const auto* root = object(document.source());
    const auto* timelineValue = root == nullptr ? nullptr : field(*root, "timelines");
    const auto* timelines = timelineValue == nullptr ? nullptr : array(*timelineValue);
    if (timelines == nullptr) fail("entityUnavailable", "/timelines", "timeline source is missing");
    return uniqueRawEntity(*timelines, timelineId, "/timelines", cancellation);
}

RawNode rawChild(
    const RawNode& parent,
    std::string_view collection,
    std::string_view id,
    std::stop_token cancellation
) {
    const auto* value = field(*parent.object, collection);
    const auto* values = value == nullptr ? nullptr : array(*value);
    const auto pointer = parent.pointer + "/" + std::string(collection);
    if (values == nullptr) fail("entityUnavailable", pointer, "render collection is missing");
    return uniqueRawEntity(*values, id, pointer, cancellation);
}

float checkedFloat(double value, const std::string& pointer) {
    const auto converted = static_cast<float>(value);
    if (!std::isfinite(value) || !std::isfinite(converted)) {
        fail("unsupportedNumericValue", pointer, "render value is outside float32");
    }
    return converted;
}

}

ProjectRenderCompileError::ProjectRenderCompileError(
    std::string codeValue,
    std::string jsonPointerValue,
    std::string detail
) : std::runtime_error(std::move(detail)),
    code(std::move(codeValue)),
    jsonPointer(std::move(jsonPointerValue)) {}

StaticVideoLayer compileStaticVideoLayer(
    const project::ProjectDocument& document,
    std::string_view timelineId,
    std::string_view trackId,
    std::string_view clipId,
    std::stop_token cancellation
) {
    checkCancellation(cancellation);
    const auto* timeline = uniquePersistedEntity(
        document.project().timelines,
        timelineId,
        "/timelines",
        cancellation
    );
    const auto* track = uniquePersistedEntity(
        timeline->tracks,
        trackId,
        "/tracks",
        cancellation
    );
    const auto* clip = uniquePersistedEntity(
        track->clips,
        clipId,
        "/clips",
        cancellation
    );
    if (track->type != "video" || track->hidden) {
        fail("unsupportedTrack", "/tracks", "render track must be visible video");
    }
    if (clip->mediaType != "video" || clip->sourceClipType != "video") {
        fail("unsupportedClipType", "/clips", "render clip must reference video");
    }
    if (
        clip->startFrame < 0
        || clip->durationFrames <= 0
        || clip->startFrame > std::numeric_limits<std::int64_t>::max() - clip->durationFrames
        || clip->trimStartFrame < 0
        || clip->trimEndFrame < 0
        || clip->trimStartFrame > std::numeric_limits<std::int64_t>::max()
            - (clip->durationFrames - 1)
        || clip->speed != 1
    ) {
        fail("unsupportedClipTiming", "/clips", "static compiler requires safe 1x timing");
    }
    if (clip->blendMode && *clip->blendMode != "normal") {
        fail("unsupportedBlendMode", "/clips", "static compiler supports normal blend only");
    }
    if (!std::isfinite(clip->opacity) || clip->opacity < 0 || clip->opacity > 1) {
        fail("unsupportedOpacity", "/clips", "static opacity must be in 0...1");
    }
    if (
        timeline->fps > std::numeric_limits<std::int32_t>::max()
        || timeline->width > std::numeric_limits<std::uint32_t>::max()
        || timeline->height > std::numeric_limits<std::uint32_t>::max()
    ) {
        fail("unsupportedTimelineSettings", "/timelines", "timeline exceeds render domains");
    }

    const auto rawTimelineNode = rawTimeline(document, timelineId, cancellation);
    const auto rawTrackNode = rawChild(rawTimelineNode, "tracks", trackId, cancellation);
    const auto rawClipNode = rawChild(rawTrackNode, "clips", clipId, cancellation);
    if (const auto* hidden = field(*rawTrackNode.object, "hidden")) {
        if (hidden->kind() != JsonKind::boolean && hidden->kind() != JsonKind::nullValue) {
            fail(
                "malformedVisualProperty",
                rawTrackNode.pointer + "/hidden",
                "track visibility must be a boolean or null"
            );
        }
    }
    const auto& rawClip = *rawClipNode.object;
    constexpr std::string_view clipTypeKeys[]{"mediaType", "sourceClipType"};
    for (const auto key : clipTypeKeys) {
        if (const auto* type = field(rawClip, key)) {
            if (type->kind() != JsonKind::string && type->kind() != JsonKind::nullValue) {
                fail(
                    "malformedVisualProperty",
                    rawClipNode.pointer + "/" + std::string(key),
                    "clip type must be a string or null"
                );
            }
        }
    }
    const auto rawTrimStart = optionalVisualInteger(
        rawClip,
        "trimStartFrame",
        0,
        rawClipNode.pointer
    );
    const auto rawTrimEnd = optionalVisualInteger(
        rawClip,
        "trimEndFrame",
        0,
        rawClipNode.pointer
    );
    const auto rawSpeed = optionalVisualNumber(
        rawClip,
        "speed",
        1,
        rawClipNode.pointer
    );
    const auto rawOpacity = optionalVisualNumber(
        rawClip,
        "opacity",
        1,
        rawClipNode.pointer
    );
    if (
        rawTrimStart != clip->trimStartFrame
        || rawTrimEnd != clip->trimEndFrame
        || rawSpeed != clip->speed
        || rawOpacity != clip->opacity
    ) {
        fail(
            "projectProjectionMismatch",
            rawClipNode.pointer,
            "typed project values differ from retained source values"
        );
    }
    if (const auto* rawBlendMode = field(rawClip, "blendMode")) {
        if (
            rawBlendMode->kind() != JsonKind::nullValue
            && rawBlendMode->kind() != JsonKind::string
        ) {
            fail(
                "malformedVisualProperty",
                rawClipNode.pointer + "/blendMode",
                "blend mode must be a string or null"
            );
        }
        if (
            rawBlendMode->kind() == JsonKind::string
            && rawBlendMode->string() != "normal"
        ) {
            fail(
                "unsupportedBlendMode",
                rawClipNode.pointer + "/blendMode",
                "static compiler supports normal blend only"
            );
        }
    }
    if (
        optionalVisualInteger(rawClip, "fadeInFrames", 0, rawClipNode.pointer) != 0
        || optionalVisualInteger(rawClip, "fadeOutFrames", 0, rawClipNode.pointer) != 0
        || activeTrack(
            field(rawClip, "opacityTrack"),
            rawClipNode.pointer + "/opacityTrack"
        )
        || activeTrack(
            field(rawClip, "positionTrack"),
            rawClipNode.pointer + "/positionTrack"
        )
        || activeTrack(
            field(rawClip, "scaleTrack"),
            rawClipNode.pointer + "/scaleTrack"
        )
        || activeTrack(
            field(rawClip, "rotationTrack"),
            rawClipNode.pointer + "/rotationTrack"
        )
        || activeTrack(
            field(rawClip, "cropTrack"),
            rawClipNode.pointer + "/cropTrack"
        )
    ) {
        fail(
            "dynamicVisualsUnsupported",
            rawClipNode.pointer,
            "static compiler does not sample fades or keyframes"
        );
    }
    const auto decodedCrop = crop(rawClip, rawClipNode.pointer);
    if (
        decodedCrop.left != 0 || decodedCrop.top != 0
        || decodedCrop.right != 0 || decodedCrop.bottom != 0
        || optionalVisualNumber(rawClip, "edgeRounding", 0, rawClipNode.pointer) != 0
        || optionalVisualNumber(rawClip, "edgeSoftness", 0, rawClipNode.pointer) != 0
    ) {
        fail("unsupportedMasking", rawClipNode.pointer, "crop and edge masks are unsupported");
    }
    const auto decodedTransform = transform(rawClip, rawClipNode.pointer);
    if (decodedTransform.flipHorizontal || decodedTransform.flipVertical) {
        fail("unsupportedFlip", rawClipNode.pointer + "/transform", "flip is unsupported");
    }

    std::optional<float> exposure;
    for (const auto& effect : effects(
        rawClip,
        rawClipNode.pointer + "/effects",
        cancellation
    )) {
        checkCancellation(cancellation);
        if (!effect.enabled) continue;
        if (effect.type != "color.exposure" || exposure.has_value()) {
            fail("unsupportedEffect", rawClipNode.pointer + "/effects", "one exposure effect is supported");
        }
        if (effect.hasActiveTrack) {
            fail("dynamicEffectUnsupported", rawClipNode.pointer + "/effects", "effect keyframes are unsupported");
        }
        const auto value = effect.value.value_or(0);
        if (value < -3 || value > 3) {
            fail("unsupportedEffectValue", rawClipNode.pointer + "/effects", "exposure is outside -3...3");
        }
        exposure = checkedFloat(value, rawClipNode.pointer + "/effects");
    }

    StaticVideoLayer result{
        static_cast<std::uint32_t>(timeline->width),
        static_cast<std::uint32_t>(timeline->height),
        static_cast<std::int32_t>(timeline->fps),
        timeline->id.value,
        track->id.value,
        clip->id.value,
        clip->mediaRef,
        clip->startFrame,
        clip->durationFrames,
        clip->trimStartFrame,
        {
            checkedFloat(decodedTransform.centerX, rawClipNode.pointer + "/transform/centerX"),
            checkedFloat(decodedTransform.centerY, rawClipNode.pointer + "/transform/centerY"),
            checkedFloat(decodedTransform.width, rawClipNode.pointer + "/transform/width"),
            checkedFloat(decodedTransform.height, rawClipNode.pointer + "/transform/height"),
            checkedFloat(decodedTransform.rotation, rawClipNode.pointer + "/transform/rotation"),
        },
        checkedFloat(rawOpacity, rawClipNode.pointer + "/opacity"),
        exposure,
    };
    try {
        static_cast<void>(makeRenderPlan(result, result.timelineStartFrame));
    } catch (const render::RenderError& error) {
        fail("unsupportedRenderPlan", error.pointer, error.what());
    }
    return result;
}

StaticVideoLayer compileExclusiveStaticVideoLayer(
    const project::ProjectDocument& document,
    std::string_view timelineId,
    std::string_view trackId,
    std::string_view clipId,
    std::stop_token cancellation
) {
    auto result = compileStaticVideoLayer(
        document,
        timelineId,
        trackId,
        clipId,
        cancellation
    );
    const project::Timeline* selectedTimeline = nullptr;
    const project::Clip* selectedClip = nullptr;
    std::size_t selectedTimelineIndex = 0;
    for (std::size_t timelineIndex = 0;
         timelineIndex < document.project().timelines.size();
         ++timelineIndex) {
        checkCancellation(cancellation);
        const auto& timeline = document.project().timelines[timelineIndex];
        if (timeline.id.value != timelineId) continue;
        selectedTimeline = &timeline;
        selectedTimelineIndex = timelineIndex;
        for (const auto& track : timeline.tracks) {
            checkCancellation(cancellation);
            if (track.id.value != trackId) continue;
            for (const auto& clip : track.clips) {
                checkCancellation(cancellation);
                if (clip.id.value == clipId) {
                    selectedClip = &clip;
                    break;
                }
            }
        }
    }
    if (selectedTimeline == nullptr || selectedClip == nullptr) {
        fail("missingEntity", "/timelines", "exclusive render entity disappeared");
    }
    const auto selectedEnd = selectedClip->startFrame + selectedClip->durationFrames;
    for (std::size_t trackIndex = 0;
         trackIndex < selectedTimeline->tracks.size();
         ++trackIndex) {
        checkCancellation(cancellation);
        const auto& track = selectedTimeline->tracks[trackIndex];
        if (track.hidden || track.type == "audio") continue;
        for (std::size_t clipIndex = 0; clipIndex < track.clips.size(); ++clipIndex) {
            checkCancellation(cancellation);
            const auto& clip = track.clips[clipIndex];
            if (&clip == selectedClip || clip.mediaType == "audio") continue;
            const std::string pointer = "/timelines/"
                + std::to_string(selectedTimelineIndex)
                + "/tracks/"
                + std::to_string(trackIndex)
                + "/clips/"
                + std::to_string(clipIndex);
            if (clip.startFrame < 0
                || clip.durationFrames <= 0
                || clip.startFrame > std::numeric_limits<std::int64_t>::max()
                    - clip.durationFrames) {
                fail(
                    "unsupportedClipTiming",
                    pointer,
                    "visible layer timing is invalid"
                );
            }
            const auto clipEnd = clip.startFrame + clip.durationFrames;
            if (clip.startFrame < selectedEnd
                && selectedClip->startFrame < clipEnd) {
                fail(
                    "overlappingVisibleLayer",
                    pointer,
                    "exclusive static rendering found another visible layer"
                );
            }
        }
    }
    return result;
}

StaticVideoTimeline compileStaticVideoTimeline(
    const project::ProjectDocument& document,
    std::string_view timelineId,
    std::stop_token cancellation
) {
    checkCancellation(cancellation);
    const auto* timeline = uniquePersistedEntity(
        document.project().timelines,
        timelineId,
        "/timelines",
        cancellation
    );
    if (
        timeline->fps > std::numeric_limits<std::int32_t>::max()
        || timeline->width > std::numeric_limits<std::uint32_t>::max()
        || timeline->height > std::numeric_limits<std::uint32_t>::max()
    ) {
        fail("unsupportedTimelineSettings", "/timelines", "timeline exceeds render domains");
    }

    std::size_t timelineIndex = 0;
    for (; timelineIndex < document.project().timelines.size(); ++timelineIndex) {
        checkCancellation(cancellation);
        if (&document.project().timelines[timelineIndex] == timeline) break;
    }

    struct OrderedClip final {
        const project::Track* track;
        const project::Clip* clip;
        std::size_t trackIndex;
        std::size_t clipIndex;
        std::string pointer;
    };
    std::vector<OrderedClip> ordered;
    std::set<std::string> clipIds;
    for (std::size_t trackIndex = 0; trackIndex < timeline->tracks.size(); ++trackIndex) {
        checkCancellation(cancellation);
        const auto& track = timeline->tracks[trackIndex];
        if (track.hidden || track.type == "audio") continue;
        for (std::size_t clipIndex = 0; clipIndex < track.clips.size(); ++clipIndex) {
            checkCancellation(cancellation);
            const auto& clip = track.clips[clipIndex];
            if (clip.mediaType == "audio") continue;
            const auto pointer = "/timelines/" + std::to_string(timelineIndex)
                + "/tracks/" + std::to_string(trackIndex)
                + "/clips/" + std::to_string(clipIndex);
            if (!clipIds.insert(clip.id.value).second) {
                fail(
                    "duplicateStableId",
                    pointer + "/id",
                    "static video timeline clip identity is ambiguous"
                );
            }
            if (
                clip.startFrame < 0
                || clip.durationFrames <= 0
                || clip.startFrame > std::numeric_limits<std::int64_t>::max()
                    - clip.durationFrames
            ) {
                fail("unsupportedClipTiming", pointer, "visible layer timing is invalid");
            }
            ordered.push_back({&track, &clip, trackIndex, clipIndex, pointer});
            if (ordered.size() > maximumStaticVideoTimelineSegments) {
                fail(
                    "resourceLimitExceeded",
                    "/timelines/" + std::to_string(timelineIndex) + "/tracks",
                    "static video timeline exceeds 256 visible segments"
                );
            }
        }
    }
    if (ordered.empty()) {
        fail(
            "noVisibleVideoSegments",
            "/timelines/" + std::to_string(timelineIndex) + "/tracks",
            "static video timeline requires one visible segment"
        );
    }
    std::stable_sort(ordered.begin(), ordered.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.clip->startFrame != rhs.clip->startFrame) {
            return lhs.clip->startFrame < rhs.clip->startFrame;
        }
        if (lhs.trackIndex != rhs.trackIndex) return lhs.trackIndex < rhs.trackIndex;
        return lhs.clipIndex < rhs.clipIndex;
    });
    std::int64_t previousEnd = 0;
    bool hasPrevious = false;
    for (const auto& candidate : ordered) {
        checkCancellation(cancellation);
        if (hasPrevious && candidate.clip->startFrame < previousEnd) {
            fail(
                "overlappingVisibleLayer",
                candidate.pointer,
                "static video timeline found overlapping visible layers"
            );
        }
        previousEnd = candidate.clip->startFrame + candidate.clip->durationFrames;
        hasPrevious = true;
    }

    StaticVideoTimeline result{
        static_cast<std::uint32_t>(timeline->width),
        static_cast<std::uint32_t>(timeline->height),
        static_cast<std::int32_t>(timeline->fps),
        timeline->id.value,
        previousEnd,
        {},
    };
    result.segments.reserve(ordered.size());
    for (const auto& candidate : ordered) {
        checkCancellation(cancellation);
        result.segments.push_back(compileStaticVideoLayer(
            document,
            timelineId,
            candidate.track->id.value,
            candidate.clip->id.value,
            cancellation
        ));
    }
    return result;
}

const StaticVideoLayer* staticVideoLayerAt(
    const StaticVideoTimeline& timeline,
    std::int64_t timelineFrame
) noexcept {
    if (timelineFrame < 0 || timelineFrame >= timeline.durationFrames) return nullptr;
    const auto found = std::upper_bound(
        timeline.segments.begin(),
        timeline.segments.end(),
        timelineFrame,
        [](std::int64_t frame, const StaticVideoLayer& layer) {
            return frame < layer.timelineStartFrame;
        }
    );
    if (found == timeline.segments.begin()) return nullptr;
    const auto& candidate = *(found - 1);
    if (
        candidate.timelineStartFrame < 0
        || candidate.durationFrames <= 0
        || candidate.timelineStartFrame > std::numeric_limits<std::int64_t>::max()
            - candidate.durationFrames
        || timelineFrame < candidate.timelineStartFrame
        || timelineFrame >= candidate.timelineStartFrame + candidate.durationFrames
    ) {
        return nullptr;
    }
    return &candidate;
}

render::RenderPlan makeRenderPlan(
    const StaticVideoLayer& layer,
    std::int64_t timelineFrame
) {
    if (
        timelineFrame < layer.timelineStartFrame
        || layer.durationFrames <= 0
        || timelineFrame - layer.timelineStartFrame >= layer.durationFrames
    ) {
        fail("inactiveTimelineFrame", "/timelineFrame", "clip is inactive at the requested frame");
    }
    const auto offset = timelineFrame - layer.timelineStartFrame;
    if (layer.sourceStartFrame > std::numeric_limits<std::int64_t>::max() - offset) {
        fail("sourceFrameOverflow", "/timelineFrame", "source frame overflowed");
    }
    return render::RenderPlan::create(
        layer.canvasWidth,
        layer.canvasHeight,
        layer.framesPerSecond,
        timelineFrame,
        {{
            layer.clipId,
            layer.trackId,
            layer.mediaId,
            layer.sourceStartFrame + offset,
            layer.transform,
            layer.opacity,
            render::BlendMode::normal,
            layer.exposureEv,
        }}
    );
}

render::RenderPlan makeRenderPlan(
    const StaticVideoTimeline& timeline,
    std::int64_t timelineFrame
) {
    if (timelineFrame < 0 || timelineFrame >= timeline.durationFrames) {
        fail(
            "inactiveTimelineFrame",
            "/timelineFrame",
            "timeline is inactive at the requested frame"
        );
    }
    const auto* layer = staticVideoLayerAt(timeline, timelineFrame);
    if (layer != nullptr) return makeRenderPlan(*layer, timelineFrame);
    try {
        return render::RenderPlan::create(
            timeline.canvasWidth,
            timeline.canvasHeight,
            timeline.framesPerSecond,
            timelineFrame,
            {}
        );
    } catch (const render::RenderError& error) {
        fail("unsupportedRenderPlan", error.pointer, error.what());
    }
}

}
