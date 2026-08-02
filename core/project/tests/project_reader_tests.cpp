#include "palmier/project/project_reader.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace {

using palmier::json::Value;
using palmier::project::EntityIdOrigin;
using palmier::project::ReadError;

std::string readFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("cannot open fixture");
    }
    return {
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()
    };
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

const Value& at(const Value& value, const std::string& key) {
    const auto* child = value.find(key);
    if (!child) {
        throw std::runtime_error("missing JSON field " + key);
    }
    return *child;
}

template<typename Operation>
void requireReadError(Operation operation, const std::string& code) {
    try {
        operation();
    } catch (const ReadError& error) {
        require(error.code == code, "unexpected project error code");
        return;
    }
    throw std::runtime_error("expected project read failure");
}

void currentFixture(const std::filesystem::path& root) {
    const auto path = root / "fixtures/contracts/projects/current-multitimeline.palmier/project.json";
    auto document = palmier::project::readProject(readFile(path), [] {
        return std::string("unexpected-generated-id");
    });
    require(document.rootKind() == palmier::project::RootKind::current, "current root kind");
    require(
        document.disposition() == palmier::project::ProjectDocumentDisposition::safeEdits,
        "reader disposition"
    );
    require(document.diagnostics().empty(), "unexpected current diagnostics");
    require(document.project().timelines.size() == 2, "current timeline count");
    require(document.project().activeTimelineId == "timeline-main", "current active timeline");
    require(document.project().openTimelineIds.size() == 2, "current open timelines");
    const auto& timeline = document.project().timelines.front();
    require(timeline.id.value == "timeline-main", "current timeline ID");
    require(timeline.id.origin == EntityIdOrigin::persisted, "current timeline ID origin");
    require(timeline.fps == 30 && timeline.width == 1920 && timeline.height == 1080, "current dimensions");
    const auto& track = timeline.tracks.front();
    require(track.id.value == "track-main-v1" && track.type == "video", "current track");
    const auto& clip = track.clips.front();
    require(clip.id.value == "clip-main-1", "current clip ID");
    require(clip.startFrame == 0 && clip.durationFrames == 150, "current clip frames");
    require(clip.blendMode == "normal", "current blend mode");
}

void legacyFixture(const std::filesystem::path& root) {
    const auto path = root / "fixtures/contracts/projects/legacy-bare-timeline.palmier/project.json";
    auto document = palmier::project::readProject(readFile(path), [] {
        return std::string("unexpected-generated-id");
    });
    require(document.rootKind() == palmier::project::RootKind::legacy, "legacy root kind");
    require(document.project().timelines.size() == 1, "legacy timeline count");
    require(document.project().activeTimelineId == "timeline-legacy", "legacy active timeline");
    require(document.project().openTimelineIds == std::vector<std::string>{"timeline-legacy"}, "legacy open timeline");
    const auto& clip = document.project().timelines.front().tracks.front().clips.front();
    require(clip.id.value == "clip-legacy-1", "legacy clip ID");
    require(clip.startFrame == 0 && clip.durationFrames == 90, "legacy frames");
    require(clip.mediaType == "video" && clip.sourceClipType == "video", "legacy clip defaults");
}

void unknownSourceIsRetained(const std::filesystem::path& root) {
    const auto path = root / "fixtures/contracts/projects/unknown-fields.palmier/project.json";
    const auto input = readFile(path);
    auto document = palmier::project::readProject(input, [] {
        return std::string("unexpected-generated-id");
    });
    require(at(document.source(), "x-contract-null").kind() == Value::Kind::nullValue, "unknown null");
    const auto& timelines = at(document.source(), "timelines").array();
    require(at(timelines.front(), "x-contract-timeline").string() == "timeline", "unknown timeline");
    const auto& tracks = at(timelines.front(), "tracks").array();
    require(at(tracks.front(), "x-contract-track").boolean(), "unknown track");
    const auto& clips = at(tracks.front(), "clips").array();
    require(at(clips.front(), "x-contract-clip").array().size() == 2, "unknown clip");
    const auto canonical = palmier::json::canonical(document.source());
    require(palmier::json::canonical(palmier::json::parse(canonical)) == canonical, "canonical source replay");
}

void synthesizedIdsAndDefaults() {
    const std::string source = R"({
        "timelines":[{
            "fps":30,"width":1920,"height":1080,"tracks":[{
                "type":"video","displayHeight":500,"clips":[{
                    "mediaRef":"media-1","startFrame":0,"durationFrames":30,
                    "mediaType":"future","blendMode":"future"
                }]
            }]
        }],
        "activeTimelineId":"missing","openTimelineIds":["missing"]
    })";
    int next = 0;
    auto document = palmier::project::readProject(source, [&] {
        return "generated-" + std::to_string(++next);
    });
    const auto& timeline = document.project().timelines.front();
    const auto& track = timeline.tracks.front();
    const auto& clip = track.clips.front();
    require(timeline.id.value == "generated-1", "synthesized timeline ID");
    require(track.id.value == "generated-2", "synthesized track ID");
    require(clip.id.value == "generated-3", "synthesized clip ID");
    require(timeline.id.origin == EntityIdOrigin::synthesized, "synthesized origin");
    require(timeline.name == "Timeline 1" && !timeline.settingsConfigured, "timeline defaults");
    require(track.displayHeight == 200 && track.syncLocked, "track defaults and clamp");
    require(clip.mediaType == "video" && !clip.blendMode, "clip enum defaults");
    require(document.project().activeTimelineId == "generated-1", "active fallback");
    require(document.project().openTimelineIds == std::vector<std::string>{"generated-1"}, "open fallback");
    require(document.diagnostics().size() >= 7, "expected reader diagnostics");
    const auto normalized = palmier::project::normalizedModelJson(document);
    require(at(palmier::json::parse(normalized), "disposition").string() == "readOnly", "normalized disposition");
}

void invalidOptionalClipListDefaultsAsSwift() {
    const std::string source = R"({
        "timelines":[{
            "id":"timeline","fps":30,"width":1920,"height":1080,"tracks":[{
                "id":"track","type":"video","clips":[{"durationFrames":30,"startFrame":0}]
            }]
        }]
    })";
    auto document = palmier::project::readProject(source, [] { return std::string("generated"); });
    require(document.project().timelines.front().tracks.front().clips.empty(), "invalid optional clips default");
    require(
        document.diagnostics().back().code == "invalidOptionalDefaulted",
        "invalid optional clips diagnostic"
    );
}

void optionalClipFailurePreservesGeneratorOrder() {
    const std::string source = R"({
        "timelines":[{
            "id":"timeline","fps":30,"width":1920,"height":1080,"tracks":[
                {"id":"track-1","type":"video","clips":[{"mediaRef":"media-1","durationFrames":30}]},
                {"type":"audio"}
            ]
        }]
    })";
    int next = 0;
    auto document = palmier::project::readProject(source, [&] {
        return "generated-" + std::to_string(++next);
    });
    const auto& tracks = document.project().timelines.front().tracks;
    require(tracks.front().clips.empty(), "failed optional clip list defaults");
    require(tracks.back().id.value == "generated-2", "failed clip consumed its generated ID");
}

void generatorFailureRemainsFatal() {
    const std::string source = R"({
        "timelines":[{
            "id":"timeline","fps":30,"width":1920,"height":1080,"tracks":[{
                "id":"track","type":"video","clips":[{
                    "mediaRef":"media-1","startFrame":0,"durationFrames":30
                }]
            }]
        }]
    })";
    requireReadError(
        [&] { palmier::project::readProject(source, [] { return std::string(); }); },
        "invalidGeneratedId"
    );
}

void errorBoundaries() {
    const auto generator = [] { return std::string("generated"); };
    requireReadError(
        [&] { palmier::project::readProject(R"({"timelines":[]})", generator); },
        "emptyTimelines"
    );
    requireReadError(
        [&] { palmier::project::readProject(R"({"timelines":null})", generator); },
        "wrongRequiredType"
    );
    requireReadError(
        [&] {
            palmier::project::readProject(
                R"({"timelines":[{"width":1,"height":1,"tracks":[]}]})",
                generator
            );
        },
        "missingRequiredField"
    );
    requireReadError(
        [&] {
            palmier::project::readProject(
                R"({"timelines":[{"fps":30,"width":1,"height":1,"tracks":[{"type":"future"}]}]})",
                generator
            );
        },
        "unsupportedRequiredEnum"
    );
    requireReadError(
        [&] {
            palmier::project::readProject(
                R"({"timelines":[{"fps":"30","width":1,"height":1,"tracks":[]}]})",
                generator
            );
        },
        "wrongRequiredType"
    );
    requireReadError(
        [&] {
            palmier::project::readProject(
                R"({"timelines":[{"fps":30.5,"width":1,"height":1,"tracks":[]}]})",
                generator
            );
        },
        "wrongRequiredType"
    );
    requireReadError(
        [&] {
            palmier::project::readProject(
                R"({"timelines":[{"fps":9223372036854775808,"width":1,"height":1,"tracks":[]}]})",
                generator
            );
        },
        "integerOutOfRange"
    );
    requireReadError(
        [&] {
            palmier::project::readProject(
                R"({"timelines":[{"fps":0,"width":1,"height":1,"tracks":[]}]})",
                generator
            );
        },
        "invalidRequiredValue"
    );
}

void largeUnknownIntegerRetainsItsLexeme() {
    const std::string source = R"({"fps":30,"width":1,"height":1,"tracks":[],"x":9223372036854775808})";
    auto document = palmier::project::readProject(source, [] { return std::string("generated"); });
    require(at(document.source(), "x").number().lexeme == "9223372036854775808", "large integer lexeme");
    require(!at(document.source(), "x").number().integer, "large integer domain value");
    require(palmier::json::canonical(document.source()).find("9223372036854775808") != std::string::npos, "large integer canonical");
}

}

int wmain(int argumentCount, wchar_t* arguments[]) {
    try {
        if (argumentCount != 2) {
            throw std::runtime_error("expected repository root");
        }
        const std::filesystem::path root(arguments[1]);
        currentFixture(root);
        legacyFixture(root);
        unknownSourceIsRetained(root);
        synthesizedIdsAndDefaults();
        invalidOptionalClipListDefaultsAsSwift();
        optionalClipFailurePreservesGeneratorOrder();
        generatorFailureRemainsFatal();
        errorBoundaries();
        largeUnknownIntegerRetainsItsLexeme();
        std::cout << "PALMIER_PROJECT_READER_TESTS_OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "PALMIER_PROJECT_READER_TESTS_FAILED " << error.what() << '\n';
        return 1;
    }
}
