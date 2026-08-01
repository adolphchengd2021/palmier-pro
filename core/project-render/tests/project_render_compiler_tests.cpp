#include "palmier/project/project_reader.hpp"
#include "palmier/project_render/project_render_compiler.hpp"
#include "palmier/render/cpu_renderer.hpp"

#include <iostream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>

namespace {

using palmier::project_render::ProjectRenderCompileError;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

palmier::project::ProjectDocument document(
    std::string_view clipProperties,
    std::string_view trackProperties = {},
    std::string_view blendMode = "normal",
    std::string_view speed = "1",
    std::string_view opacity = "0.5",
    std::string_view startFrame = "12",
    std::string_view durationFrames = "10",
    std::string_view trimStartFrame = "2"
) {
    const auto source = std::string(R"({
        "timelines":[{
            "id":"timeline","fps":30,"width":4,"height":4,
            "tracks":[{
                "id":"track","type":"video",)")
        + std::string(trackProperties)
        + R"("clips":[{
                    "id":"clip","mediaRef":"media","mediaType":"video",
                    "sourceClipType":"video","startFrame":)"
        + std::string(startFrame)
        + R"(,"durationFrames":)"
        + std::string(durationFrames)
        + R"(,"trimStartFrame":)"
        + std::string(trimStartFrame)
        + R"(,"trimEndFrame":3,"speed":)"
        + std::string(speed)
        + R"(,"opacity":)"
        + std::string(opacity)
        + R"(,"blendMode":")"
        + std::string(blendMode)
        + R"(",)"
        + std::string(clipProperties)
        + R"(}]
            }]
        }],
        "activeTimelineId":"timeline"
    })";
    return palmier::project::readProject(source, [] {
        return std::string("unexpected-generated-id");
    });
}

template<typename Operation>
void requireCompileError(
    Operation operation,
    std::string_view code,
    std::string_view pointer = {}
) {
    try {
        operation();
    } catch (const ProjectRenderCompileError& error) {
        require(error.code == code, "unexpected compiler error " + error.code);
        if (!pointer.empty()) {
            require(error.jsonPointer == pointer, "unexpected compiler pointer " + error.jsonPointer);
        }
        return;
    }
    throw std::runtime_error("expected project render compiler failure");
}

palmier::project::ProjectDocument rawDocument(std::string_view source) {
    return palmier::project::readProject(source, [] {
        return std::string("generated-id");
    });
}

void staticProjectPropertiesDriveOnePlan() {
    const auto project = document(R"(
        "transform":{
            "centerX":0.25,"centerY":0.75,"width":0.5,"height":0.25,
            "rotation":15,"flipHorizontal":false,"flipVertical":false
        },
        "crop":{"left":0,"top":0,"right":0,"bottom":0},
        "edgeRounding":0,"edgeSoftness":0,
        "effects":[{
            "id":"effect","type":"color.exposure","enabled":true,
            "params":{"ev":{"value":1}}
        }]
    )");
    const auto compiled = palmier::project_render::compileStaticVideoLayer(
        project,
        "timeline",
        "track",
        "clip"
    );
    require(compiled.canvasWidth == 4 && compiled.canvasHeight == 4, "canvas changed");
    require(compiled.framesPerSecond == 30, "frame rate changed");
    require(compiled.timelineStartFrame == 12, "timeline start changed");
    require(compiled.durationFrames == 10, "duration changed");
    require(compiled.sourceStartFrame == 2, "source start changed");
    require(compiled.transform.centerX == 0.25F, "center X changed");
    require(compiled.transform.centerY == 0.75F, "center Y changed");
    require(compiled.transform.width == 0.5F, "width changed");
    require(compiled.transform.height == 0.25F, "height changed");
    require(compiled.transform.rotationDegrees == 15, "rotation changed");
    require(compiled.opacity == 0.5F, "opacity changed");
    require(compiled.exposureEv == 1, "exposure changed");

    const auto plan = palmier::project_render::makeRenderPlan(compiled, 15);
    require(plan.timelineFrame() == 15, "plan timeline frame changed");
    require(plan.layers().size() == 1, "plan layer count changed");
    const auto& layer = plan.layers().front();
    require(layer.id == "clip" && layer.trackId == "track", "stable IDs changed");
    require(layer.mediaId == "media", "media ID changed");
    require(layer.sourceFrame == 5, "source frame mapping changed");
    require(layer.blendMode == palmier::render::BlendMode::normal, "blend changed");
}

void projectPlanKeepsPreviewExportParity() {
    const auto project = document(R"(
        "transform":{
            "centerX":0.5,"centerY":0.5,"width":1,"height":1,
            "rotation":0,"flipHorizontal":false,"flipVertical":false
        },
        "effects":[{"type":"color.exposure","params":{"ev":{"value":1}}}]
    )");
    const auto compiled = palmier::project_render::compileStaticVideoLayer(
        project,
        "timeline",
        "track",
        "clip"
    );
    const auto plan = palmier::project_render::makeRenderPlan(compiled, 12);
    const palmier::render::SourceFrame source{
        1,
        1,
        {{0.25F, 0.5F, 0.75F, 1}},
    };
    const auto resolver = [&](std::string_view mediaId, std::int64_t sourceFrame)
        -> const palmier::render::SourceFrame* {
        return mediaId == "media" && sourceFrame == 2 ? &source : nullptr;
    };
    palmier::render::CpuRenderer renderer;
    const auto preview = palmier::render::renderPreviewFrame(plan, resolver, renderer);
    const auto exported = palmier::render::renderExportFrame(plan, resolver, renderer);
    require(preview.width == exported.width && preview.height == exported.height, "sizes differ");
    require(preview.pixels == exported.pixels, "project preview/export pixels differ");
    require(
        preview.pixels[5].red > 0
            && preview.pixels[5].red < source.pixels.front().red,
        "exposure and opacity were not applied"
    );
    require(preview.pixels[5].alpha == 1, "opacity composite alpha changed");
}

void unsupportedVisualsAreRefused() {
    const auto crop = document(R"("crop":{"left":0.1,"top":0,"right":0,"bottom":0})");
    requireCompileError(
        [&] { static_cast<void>(palmier::project_render::compileStaticVideoLayer(
            crop, "timeline", "track", "clip")); },
        "unsupportedMasking"
    );

    const auto flip = document(R"("transform":{"flipHorizontal":true})");
    requireCompileError(
        [&] { static_cast<void>(palmier::project_render::compileStaticVideoLayer(
            flip, "timeline", "track", "clip")); },
        "unsupportedFlip"
    );

    const auto animated = document(R"(
        "opacityTrack":{"keyframes":[{"frame":0,"value":0.5}]}
    )");
    requireCompileError(
        [&] { static_cast<void>(palmier::project_render::compileStaticVideoLayer(
            animated, "timeline", "track", "clip")); },
        "dynamicVisualsUnsupported"
    );

    const auto unsupportedEffect = document(R"(
        "effects":[{"type":"color.contrast","params":{}}]
    )");
    requireCompileError(
        [&] { static_cast<void>(palmier::project_render::compileStaticVideoLayer(
            unsupportedEffect, "timeline", "track", "clip")); },
        "unsupportedEffect"
    );

    const auto animatedEffect = document(R"(
        "effects":[{"type":"color.exposure","params":{"ev":{
            "value":0,"track":{"keyframes":[{"frame":0,"value":1}]}
        }}}]
    )");
    requireCompileError(
        [&] { static_cast<void>(palmier::project_render::compileStaticVideoLayer(
            animatedEffect, "timeline", "track", "clip")); },
        "dynamicEffectUnsupported"
    );

    const auto fade = document(R"("fadeInFrames":1)");
    requireCompileError(
        [&] { static_cast<void>(palmier::project_render::compileStaticVideoLayer(
            fade, "timeline", "track", "clip")); },
        "dynamicVisualsUnsupported"
    );

    const auto nonNormal = document(R"("transform":{})", {}, "multiply");
    requireCompileError(
        [&] { static_cast<void>(palmier::project_render::compileStaticVideoLayer(
            nonNormal, "timeline", "track", "clip")); },
        "unsupportedBlendMode"
    );

    const auto outOfRangeExposure = document(R"(
        "effects":[{"type":"color.exposure","params":{"ev":{"value":4}}}]
    )");
    requireCompileError(
        [&] { static_cast<void>(palmier::project_render::compileStaticVideoLayer(
            outOfRangeExposure, "timeline", "track", "clip")); },
        "unsupportedEffectValue"
    );

    const auto multipleEffects = document(R"(
        "effects":[
            {"type":"color.exposure","params":{"ev":{"value":1}}},
            {"type":"color.exposure","params":{"ev":{"value":1}}}
        ]
    )");
    requireCompileError(
        [&] { static_cast<void>(palmier::project_render::compileStaticVideoLayer(
            multipleEffects, "timeline", "track", "clip")); },
        "unsupportedEffect"
    );
}

void lifecycleAndFrameBoundariesAreRefused() {
    const auto hidden = document(R"("transform":{})", R"("hidden":true,)");
    requireCompileError(
        [&] { static_cast<void>(palmier::project_render::compileStaticVideoLayer(
            hidden, "timeline", "track", "clip")); },
        "unsupportedTrack"
    );

    const auto project = document(R"("transform":{})");
    const auto compiled = palmier::project_render::compileStaticVideoLayer(
        project,
        "timeline",
        "track",
        "clip"
    );
    requireCompileError(
        [&] { static_cast<void>(palmier::project_render::makeRenderPlan(compiled, 11)); },
        "inactiveTimelineFrame"
    );
    requireCompileError(
        [&] { static_cast<void>(palmier::project_render::makeRenderPlan(compiled, 22)); },
        "inactiveTimelineFrame"
    );

    std::stop_source cancellation;
    cancellation.request_stop();
    requireCompileError(
        [&] { static_cast<void>(palmier::project_render::compileStaticVideoLayer(
            project, "timeline", "track", "clip", cancellation.get_token())); },
        "cancelled"
    );
}

void identityTimingAndNumericRefusals() {
    const auto synthesized = rawDocument(R"({
        "timelines":[{
            "id":"timeline","fps":30,"width":4,"height":4,
            "tracks":[{"id":"track","type":"video","clips":[{
                "mediaRef":"media","mediaType":"video","sourceClipType":"video",
                "startFrame":0,"durationFrames":1
            }]}]
        }],"activeTimelineId":"timeline"
    })");
    requireCompileError(
        [&] { static_cast<void>(palmier::project_render::compileStaticVideoLayer(
            synthesized, "timeline", "track", "generated-id")); },
        "unstableEntityId",
        "/clips"
    );

    const auto duplicate = rawDocument(R"({
        "timelines":[{
            "id":"timeline","fps":30,"width":4,"height":4,
            "tracks":[{"id":"track","type":"video","clips":[
                {"id":"clip","mediaRef":"one","mediaType":"video","sourceClipType":"video","startFrame":0,"durationFrames":1},
                {"id":"clip","mediaRef":"two","mediaType":"video","sourceClipType":"video","startFrame":1,"durationFrames":1}
            ]}]
        }],"activeTimelineId":"timeline"
    })");
    requireCompileError(
        [&] { static_cast<void>(palmier::project_render::compileStaticVideoLayer(
            duplicate, "timeline", "track", "clip")); },
        "duplicateStableId",
        "/clips"
    );

    const auto nonUnitSpeed = document(R"("transform":{})", {}, "normal", "2");
    requireCompileError(
        [&] { static_cast<void>(palmier::project_render::compileStaticVideoLayer(
            nonUnitSpeed, "timeline", "track", "clip")); },
        "unsupportedClipTiming"
    );
    const auto overflowingTimeline = document(
        R"("transform":{})",
        {},
        "normal",
        "1",
        "0.5",
        "9223372036854775800",
        "10"
    );
    requireCompileError(
        [&] { static_cast<void>(palmier::project_render::compileStaticVideoLayer(
            overflowingTimeline, "timeline", "track", "clip")); },
        "unsupportedClipTiming"
    );
    const auto overflowingSource = document(
        R"("transform":{})",
        {},
        "normal",
        "1",
        "0.5",
        "12",
        "10",
        "9223372036854775800"
    );
    requireCompileError(
        [&] { static_cast<void>(palmier::project_render::compileStaticVideoLayer(
            overflowingSource, "timeline", "track", "clip")); },
        "unsupportedClipTiming"
    );
    for (const auto opacity : {"-0.1", "1.1"}) {
        const auto invalidOpacity = document(
            R"("transform":{})", {}, "normal", "1", opacity
        );
        requireCompileError(
            [&] { static_cast<void>(palmier::project_render::compileStaticVideoLayer(
                invalidOpacity, "timeline", "track", "clip")); },
            "unsupportedOpacity"
        );
    }
    const auto zeroWidth = document(R"("transform":{"width":0})");
    requireCompileError(
        [&] { static_cast<void>(palmier::project_render::compileStaticVideoLayer(
            zeroWidth, "timeline", "track", "clip")); },
        "unsupportedRenderPlan",
        "/layers/0/transform"
    );
}

void malformedVisualValuesAreRefusedInsteadOfDefaulted() {
    struct Case final {
        std::string_view properties;
        std::string_view pointer;
    };
    const Case cases[]{
        {R"("transform":{"width":"bad"})", "/timelines/0/tracks/0/clips/0/transform/width"},
        {R"("crop":{"left":0,"top":0,"right":0})", "/timelines/0/tracks/0/clips/0/crop"},
        {R"("edgeRounding":"bad")", "/timelines/0/tracks/0/clips/0/edgeRounding"},
        {R"("fadeInFrames":"bad")", "/timelines/0/tracks/0/clips/0/fadeInFrames"},
        {
            R"("effects":[{"type":"color.exposure","params":{"ev":{"value":"bad"}}}])",
            "/timelines/0/tracks/0/clips/0/effects/0/params/ev/value",
        },
    };
    for (const auto& value : cases) {
        const auto project = document(value.properties);
        requireCompileError(
            [&] { static_cast<void>(palmier::project_render::compileStaticVideoLayer(
                project, "timeline", "track", "clip")); },
            "malformedVisualProperty",
            value.pointer
        );
    }
    const auto malformedOpacity = document(
        R"("transform":{})", {}, "normal", "1", R"("bad")"
    );
    requireCompileError(
        [&] { static_cast<void>(palmier::project_render::compileStaticVideoLayer(
            malformedOpacity, "timeline", "track", "clip")); },
        "malformedVisualProperty",
        "/timelines/0/tracks/0/clips/0/opacity"
    );
    const auto malformedSpeed = document(
        R"("transform":{})", {}, "normal", R"("bad")"
    );
    requireCompileError(
        [&] { static_cast<void>(palmier::project_render::compileStaticVideoLayer(
            malformedSpeed, "timeline", "track", "clip")); },
        "malformedVisualProperty",
        "/timelines/0/tracks/0/clips/0/speed"
    );
    const auto malformedVisibility = document(
        R"("transform":{})", R"("hidden":"bad",)"
    );
    requireCompileError(
        [&] { static_cast<void>(palmier::project_render::compileStaticVideoLayer(
            malformedVisibility, "timeline", "track", "clip")); },
        "malformedVisualProperty",
        "/timelines/0/tracks/0/hidden"
    );
}

}

int wmain(int argumentCount, wchar_t*[]) {
    try {
        require(argumentCount == 2, "expected repository root");
        staticProjectPropertiesDriveOnePlan();
        projectPlanKeepsPreviewExportParity();
        unsupportedVisualsAreRefused();
        lifecycleAndFrameBoundariesAreRefused();
        identityTimingAndNumericRefusals();
        malformedVisualValuesAreRefusedInsteadOfDefaulted();
        std::cout << "PALMIER_PROJECT_RENDER_COMPILER_TESTS_OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "PALMIER_PROJECT_RENDER_COMPILER_TESTS_FAILED "
                  << error.what() << '\n';
        return 1;
    }
}
