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

void exclusiveCompilerRefusesAnotherVisibleLayer() {
    const auto overlapping = rawDocument(R"({
        "timelines":[{
            "id":"timeline","fps":30,"width":4,"height":4,
            "tracks":[
                {"id":"track","type":"video","clips":[{
                    "id":"clip","mediaRef":"media","mediaType":"video",
                    "sourceClipType":"video","startFrame":0,"durationFrames":10,
                    "trimStartFrame":0,"trimEndFrame":0,"speed":1,
                    "opacity":1,"blendMode":"normal"
                }]},
                {"id":"text-track","type":"text","clips":[{
                    "id":"text","mediaRef":"title","mediaType":"text",
                    "sourceClipType":"text","startFrame":4,"durationFrames":2
                }]}
            ]
        }],
        "activeTimelineId":"timeline"
    })");
    static_cast<void>(palmier::project_render::compileStaticVideoLayer(
        overlapping,
        "timeline",
        "track",
        "clip"
    ));
    requireCompileError(
        [&] {
            static_cast<void>(
                palmier::project_render::compileExclusiveStaticVideoLayer(
                    overlapping,
                    "timeline",
                    "track",
                    "clip"
                )
            );
        },
        "overlappingVisibleLayer",
        "/timelines/0/tracks/1/clips/0"
    );

    const auto hidden = rawDocument(R"({
        "timelines":[{
            "id":"timeline","fps":30,"width":4,"height":4,
            "tracks":[
                {"id":"track","type":"video","clips":[{
                    "id":"clip","mediaRef":"media","mediaType":"video",
                    "sourceClipType":"video","startFrame":0,"durationFrames":10,
                    "trimStartFrame":0,"trimEndFrame":0,"speed":1,
                    "opacity":1,"blendMode":"normal"
                }]},
                {"id":"hidden","type":"text","hidden":true,"clips":[{
                    "id":"text","mediaRef":"title","mediaType":"text",
                    "sourceClipType":"text","startFrame":4,"durationFrames":2
                }]}
            ]
        }],
        "activeTimelineId":"timeline"
    })");
    static_cast<void>(palmier::project_render::compileExclusiveStaticVideoLayer(
        hidden,
        "timeline",
        "track",
        "clip"
    ));
}

void staticVideoTimelineOrdersSegmentsAndRepresentsGaps() {
    const auto project = rawDocument(R"({
        "timelines":[{
            "id":"timeline","fps":30,"width":4,"height":4,
            "tracks":[
                {"id":"late-track","type":"video","clips":[{
                    "id":"late","mediaRef":"late-media","mediaType":"video",
                    "sourceClipType":"video","startFrame":8,"durationFrames":2,
                    "trimStartFrame":3,"trimEndFrame":0,"speed":1,
                    "opacity":1,"blendMode":"normal"
                }]},
                {"id":"early-track","type":"video","clips":[{
                    "id":"early","mediaRef":"early-media","mediaType":"video",
                    "sourceClipType":"video","startFrame":2,"durationFrames":3,
                    "trimStartFrame":1,"trimEndFrame":0,"speed":1,
                    "opacity":1,"blendMode":"normal"
                },{
                    "id":"adjacent","mediaRef":"adjacent-media","mediaType":"video",
                    "sourceClipType":"video","startFrame":5,"durationFrames":2,
                    "trimStartFrame":0,"trimEndFrame":0,"speed":1,
                    "opacity":1,"blendMode":"normal"
                }]}
            ]
        }],
        "activeTimelineId":"timeline"
    })");
    const auto timeline = palmier::project_render::compileStaticVideoTimeline(
        project,
        "timeline"
    );
    require(timeline.timelineId == "timeline", "timeline identity changed");
    require(timeline.durationFrames == 10, "timeline duration changed");
    require(timeline.segments.size() == 3, "timeline segment count changed");
    require(timeline.segments[0].clipId == "early", "early segment was not sorted");
    require(timeline.segments[1].clipId == "adjacent", "adjacent segment was not sorted");
    require(timeline.segments[2].clipId == "late", "late segment was not sorted");

    require(
        palmier::project_render::staticVideoLayerAt(timeline, 0) == nullptr,
        "leading gap became active"
    );
    require(
        palmier::project_render::staticVideoLayerAt(timeline, 4)->clipId == "early",
        "early end-inclusive frame changed"
    );
    require(
        palmier::project_render::staticVideoLayerAt(timeline, 5)->clipId == "adjacent",
        "adjacent boundary did not switch segments"
    );
    require(
        palmier::project_render::staticVideoLayerAt(timeline, 7) == nullptr,
        "inter-clip gap became active"
    );

    const auto leadingGap = palmier::project_render::makeRenderPlan(timeline, 0);
    require(leadingGap.layers().empty(), "leading gap did not produce a black plan");
    const auto early = palmier::project_render::makeRenderPlan(timeline, 2);
    require(early.layers().size() == 1, "active segment did not produce one layer");
    require(early.layers().front().id == "early", "active segment identity changed");
    require(early.layers().front().sourceFrame == 1, "active source frame changed");
    const auto gap = palmier::project_render::makeRenderPlan(timeline, 7);
    require(gap.layers().empty(), "inter-clip gap did not produce a black plan");
    const auto late = palmier::project_render::makeRenderPlan(timeline, 9);
    require(late.layers().front().id == "late", "late segment identity changed");
    require(late.layers().front().sourceFrame == 4, "late source frame changed");
    requireCompileError(
        [&] { static_cast<void>(palmier::project_render::makeRenderPlan(timeline, 10)); },
        "inactiveTimelineFrame",
        "/timelineFrame"
    );

    palmier::render::CpuRenderer renderer;
    const auto black = palmier::render::renderPreviewFrame(
        gap,
        [](std::string_view, std::int64_t) -> const palmier::render::SourceFrame* {
            return nullptr;
        },
        renderer
    );
    require(black.pixels.size() == 16, "gap render size changed");
    for (const auto& pixel : black.pixels) {
        require(pixel == palmier::render::Rgba32Float{0, 0, 0, 1}, "gap was not opaque black");
    }
}

void staticVideoTimelineRefusesOverlapUnsupportedContentAndCapacity() {
    const auto overlapping = rawDocument(R"({
        "timelines":[{"id":"timeline","fps":30,"width":4,"height":4,
            "tracks":[{"id":"track","type":"video","clips":[
                {"id":"first","mediaRef":"first","mediaType":"video",
                 "sourceClipType":"video","startFrame":0,"durationFrames":5},
                {"id":"second","mediaRef":"second","mediaType":"video",
                 "sourceClipType":"video","startFrame":4,"durationFrames":2}
            ]}]}],"activeTimelineId":"timeline"
    })");
    requireCompileError(
        [&] { static_cast<void>(palmier::project_render::compileStaticVideoTimeline(
            overlapping, "timeline")); },
        "overlappingVisibleLayer",
        "/timelines/0/tracks/0/clips/1"
    );

    const auto unsupported = rawDocument(R"({
        "timelines":[{"id":"timeline","fps":30,"width":4,"height":4,
            "tracks":[
                {"id":"video-track","type":"video","clips":[{
                    "id":"video","mediaRef":"video","mediaType":"video",
                    "sourceClipType":"video","startFrame":0,"durationFrames":2
                }]},
                {"id":"text-track","type":"text","clips":[{
                    "id":"text","mediaRef":"text","mediaType":"text",
                    "sourceClipType":"text","startFrame":3,"durationFrames":2
                }]}
            ]
        }],"activeTimelineId":"timeline"
    })");
    requireCompileError(
        [&] { static_cast<void>(palmier::project_render::compileStaticVideoTimeline(
            unsupported, "timeline")); },
        "unsupportedTrack"
    );

    std::string dense = R"({"timelines":[{"id":"timeline","fps":30,"width":4,"height":4,"tracks":[{"id":"track","type":"video","clips":[)";
    for (std::size_t index = 0;
         index <= palmier::project_render::maximumStaticVideoTimelineSegments;
         ++index) {
        if (index != 0) dense += ',';
        dense += "{\"id\":\"clip-" + std::to_string(index)
            + "\",\"mediaRef\":\"media-" + std::to_string(index)
            + "\",\"mediaType\":\"video\",\"sourceClipType\":\"video\",\"startFrame\":"
            + std::to_string(index) + ",\"durationFrames\":1}";
    }
    dense += R"(]}]}],"activeTimelineId":"timeline"})";
    const auto denseProject = rawDocument(dense);
    requireCompileError(
        [&] { static_cast<void>(palmier::project_render::compileStaticVideoTimeline(
            denseProject, "timeline")); },
        "resourceLimitExceeded",
        "/timelines/0/tracks"
    );

    std::stop_source cancellation;
    cancellation.request_stop();
    requireCompileError(
        [&] { static_cast<void>(palmier::project_render::compileStaticVideoTimeline(
            overlapping, "timeline", cancellation.get_token())); },
        "cancelled"
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
        exclusiveCompilerRefusesAnotherVisibleLayer();
        staticVideoTimelineOrdersSegmentsAndRepresentsGaps();
        staticVideoTimelineRefusesOverlapUnsupportedContentAndCapacity();
        std::cout << "PALMIER_PROJECT_RENDER_COMPILER_TESTS_OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "PALMIER_PROJECT_RENDER_COMPILER_TESTS_FAILED "
                  << error.what() << '\n';
        return 1;
    }
}
