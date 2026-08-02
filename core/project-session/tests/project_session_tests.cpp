#include "palmier/project/project_session.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using palmier::json::Value;
using palmier::project::CommandError;
using palmier::project::ClipMove;
using palmier::project::MoveClipsCommand;
using palmier::project::ProjectSession;
using palmier::project::RemoveClipsCommand;
using palmier::project::SetClipPropertiesCommand;
using palmier::project::SplitClipsCommand;
using palmier::project::SplitPoint;

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

std::string readFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("cannot open project fixture");
    }
    return {
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()
    };
}

template<typename Operation>
void requireCommandError(Operation operation, const std::string& code) {
    try {
        operation();
    } catch (const CommandError& error) {
        require(error.code == code, "unexpected command error code: " + error.code);
        return;
    }
    throw std::runtime_error("expected command failure " + code);
}

const Value& firstTrack(const Value& timeline) {
    return at(timeline, "tracks").array().front();
}

const Value& clipAt(const Value& timeline, std::size_t index) {
    return at(firstTrack(timeline), "clips").array().at(index);
}

const Value& firstSourceTrack(const palmier::project::ProjectDocument& document) {
    return at(at(document.source(), "timelines").array().front(), "tracks").array().front();
}

const Value& sourceClipAt(
    const palmier::project::ProjectDocument& document,
    std::size_t index
) {
    return at(firstSourceTrack(document), "clips").array().at(index);
}

const Value& sourceClip(const palmier::project::ProjectDocument& document, const std::string& id) {
    for (const auto& track : at(at(document.source(), "timelines").array().front(), "tracks").array()) {
        const auto* clips = track.find("clips");
        if (!clips) continue;
        for (const auto& clip : clips->array()) {
            const auto* clipId = clip.find("id");
            if (clipId && clipId->string() == id) return clip;
        }
    }
    throw std::runtime_error("missing source clip " + id);
}

std::int64_t integer(const Value& value) {
    require(value.kind() == Value::Kind::number, "expected JSON number");
    require(value.number().integer.has_value(), "expected JSON integer");
    return *value.number().integer;
}

ProjectSession fixtureSession(const std::filesystem::path& root, int& nextId) {
    const auto source = readFile(
        root / "fixtures/contracts/projects/current-multitimeline.palmier/project.json"
    );
    const auto document = palmier::project::readProject(source, [] {
        return std::string("unexpected-reader-id");
    });
    return ProjectSession(document, [&nextId] {
        return "session-id-" + std::to_string(++nextId);
    });
}

void explicitSplitAndUndo(const std::filesystem::path& root) {
    int nextId = 0;
    auto session = fixtureSession(root, nextId);
    const auto baseline = palmier::json::canonical(session.getTimeline());
    const auto baselineSource = palmier::json::canonical(session.snapshot().document.source());
    const auto result = session.splitClips(SplitClipsCommand{
        std::vector<SplitPoint>{{"clip-main-1", 60}}, std::nullopt, std::nullopt,
    });
    require(result.changed, "split should change the project");
    require(result.revisionBefore == 0 && result.revisionAfter == 1, "split revision");
    require(
        result.publication
        && result.publication->revision == 1
        && result.publication->stateId == 1,
        "split prepares its committed publication"
    );
    require(session.dirty(), "split should make the session dirty");

    const auto timeline = session.getTimeline();
    require(at(firstTrack(timeline), "clips").array().size() == 2, "split clip count");
    const auto& left = clipAt(timeline, 0);
    const auto& right = clipAt(timeline, 1);
    require(at(left, "id").string() == "clip-main-1", "left ID must remain stable");
    require(integer(at(left, "durationFrames")) == 60, "left duration");
    require(integer(at(left, "trimEndFrame")) == 90, "left trim end");
    require(at(right, "id").string() == "session-id-2", "right ID");
    require(integer(at(right, "startFrame")) == 60, "right start");
    require(integer(at(right, "durationFrames")) == 90, "right duration");
    require(integer(at(right, "trimStartFrame")) == 60, "right trim start");
    const auto splitSnapshot = session.snapshot();
    require(splitSnapshot.revision == 1 && splitSnapshot.stateId == 1, "split snapshot identity");
    require(splitSnapshot.dirty(), "split snapshot dirty state");
    require(at(sourceClipAt(splitSnapshot.document, 0), "id").string() == "clip-main-1", "source left ID");
    require(integer(at(sourceClipAt(splitSnapshot.document, 0), "durationFrames")) == 60, "source left duration");
    require(at(sourceClipAt(splitSnapshot.document, 1), "id").string() == "session-id-2", "source right ID");
    require(integer(at(sourceClipAt(splitSnapshot.document, 1), "trimStartFrame")) == 60, "source right trim");

    const auto undo = session.undo();
    require(undo.changed && undo.revisionAfter == 2, "undo revision");
    require(
        undo.publication
        && undo.publication->revision == 2
        && undo.publication->stateId == 0,
        "undo prepares its committed publication"
    );
    require(!session.dirty(), "undo to baseline should clear dirty");
    require(palmier::json::canonical(session.getTimeline()) == baseline, "undo exact restore");
    require(
        palmier::json::canonical(session.snapshot().document.source()) == baselineSource,
        "undo exact source restore"
    );
    requireCommandError([&] { static_cast<void>(session.undo()); }, "nothingToUndo");
}

void moveAcrossTrackPrunesAndUndoesExactly() {
    const std::string source = R"({
        "timelines":[{
            "id":"timeline","name":"Project","fps":30,"width":1920,"height":1080,
            "tracks":[
                {"id":"source","type":"video","clips":[{
                    "id":"target","mediaRef":"media","mediaType":"video",
                    "sourceClipType":"video","startFrame":0,"durationFrames":20
                }]},
                {"id":"destination","type":"image","x-track":{"keep":true},"clips":[{
                    "id":"keeper","mediaRef":"still","mediaType":"image",
                    "sourceClipType":"image","startFrame":200,"durationFrames":20
                }]}
            ]
        }],
        "activeTimelineId":"timeline","openTimelineIds":["timeline"],
        "x-root":{"keep":"root"}
    })";
    const auto document = palmier::project::readProject(source, [] {
        return std::string("unexpected-reader-id");
    });
    int nextId{};
    ProjectSession session(document, [&] {
        return "move-track-id-" + std::to_string(++nextId);
    });
    const auto baseline = palmier::json::canonical(session.snapshot().document.source());
    const auto result = session.moveClips(MoveClipsCommand{{
        ClipMove{"target", std::size_t{1}, std::int64_t{100}},
    }});
    require(result.changed && result.revisionAfter == 1, "cross-track move revision");
    require(result.publication->undoDepth == 1, "cross-track move undo depth");
    const auto timeline = session.getTimeline();
    const auto& tracks = at(timeline, "tracks").array();
    require(tracks.size() == 1, "empty source track was not pruned");
    require(at(tracks.front(), "trackId").string() == "destination", "destination track identity");
    const auto& clips = at(tracks.front(), "clips").array();
    require(clips.size() == 2, "cross-track move clip count");
    require(at(clips.front(), "id").string() == "target", "moved clip order");
    require(integer(at(clips.front(), "startFrame")) == 100, "moved clip frame");
    const auto movedSource = session.snapshot();
    const auto& sourceTracks = at(
        at(movedSource.document.source(), "timelines").array().front(),
        "tracks"
    ).array();
    require(sourceTracks.size() == 1, "source track pruning was not persisted");
    require(
        palmier::json::canonical(at(sourceTracks.front(), "x-track")) == R"({"keep":true})",
        "destination track canary changed"
    );
    require(
        palmier::json::canonical(at(movedSource.document.source(), "x-root"))
            == R"({"keep":"root"})",
        "move changed root canary"
    );
    static_cast<void>(session.undo());
    require(
        palmier::json::canonical(session.snapshot().document.source()) == baseline,
        "move undo did not restore exact project source"
    );
}

void linkedMoveAndNoOpShareOneHistory() {
    const std::string source = R"({
        "timelines":[{"id":"timeline","fps":30,"width":1920,"height":1080,"tracks":[
            {"id":"video-track","type":"video","clips":[{
                "id":"video","mediaRef":"media","mediaType":"video","sourceClipType":"video",
                "startFrame":10,"durationFrames":30,"linkGroupId":"link"
            }]},
            {"id":"audio-track","type":"audio","clips":[{
                "id":"audio","mediaRef":"media","mediaType":"audio","sourceClipType":"audio",
                "startFrame":5,"durationFrames":40,"linkGroupId":"link"
            }]}
        ]}],"activeTimelineId":"timeline","openTimelineIds":["timeline"]
    })";
    const auto document = palmier::project::readProject(source, [] {
        return std::string("unexpected-reader-id");
    });
    int nextId{};
    ProjectSession session(document, [&] {
        return "linked-move-id-" + std::to_string(++nextId);
    });
    const auto baseline = palmier::json::canonical(session.snapshot().document.source());
    requireCommandError(
        [&] {
            static_cast<void>(session.moveClips(MoveClipsCommand{{
                ClipMove{"video", std::nullopt, std::int64_t{0}},
            }}));
        },
        "invalidMoveFrame"
    );
    require(
        session.revision() == 0
        && palmier::json::canonical(session.snapshot().document.source()) == baseline,
        "negative linked move changed the project"
    );
    const auto moved = session.moveClips(MoveClipsCommand{{
        ClipMove{"video", std::nullopt, std::int64_t{50}},
    }});
    require(moved.changed && moved.publication->undoDepth == 1, "linked move history");
    const auto timeline = session.getTimeline();
    const auto& tracks = at(timeline, "tracks").array();
    require(integer(at(at(tracks[0], "clips").array().front(), "startFrame")) == 50, "video move");
    require(integer(at(at(tracks[1], "clips").array().front(), "startFrame")) == 45, "audio delta");
    const auto noOp = session.moveClips(MoveClipsCommand{{
        ClipMove{"video", std::nullopt, std::int64_t{50}},
    }});
    require(!noOp.changed, "exact move should be a no-op");
    require(noOp.revisionBefore == 1 && noOp.revisionAfter == 1, "no-op revision changed");
    require(noOp.publication->undoDepth == 1, "no-op added undo history");
    static_cast<void>(session.undo());
    require(session.revision() == 2 && !session.dirty(), "linked move undo identity");
}

void overlappingMovesDoNotConsumeGeneratedIds() {
    const std::string source = R"({
        "timelines":[{"id":"timeline","fps":30,"width":1920,"height":1080,"tracks":[{
            "id":"track","type":"video","clips":[
                {"id":"one","mediaRef":"one","mediaType":"video","sourceClipType":"video",
                 "startFrame":0,"durationFrames":20},
                {"id":"two","mediaRef":"two","mediaType":"video","sourceClipType":"video",
                 "startFrame":40,"durationFrames":20}
            ]
        }]}],"activeTimelineId":"timeline","openTimelineIds":["timeline"]
    })";
    const auto document = palmier::project::readProject(source, [] {
        return std::string("unexpected-reader-id");
    });
    int nextId{};
    ProjectSession session(document, [&] {
        return "overlap-id-" + std::to_string(++nextId);
    });
    const auto baseline = palmier::json::canonical(session.snapshot().document.source());
    requireCommandError(
        [&] {
            static_cast<void>(session.moveClips(MoveClipsCommand{{
                ClipMove{"one", std::nullopt, std::int64_t{100}},
                ClipMove{"two", std::nullopt, std::int64_t{110}},
            }}));
        },
        "overlappingMoves"
    );
    require(nextId == 0, "overlapping move consumed a generated ID");
    require(
        session.revision() == 0
        && palmier::json::canonical(session.snapshot().document.source()) == baseline,
        "overlapping move changed the project"
    );
    const auto moved = session.moveClips(MoveClipsCommand{{
        ClipMove{"one", std::nullopt, std::int64_t{100}},
    }});
    require(moved.actionId == "overlap-id-1", "rejected move advanced generated identity");
}

void linkedOverwriteIsRefusedWithoutMutation() {
    const std::string source = R"({
        "timelines":[{"id":"timeline","fps":30,"width":1920,"height":1080,"tracks":[
            {"id":"video","type":"video","clips":[
                {"id":"moving","mediaRef":"one","mediaType":"video","sourceClipType":"video",
                 "startFrame":0,"durationFrames":20},
                {"id":"linked-video","mediaRef":"two","mediaType":"video","sourceClipType":"video",
                 "startFrame":40,"durationFrames":60,"linkGroupId":"link"}
            ]},
            {"id":"audio","type":"audio","clips":[
                {"id":"linked-audio","mediaRef":"two","mediaType":"audio","sourceClipType":"audio",
                 "startFrame":40,"durationFrames":60,"linkGroupId":"link"}
            ]}
        ]}],"activeTimelineId":"timeline","openTimelineIds":["timeline"]
    })";
    const auto document = palmier::project::readProject(source, [] {
        return std::string("unexpected-reader-id");
    });
    ProjectSession session(document, [] { return std::string("linked-overwrite-id"); });
    const auto baseline = palmier::json::canonical(session.snapshot().document.source());
    requireCommandError(
        [&] {
            static_cast<void>(session.moveClips(MoveClipsCommand{{
                ClipMove{"moving", std::nullopt, std::int64_t{50}},
            }}));
        },
        "unsupportedLinkedOverwrite"
    );
    require(
        session.revision() == 0
        && palmier::json::canonical(session.snapshot().document.source()) == baseline,
        "linked overwrite refusal changed the project"
    );
}

void moveCancellationDuringPlanningDoesNotCommit() {
    const std::string source = R"({
        "timelines":[{"id":"timeline","fps":30,"width":1920,"height":1080,"tracks":[{
            "id":"track","type":"video","clips":[
                {"id":"moving","mediaRef":"one","mediaType":"video","sourceClipType":"video",
                 "startFrame":0,"durationFrames":20},
                {"id":"blocker","mediaRef":"two","mediaType":"video","sourceClipType":"video",
                 "startFrame":40,"durationFrames":60,"speed":1}
            ]
        }]}],"activeTimelineId":"timeline","openTimelineIds":["timeline"]
    })";
    const auto document = palmier::project::readProject(source, [] {
        return std::string("unexpected-reader-id");
    });
    std::stop_source cancellation;
    int nextId{};
    ProjectSession session(document, [&] {
        const auto generated = "cancel-move-id-" + std::to_string(++nextId);
        if (nextId == 2) cancellation.request_stop();
        return generated;
    });
    const auto baseline = palmier::json::canonical(session.snapshot().document.source());
    requireCommandError(
        [&] {
            static_cast<void>(session.moveClips(
                MoveClipsCommand{{
                    ClipMove{"moving", std::nullopt, std::int64_t{50}},
                }},
                cancellation.get_token()
            ));
        },
        "cancelled"
    );
    require(
        session.revision() == 0
        && !session.dirty()
        && palmier::json::canonical(session.snapshot().document.source()) == baseline,
        "move cancellation during planning committed state"
    );
}

void removeLinkedGroupPrunesAndUndoesExactly() {
    const std::string source = R"({
        "timelines":[{"id":"timeline","fps":30,"width":1920,"height":1080,"tracks":[
            {"id":"video","type":"video","clips":[{
                "id":"video-clip","mediaRef":"media","mediaType":"video",
                "sourceClipType":"video","startFrame":0,"durationFrames":30,
                "linkGroupId":"link","x-remove-canary":{"keep":true}
            }]},
            {"id":"audio","type":"audio","clips":[{
                "id":"audio-clip","mediaRef":"media","mediaType":"audio",
                "sourceClipType":"audio","startFrame":0,"durationFrames":30,
                "linkGroupId":"link"
            }]},
            {"id":"keeper-track","type":"image","x-track":{"keep":true},"clips":[{
                "id":"keeper","mediaRef":"still","mediaType":"image",
                "sourceClipType":"image","startFrame":100,"durationFrames":30
            }]}
        ]}],"activeTimelineId":"timeline","openTimelineIds":["timeline"],
        "x-root":{"keep":"root"}
    })";
    const auto document = palmier::project::readProject(source, [] {
        return std::string("unexpected-reader-id");
    });
    ProjectSession session(document, [] { return std::string("remove-action-id"); });
    const auto baseline = palmier::json::canonical(session.snapshot().document.source());
    const auto removed = session.removeClips(RemoveClipsCommand{{"video-clip"}});
    require(removed.changed && removed.revisionAfter == 1, "remove revision");
    require(removed.publication->undoDepth == 1, "remove undo depth");
    const auto& removedIds = at(*removed.payload, "removedClipIds").array();
    require(removedIds.size() == 2, "linked remove receipt");
    require(at(*removed.payload, "clips").array().empty(), "remove receipt clips must be empty");
    require(
        at(*removed.payload, "notes").array().front().string().find("Track indices shifted")
            != std::string::npos,
        "remove receipt did not report pruned track indexes"
    );
    const auto timeline = session.getTimeline();
    const auto& tracks = at(timeline, "tracks").array();
    require(tracks.size() == 1, "remove did not prune empty linked tracks");
    require(at(tracks.front(), "trackId").string() == "keeper-track", "remove kept wrong track");
    const auto sourceAfter = session.snapshot();
    require(
        palmier::json::canonical(at(sourceAfter.document.source(), "x-root"))
            == R"({"keep":"root"})",
        "remove changed root canary"
    );
    static_cast<void>(session.undo());
    require(
        palmier::json::canonical(session.snapshot().document.source()) == baseline,
        "remove undo did not restore exact project source"
    );
}

void invalidRemovalsDoNotMutateOrConsumeIds() {
    const std::string source = R"({
        "timelines":[{"id":"timeline","fps":30,"width":1920,"height":1080,"tracks":[{
            "id":"track","type":"video","clips":[
                {"id":"clip","mediaRef":"media","mediaType":"video",
                 "sourceClipType":"video","startFrame":0,"durationFrames":30},
                {"id":"keeper","mediaRef":"media","mediaType":"video",
                 "sourceClipType":"video","startFrame":100,"durationFrames":30}
            ]
        }]}],"activeTimelineId":"timeline","openTimelineIds":["timeline"]
    })";
    const auto document = palmier::project::readProject(source, [] {
        return std::string("unexpected-reader-id");
    });
    int nextId{};
    ProjectSession session(document, [&] {
        return "remove-id-" + std::to_string(++nextId);
    });
    const auto baseline = palmier::json::canonical(session.snapshot().document.source());
    requireCommandError(
        [&] { static_cast<void>(session.removeClips(RemoveClipsCommand{})); },
        "invalidClipIds"
    );
    requireCommandError(
        [&] {
            static_cast<void>(session.removeClips(RemoveClipsCommand{{"clip", "missing"}}));
        },
        "clipNotFound"
    );
    require(
        nextId == 0
        && session.revision() == 0
        && palmier::json::canonical(session.snapshot().document.source()) == baseline,
        "invalid remove changed state or generated identity"
    );
    const auto removed = session.removeClips(RemoveClipsCommand{{"clip", "clip"}});
    require(removed.actionId == "remove-id-1", "duplicate remove changed action identity");
    require(at(*removed.payload, "removedClipIds").array().size() == 1, "duplicate remove receipt");
    require(at(*removed.payload, "notes").array().empty(), "remove without pruning returned a track note");
}

void removeCancellationDuringPlanningDoesNotCommit() {
    const std::string source = R"({
        "timelines":[{"id":"timeline","fps":30,"width":1920,"height":1080,"tracks":[{
            "id":"track","type":"video","clips":[{
                "id":"clip","mediaRef":"media","mediaType":"video",
                "sourceClipType":"video","startFrame":0,"durationFrames":30
            }]
        }]}],"activeTimelineId":"timeline","openTimelineIds":["timeline"]
    })";
    const auto document = palmier::project::readProject(source, [] {
        return std::string("unexpected-reader-id");
    });
    std::stop_source cancellation;
    ProjectSession session(document, [&] {
        cancellation.request_stop();
        return std::string("cancel-remove-id");
    });
    const auto baseline = palmier::json::canonical(session.snapshot().document.source());
    requireCommandError(
        [&] {
            static_cast<void>(session.removeClips(
                RemoveClipsCommand{{"clip"}},
                cancellation.get_token()
            ));
        },
        "cancelled"
    );
    require(
        session.revision() == 0
        && !session.dirty()
        && palmier::json::canonical(session.snapshot().document.source()) == baseline,
        "remove cancellation during planning committed state"
    );
}

void moveOverwriteSplitsBlockerAtomically() {
    const std::string source = R"({
        "timelines":[{"id":"timeline","fps":30,"width":1920,"height":1080,"tracks":[{
            "id":"track","type":"video","clips":[
                {"id":"moving","mediaRef":"one","mediaType":"video","sourceClipType":"video",
                 "startFrame":0,"durationFrames":20},
                {"id":"blocker","mediaRef":"two","mediaType":"video","sourceClipType":"video",
                 "startFrame":40,"durationFrames":60,"trimStartFrame":10,"trimEndFrame":20,"speed":1}
            ]
        }]}],"activeTimelineId":"timeline","openTimelineIds":["timeline"]
    })";
    const auto document = palmier::project::readProject(source, [] {
        return std::string("unexpected-reader-id");
    });
    int nextId{};
    ProjectSession session(document, [&] {
        return "overwrite-id-" + std::to_string(++nextId);
    });
    const auto baseline = palmier::json::canonical(session.snapshot().document.source());
    const auto result = session.moveClips(MoveClipsCommand{{
        ClipMove{"moving", std::nullopt, std::int64_t{50}},
    }});
    require(result.changed && result.publication->undoDepth == 1, "overwrite move history");
    const auto timeline = session.getTimeline();
    const auto& clips = at(firstTrack(timeline), "clips").array();
    require(clips.size() == 3, "overwrite did not split the blocker");
    require(at(clips[0], "id").string() == "blocker", "left blocker ID changed");
    require(integer(at(clips[0], "startFrame")) == 40, "left blocker start");
    require(integer(at(clips[0], "durationFrames")) == 10, "left blocker duration");
    require(integer(at(clips[0], "trimEndFrame")) == 70, "left blocker trim end");
    require(at(clips[1], "id").string() == "moving", "moved clip placement");
    require(integer(at(clips[1], "startFrame")) == 50, "moved clip start");
    require(at(clips[2], "id").string() == "overwrite-id-2", "right blocker ID");
    require(integer(at(clips[2], "startFrame")) == 70, "right blocker start");
    require(integer(at(clips[2], "trimStartFrame")) == 40, "right blocker trim start");
    static_cast<void>(session.undo());
    require(
        palmier::json::canonical(session.snapshot().document.source()) == baseline,
        "overwrite move undo was not exact"
    );
}

void invalidMovesDoNotMutate() {
    const std::string source = R"({
        "timelines":[{"id":"timeline","fps":30,"width":1920,"height":1080,"tracks":[
            {"id":"video","type":"video","clips":[{
                "id":"clip","mediaRef":"media","mediaType":"video","sourceClipType":"video",
                "startFrame":0,"durationFrames":20
            }]},
            {"id":"audio","type":"audio","clips":[]}
        ]}],"activeTimelineId":"timeline","openTimelineIds":["timeline"]
    })";
    const auto document = palmier::project::readProject(source, [] {
        return std::string("unexpected-reader-id");
    });
    ProjectSession session(document, [] { return std::string("unused-generated-id"); });
    const auto baseline = palmier::json::canonical(session.snapshot().document.source());
    requireCommandError(
        [&] { static_cast<void>(session.moveClips(MoveClipsCommand{})); },
        "invalidMoves"
    );
    requireCommandError(
        [&] {
            static_cast<void>(session.moveClips(MoveClipsCommand{{
                ClipMove{"clip", std::nullopt, std::nullopt},
            }}));
        },
        "invalidMoveDestination"
    );
    requireCommandError(
        [&] {
            static_cast<void>(session.moveClips(MoveClipsCommand{{
                ClipMove{"clip", std::size_t{1}, std::int64_t{10}},
            }}));
        },
        "incompatibleTrackType"
    );
    requireCommandError(
        [&] {
            static_cast<void>(session.moveClips(MoveClipsCommand{{
                ClipMove{"clip", std::nullopt, std::int64_t{-1}},
            }}));
        },
        "invalidMoveFrame"
    );
    require(session.revision() == 0 && !session.dirty(), "invalid move changed identity");
    require(
        palmier::json::canonical(session.snapshot().document.source()) == baseline,
        "invalid move changed project source"
    );
}

void publicationPreparationFailureDoesNotCommit() {
    const auto source = std::string(R"({
        "timelines":[{
            "id":"timeline","name":"Project","fps":30,"width":1920,"height":1080,
            "tracks":[{"id":"track","type":"video","clips":[{
                "id":"target","mediaRef":"media","mediaType":"video",
                "sourceClipType":"video","startFrame":0,"durationFrames":120,
                "speed":1,"opacity":1,"blendMode":"normal"
            }]}]
        }],
        "activeTimelineId":"timeline","openTimelineIds":["timeline"]
    })");
    const auto document = palmier::project::readProject(source, [] {
        return std::string("unexpected-reader-id");
    });
    bool failPublication{};
    int nextId{};
    ProjectSession session(
        document,
        [&] { return "publication-id-" + std::to_string(++nextId); },
        [&failPublication](palmier::project::ProjectSessionSnapshot snapshot) {
            if (failPublication) throw std::bad_alloc();
            return std::make_shared<const palmier::project::ProjectSessionSnapshot>(
                std::move(snapshot)
            );
        }
    );
    const auto baseline = palmier::json::canonical(session.snapshot().document.source());
    failPublication = true;
    try {
        static_cast<void>(session.removeClips(RemoveClipsCommand{{"target"}}));
        throw std::runtime_error("expected remove publication preparation failure");
    } catch (const std::bad_alloc&) {
    }
    require(
        session.revision() == 0
        && session.stateId() == 0
        && !session.dirty()
        && palmier::json::canonical(session.snapshot().document.source()) == baseline,
        "remove publication failure must preserve the exact session"
    );
    try {
        static_cast<void>(session.moveClips(MoveClipsCommand{{
            ClipMove{"target", std::nullopt, std::int64_t{20}},
        }}));
        throw std::runtime_error("expected move publication preparation failure");
    } catch (const std::bad_alloc&) {
    }
    require(
        session.revision() == 0
        && session.stateId() == 0
        && !session.dirty()
        && palmier::json::canonical(session.snapshot().document.source()) == baseline,
        "move publication failure must preserve the exact session"
    );
    try {
        static_cast<void>(session.setClipProperties(SetClipPropertiesCommand{
            {"target"}, std::nullopt, 5, std::nullopt, std::nullopt,
        }));
        throw std::runtime_error("expected property publication preparation failure");
    } catch (const std::bad_alloc&) {
    }
    require(
        session.revision() == 0
        && session.stateId() == 0
        && !session.dirty()
        && palmier::json::canonical(session.snapshot().document.source()) == baseline,
        "property publication failure must preserve the exact session"
    );
    try {
        static_cast<void>(session.splitClips(SplitClipsCommand{
            std::vector<SplitPoint>{{"target", 40}}, std::nullopt, std::nullopt,
        }));
        throw std::runtime_error("expected split publication preparation failure");
    } catch (const std::bad_alloc&) {
    }
    require(
        session.revision() == 0
        && session.stateId() == 0
        && !session.dirty()
        && palmier::json::canonical(session.snapshot().document.source()) == baseline,
        "split publication failure must preserve the exact session"
    );

    failPublication = false;
    const auto split = session.splitClips(SplitClipsCommand{
        std::vector<SplitPoint>{{"target", 40}}, std::nullopt, std::nullopt,
    });
    const auto splitSource = palmier::json::canonical(split.publication->document.source());
    failPublication = true;
    try {
        static_cast<void>(session.undo());
        throw std::runtime_error("expected undo publication preparation failure");
    } catch (const std::bad_alloc&) {
    }
    require(
        session.revision() == 1
        && session.stateId() == split.publication->stateId
        && palmier::json::canonical(session.snapshot().document.source()) == splitSource,
        "undo publication failure must retain the committed split and undo entry"
    );
    failPublication = false;
    static_cast<void>(session.undo());
    require(session.stateId() == 0, "undo remains available after publication failure");
    failPublication = true;
    try {
        static_cast<void>(session.redo());
        throw std::runtime_error("expected redo publication preparation failure");
    } catch (const std::bad_alloc&) {
    }
    require(
        session.stateId() == 0
            && session.snapshot().undoDepth == 0
            && session.snapshot().redoDepth == 1
            && palmier::json::canonical(session.snapshot().document.source()) == baseline,
        "redo publication failure must retain the undone state and redo entry"
    );
    failPublication = false;
    static_cast<void>(session.redo());
    require(
        palmier::json::canonical(session.snapshot().document.source()) == splitSource,
        "redo remains available after publication failure"
    );
    static_cast<void>(session.undo());

    const auto secondSplit = session.splitClips(SplitClipsCommand{
        std::vector<SplitPoint>{{"target", 60}}, std::nullopt, std::nullopt,
    });
    failPublication = true;
    try {
        static_cast<void>(session.markPersisted(secondSplit.publication->stateId));
        throw std::runtime_error("expected persistence publication preparation failure");
    } catch (const std::bad_alloc&) {
    }
    require(
        session.dirty() && session.persistedStateId() == 0,
        "persistence publication failure must not acknowledge the state"
    );
}

void sourceCanariesAndPersistedIdentity() {
    const std::string source = R"({
        "timelines":[{
            "id":"timeline","fps":30,"width":1920,"height":1080,
            "tracks":[{
                "id":"track","type":"video","x-track":{"keep":true},"clips":[
                    {"id":"target","mediaRef":"media","mediaType":"video",
                     "sourceClipType":"video","startFrame":0,"durationFrames":120,
                     "speed":1,"opacity":1,"blendMode":"normal"},
                    {"id":"canary","mediaRef":"other","mediaType":"video",
                     "sourceClipType":"video","startFrame":200,"durationFrames":30,
                     "x-clip":{"nested":[1,"two",null]}}
                ]
            }],
            "x-timeline":{"keep":"timeline"}
        }],
        "activeTimelineId":"timeline","openTimelineIds":["timeline"],
        "x-root":{"keep":"root"}
    })";
    const auto document = palmier::project::readProject(source, [] {
        return std::string("unexpected-reader-id");
    });
    int nextId = 0;
    ProjectSession session(document, [&] {
        return "state-id-" + std::to_string(++nextId);
    });
    const auto baseline = session.snapshot();
    const auto baselineCanary = palmier::json::canonical(sourceClipAt(baseline.document, 1));
    const auto baselineRoot = palmier::json::canonical(at(baseline.document.source(), "x-root"));

    static_cast<void>(session.splitClips(SplitClipsCommand{
        std::vector<SplitPoint>{{"target", 40}}, std::nullopt, std::nullopt,
    }));
    const auto saved = session.saveSnapshot();
    require(saved.revision == 1 && saved.stateId == 1, "save snapshot identity");
    const auto split = session.snapshot();
    require(
        palmier::json::canonical(sourceClipAt(split.document, 2)) == baselineCanary,
        "unrelated clip canary must remain exact"
    );
    require(
        palmier::json::canonical(at(split.document.source(), "x-root")) == baselineRoot,
        "root canary must remain exact"
    );
    require(
        palmier::json::canonical(at(firstSourceTrack(split.document), "x-track"))
            == R"({"keep":true})",
        "track canary must remain exact"
    );

    const auto reparsed = palmier::project::readProject(saved.source, [] {
        return std::string("unexpected-reopen-id");
    });
    require(
        reparsed.project().timelines.front().tracks.front().clips.at(1).id.origin
            == palmier::project::EntityIdOrigin::persisted,
        "saved right ID must become persisted after reopen"
    );

    session.markPersisted(saved.stateId);
    require(!session.dirty(), "saved split state must be clean");
    static_cast<void>(session.splitClips(SplitClipsCommand{
        std::vector<SplitPoint>{{"state-id-2", 80}}, std::nullopt, std::nullopt,
    }));
    require(session.stateId() == 2 && session.dirty(), "new split must create dirty state");
    session.markPersisted(saved.stateId);
    require(session.dirty(), "stale save completion must not clear newer state");
    static_cast<void>(session.undo());
    require(session.stateId() == saved.stateId && !session.dirty(), "undo to saved state must be clean");
    static_cast<void>(session.undo());
    require(session.stateId() == 0 && session.dirty(), "undo past saved state must be dirty");
    session.markPersisted(0);
    require(!session.dirty(), "persisted baseline must be clean");
    requireCommandError([&] { session.markPersisted(999); }, "unknownProjectState");
}

void unstableWriteParentsAreRefused() {
    const auto makeSession = [](std::string source) {
        auto document = palmier::project::readProject(source, [] {
            return std::string("synthesized-parent");
        });
        return ProjectSession(document, [] { return std::string("generated"); });
    };
    auto unstableTimeline = makeSession(R"({
        "timelines":[{"fps":30,"width":1920,"height":1080,"tracks":[{
            "id":"track","type":"video","clips":[{
                "id":"clip","mediaRef":"media","startFrame":0,"durationFrames":100
            }]
        }]}],"activeTimelineId":"synthesized-parent","openTimelineIds":[]
    })");
    requireCommandError(
        [&] {
            static_cast<void>(unstableTimeline.splitClips(SplitClipsCommand{
                std::vector<SplitPoint>{{"clip", 50}}, std::nullopt, std::nullopt,
            }));
        },
        "unstableTimelineId"
    );

    auto unstableTrack = makeSession(R"({
        "timelines":[{"id":"timeline","fps":30,"width":1920,"height":1080,"tracks":[{
            "type":"video","clips":[{
                "id":"clip","mediaRef":"media","startFrame":0,"durationFrames":100
            }]
        }]}],"activeTimelineId":"timeline","openTimelineIds":[]
    })");
    requireCommandError(
        [&] {
            static_cast<void>(unstableTrack.splitClips(SplitClipsCommand{
                std::vector<SplitPoint>{{"clip", 50}}, std::nullopt, std::nullopt,
            }));
        },
        "unstableTrackId"
    );

    auto legacy = makeSession(R"({
        "id":"timeline","fps":30,"width":1920,"height":1080,"tracks":[{
            "id":"track","type":"video","clips":[{
                "id":"clip","mediaRef":"media","startFrame":0,"durationFrames":100
            }]
        }]
    })");
    requireCommandError(
        [&] {
            static_cast<void>(legacy.splitClips(SplitClipsCommand{
                std::vector<SplitPoint>{{"clip", 50}}, std::nullopt, std::nullopt,
            }));
        },
        "unsupportedProjectWriteRoot"
    );
}

void invalidBatchDoesNotMutate(const std::filesystem::path& root) {
    int nextId = 0;
    auto session = fixtureSession(root, nextId);
    const auto baseline = palmier::json::canonical(session.getTimeline());
    requireCommandError(
        [&] {
            static_cast<void>(session.splitClips(SplitClipsCommand{
                std::vector<SplitPoint>{{"clip-main-1", 60}, {"clip-main-1", 150}},
                std::nullopt,
                std::nullopt,
            }));
        },
        "invalidSplitFrame"
    );
    requireCommandError(
        [&] {
            static_cast<void>(session.splitClips(SplitClipsCommand{
                std::nullopt, std::size_t{0}, std::nullopt,
            }));
        },
        "invalidSplitMode"
    );
    requireCommandError(
        [&] {
            static_cast<void>(session.splitClips(SplitClipsCommand{
                std::nullopt, std::nullopt, std::vector<std::int64_t>{30},
            }));
        },
        "invalidSplitMode"
    );
    require(session.revision() == 0 && !session.dirty(), "invalid split state");
    require(palmier::json::canonical(session.getTimeline()) == baseline, "invalid split readback");
    requireCommandError([&] { static_cast<void>(session.undo()); }, "nothingToUndo");
}

void duplicateAndMultipleCutsAreOneAction(const std::filesystem::path& root) {
    int nextId = 0;
    auto session = fixtureSession(root, nextId);
    const auto result = session.splitClips(SplitClipsCommand{
        std::vector<SplitPoint>{
            {"clip-main-1", 30},
            {"clip-main-1", 30},
            {"clip-main-1", 90},
        },
        std::nullopt,
        std::nullopt,
    });
    require(result.revisionAfter == 1, "multiple cuts revision");
    require(at(firstTrack(session.getTimeline()), "clips").array().size() == 3, "multiple cuts");
    static_cast<void>(session.undo());
    require(at(firstTrack(session.getTimeline()), "clips").array().size() == 1, "one undo");
}

void trackModeResolvesClip(const std::filesystem::path& root) {
    int nextId = 0;
    auto session = fixtureSession(root, nextId);
    static_cast<void>(session.splitClips(SplitClipsCommand{
        std::nullopt,
        std::size_t{0},
        std::vector<std::int64_t>{75},
    }));
    require(at(firstTrack(session.getTimeline()), "clips").array().size() == 2, "track mode split");
}

void generatedRightIdRemainsEditable(const std::filesystem::path& root) {
    int nextId = 0;
    auto session = fixtureSession(root, nextId);
    static_cast<void>(session.splitClips(SplitClipsCommand{
        std::vector<SplitPoint>{{"clip-main-1", 60}}, std::nullopt, std::nullopt,
    }));
    const auto firstSplit = session.getTimeline();
    const auto rightId = at(clipAt(firstSplit, 1), "id").string();
    static_cast<void>(session.splitClips(SplitClipsCommand{
        std::vector<SplitPoint>{{rightId, 100}}, std::nullopt, std::nullopt,
    }));
    require(at(firstTrack(session.getTimeline()), "clips").array().size() == 3, "split generated right ID");
    static_cast<void>(session.undo());
    require(palmier::json::canonical(session.getTimeline()) == palmier::json::canonical(firstSplit), "undo second split");
    static_cast<void>(session.undo());
    require(at(firstTrack(session.getTimeline()), "clips").array().size() == 1, "undo first split");
}

void emptySplitArraysAreInvalid(const std::filesystem::path& root) {
    int nextId = 0;
    auto session = fixtureSession(root, nextId);
    requireCommandError(
        [&] {
            static_cast<void>(session.splitClips(SplitClipsCommand{
                std::vector<SplitPoint>{}, std::nullopt, std::nullopt,
            }));
        },
        "invalidSplitMode"
    );
    requireCommandError(
        [&] {
            static_cast<void>(session.splitClips(SplitClipsCommand{
                std::nullopt, std::size_t{0}, std::vector<std::int64_t>{},
            }));
        },
        "invalidSplitMode"
    );
    require(session.revision() == 0, "empty split arrays must not mutate");
}

void linkedClipsSplitTogether() {
    const std::string source = R"({
        "timelines":[{"id":"timeline","fps":30,"width":1920,"height":1080,"tracks":[
            {"id":"video-track","type":"video","clips":[{
                "id":"video","mediaRef":"media","mediaType":"video","sourceClipType":"video",
                "startFrame":0,"durationFrames":100,"linkGroupId":"link"
            }]},
            {"id":"audio-track","type":"audio","clips":[{
                "id":"audio","mediaRef":"media","mediaType":"audio","sourceClipType":"audio",
                "startFrame":0,"durationFrames":100,"linkGroupId":"link"
            }]}
        ]}],"activeTimelineId":"timeline","openTimelineIds":["timeline"]
    })";
    const auto document = palmier::project::readProject(source, [] {
        return std::string("unexpected-reader-id");
    });
    int nextId = 0;
    ProjectSession session(document, [&] {
        return "linked-id-" + std::to_string(++nextId);
    });
    static_cast<void>(session.splitClips(SplitClipsCommand{
        std::vector<SplitPoint>{{"video", 40}}, std::nullopt, std::nullopt,
    }));
    const auto timeline = session.getTimeline();
    const auto& tracks = at(timeline, "tracks").array();
    require(at(tracks[0], "clips").array().size() == 2, "linked video split");
    require(at(tracks[1], "clips").array().size() == 2, "linked audio split");
    const auto& videoRight = at(tracks[0], "clips").array()[1];
    const auto& audioRight = at(tracks[1], "clips").array()[1];
    require(
        at(videoRight, "linkGroupId").string() == at(audioRight, "linkGroupId").string(),
        "right halves must share a new link group"
    );
    require(at(videoRight, "linkGroupId").string() != "link", "right link group must change");
}

void unsupportedSourceFieldsAreRefused() {
    const std::string source = R"({
        "id":"timeline","fps":30,"width":1920,"height":1080,"tracks":[{
            "id":"track","type":"video","clips":[{
                "id":"clip","mediaRef":"media","startFrame":0,"durationFrames":100,
                "fadeInFrames":10
            }]
        }]
    })";
    const auto document = palmier::project::readProject(source, [] {
        return std::string("unexpected-reader-id");
    });
    ProjectSession session(document, [] { return std::string("generated"); });
    requireCommandError(
        [&] {
            static_cast<void>(session.splitClips(SplitClipsCommand{
                std::vector<SplitPoint>{{"clip", 50}}, std::nullopt, std::nullopt,
            }));
        },
        "unsupportedClipSemantics"
    );
}

void cancellationDoesNotMutate(const std::filesystem::path& root) {
    int nextId = 0;
    auto session = fixtureSession(root, nextId);
    std::stop_source cancellation;
    cancellation.request_stop();
    requireCommandError(
        [&] {
            static_cast<void>(session.splitClips(
                SplitClipsCommand{
                    std::vector<SplitPoint>{{"clip-main-1", 60}}, std::nullopt, std::nullopt,
                },
                cancellation.get_token()
            ));
        },
        "cancelled"
    );
    require(session.revision() == 0, "cancelled split revision");
}

void cancellationDuringPlanningDoesNotCommit(const std::filesystem::path& root) {
    const auto source = readFile(
        root / "fixtures/contracts/projects/current-multitimeline.palmier/project.json"
    );
    const auto document = palmier::project::readProject(source, [] {
        return std::string("unexpected-reader-id");
    });
    std::stop_source cancellation;
    int nextId = 0;
    ProjectSession session(document, [&] {
        const auto value = "planning-id-" + std::to_string(++nextId);
        if (nextId == 2) {
            cancellation.request_stop();
        }
        return value;
    });
    const auto baseline = palmier::json::canonical(session.getTimeline());
    requireCommandError(
        [&] {
            static_cast<void>(session.splitClips(
                SplitClipsCommand{
                    std::vector<SplitPoint>{{"clip-main-1", 60}}, std::nullopt, std::nullopt,
                },
                cancellation.get_token()
            ));
        },
        "cancelled"
    );
    require(session.revision() == 0 && !session.dirty(), "planning cancellation state");
    require(palmier::json::canonical(session.getTimeline()) == baseline, "planning cancellation readback");
}

void extremeTimingIsRefusedBeforeCommit() {
    const std::string source = R"({
        "timelines":[{"id":"timeline","fps":30,"width":1920,"height":1080,"tracks":[{
            "id":"track","type":"video","clips":[{
                "id":"clip","mediaRef":"media","startFrame":0,
                "durationFrames":9223372036854775807,"speed":1
            }]
        }]}],"activeTimelineId":"timeline","openTimelineIds":["timeline"]
    })";
    const auto document = palmier::project::readProject(source, [] {
        return std::string("unexpected-reader-id");
    });
    ProjectSession session(document, [] { return std::string("generated"); });
    requireCommandError(
        [&] {
            static_cast<void>(session.splitClips(SplitClipsCommand{
                std::vector<SplitPoint>{{"clip", 1}}, std::nullopt, std::nullopt,
            }));
        },
        "unsafeTiming"
    );
    require(session.revision() == 0 && !session.dirty(), "extreme timing state");
}

void cancelledUndoPreservesHistory(const std::filesystem::path& root) {
    int nextId = 0;
    auto session = fixtureSession(root, nextId);
    static_cast<void>(session.splitClips(SplitClipsCommand{
        std::vector<SplitPoint>{{"clip-main-1", 60}}, std::nullopt, std::nullopt,
    }));
    const auto splitState = palmier::json::canonical(session.getTimeline());
    std::stop_source cancellation;
    cancellation.request_stop();
    requireCommandError(
        [&] { static_cast<void>(session.undo(cancellation.get_token())); },
        "cancelled"
    );
    require(session.revision() == 1, "cancelled undo revision");
    require(palmier::json::canonical(session.getTimeline()) == splitState, "cancelled undo state");
    static_cast<void>(session.undo());
    require(session.revision() == 2, "undo remains available after cancellation");
}

void redoRestoresSplitMoveAndRemoveExactly() {
    const std::string source = R"({
        "timelines":[{"id":"timeline","fps":30,"width":1920,"height":1080,"tracks":[{
            "id":"track","type":"video","x-track":{"keep":true},"clips":[{
                "id":"clip","mediaRef":"media","mediaType":"video",
                "sourceClipType":"video","startFrame":0,"durationFrames":120
            }]
        }]}],"activeTimelineId":"timeline","openTimelineIds":["timeline"],
        "x-root":{"keep":true}
    })";
    const auto document = palmier::project::readProject(source, [] {
        return std::string("unexpected-reader-id");
    });
    int nextId{};
    ProjectSession session(document, [&] { return "history-id-" + std::to_string(++nextId); });
    const auto baseline = palmier::json::canonical(session.snapshot().document.source());

    const auto split = session.splitClips(SplitClipsCommand{
        std::vector<SplitPoint>{{"clip", 60}}, std::nullopt, std::nullopt,
    });
    const auto splitState = palmier::json::canonical(session.snapshot().document.source());
    const auto timeline = session.getTimeline();
    const auto rightId = at(clipAt(timeline, 1), "id").string();
    const auto move = session.moveClips(MoveClipsCommand{{
        ClipMove{rightId, std::nullopt, std::int64_t{200}},
    }});
    const auto movedState = palmier::json::canonical(session.snapshot().document.source());
    const auto remove = session.removeClips(RemoveClipsCommand{{"clip"}});
    const auto removedState = palmier::json::canonical(session.snapshot().document.source());
    auto state = session.snapshot();
    require(state.undoDepth == 3 && state.redoDepth == 0, "forward history depths");

    require(session.undo().actionId == remove.actionId, "remove undo identity");
    require(
        palmier::json::canonical(session.snapshot().document.source()) == movedState,
        "remove undo exact state"
    );
    require(session.undo().actionId == move.actionId, "move undo identity");
    require(
        palmier::json::canonical(session.snapshot().document.source()) == splitState,
        "move undo exact state"
    );
    require(session.undo().actionId == split.actionId, "split undo identity");
    state = session.snapshot();
    require(
        palmier::json::canonical(state.document.source()) == baseline
            && state.undoDepth == 0
            && state.redoDepth == 3,
        "complete undo history"
    );

    require(session.redo().actionId == split.actionId, "split redo identity");
    require(
        palmier::json::canonical(session.snapshot().document.source()) == splitState,
        "split redo exact state"
    );
    require(at(clipAt(session.getTimeline(), 1), "id").string() == rightId, "redo changed split ID");
    require(session.redo().actionId == move.actionId, "move redo identity");
    require(
        palmier::json::canonical(session.snapshot().document.source()) == movedState,
        "move redo exact state"
    );
    require(session.redo().actionId == remove.actionId, "remove redo identity");
    state = session.snapshot();
    require(
        palmier::json::canonical(state.document.source()) == removedState
            && state.revision == 9
            && state.undoDepth == 3
            && state.redoDepth == 0,
        "complete redo history"
    );
    requireCommandError([&] { static_cast<void>(session.redo()); }, "nothingToRedo");
}

void changedEditInvalidatesRedoButFailuresAndNoOpsPreserveIt() {
    const std::string source = R"({
        "timelines":[{"id":"timeline","fps":30,"width":1920,"height":1080,"tracks":[{
            "id":"track","type":"video","clips":[{
                "id":"clip","mediaRef":"media","mediaType":"video",
                "sourceClipType":"video","startFrame":0,"durationFrames":120
            }]
        }]}],"activeTimelineId":"timeline","openTimelineIds":["timeline"]
    })";
    const auto document = palmier::project::readProject(source, [] {
        return std::string("unexpected-reader-id");
    });
    int nextId{};
    ProjectSession session(document, [&] { return "branch-id-" + std::to_string(++nextId); });
    static_cast<void>(session.splitClips(SplitClipsCommand{
        std::vector<SplitPoint>{{"clip", 60}}, std::nullopt, std::nullopt,
    }));
    static_cast<void>(session.undo());
    require(session.snapshot().redoDepth == 1, "undo did not create redo history");
    requireCommandError(
        [&] { static_cast<void>(session.removeClips(RemoveClipsCommand{{"missing"}})); },
        "clipNotFound"
    );
    const auto noOp = session.moveClips(MoveClipsCommand{{
        ClipMove{"clip", std::nullopt, std::int64_t{0}},
    }});
    require(!noOp.changed && noOp.publication->redoDepth == 1, "no-op cleared redo history");
    static_cast<void>(session.moveClips(MoveClipsCommand{{
        ClipMove{"clip", std::nullopt, std::int64_t{10}},
    }}));
    require(session.snapshot().redoDepth == 0, "changed edit retained stale redo history");
    requireCommandError([&] { static_cast<void>(session.redo()); }, "nothingToRedo");
}

void persistenceKeepsHistoryAndUsesRestoredStateIdentity() {
    const std::string source = R"({
        "timelines":[{"id":"timeline","fps":30,"width":1920,"height":1080,"tracks":[{
            "id":"track","type":"video","clips":[{
                "id":"clip","mediaRef":"media","mediaType":"video",
                "sourceClipType":"video","startFrame":0,"durationFrames":120
            }]
        }]}],"activeTimelineId":"timeline","openTimelineIds":["timeline"]
    })";
    const auto document = palmier::project::readProject(source, [] {
        return std::string("unexpected-reader-id");
    });
    int nextId{};
    ProjectSession session(document, [&] { return "persist-id-" + std::to_string(++nextId); });
    static_cast<void>(session.splitClips(SplitClipsCommand{
        std::vector<SplitPoint>{{"clip", 60}}, std::nullopt, std::nullopt,
    }));
    static_cast<void>(session.undo());
    const auto persistedBaseline = session.markPersisted(0);
    require(!persistedBaseline->dirty() && persistedBaseline->redoDepth == 1, "save cleared redo");
    static_cast<void>(session.redo());
    require(session.stateId() == 1 && session.dirty(), "redo did not restore dirty state identity");
    const auto persistedSplit = session.markPersisted(1);
    require(!persistedSplit->dirty() && persistedSplit->undoDepth == 1, "save cleared undo");
    static_cast<void>(session.undo());
    require(session.stateId() == 0 && session.dirty(), "undo did not restore dirty baseline");
    static_cast<void>(session.redo());
    require(session.stateId() == 1 && !session.dirty(), "redo did not restore persisted identity");
}

void redoCancellationAfterPublicationDoesNotCommit() {
    const std::string source = R"({
        "timelines":[{"id":"timeline","fps":30,"width":1920,"height":1080,"tracks":[{
            "id":"track","type":"video","clips":[{
                "id":"clip","mediaRef":"media","mediaType":"video",
                "sourceClipType":"video","startFrame":0,"durationFrames":120
            }]
        }]}],"activeTimelineId":"timeline","openTimelineIds":["timeline"]
    })";
    const auto document = palmier::project::readProject(source, [] {
        return std::string("unexpected-reader-id");
    });
    std::stop_source cancellation;
    bool cancelPublication{};
    int nextId{};
    ProjectSession session(
        document,
        [&] { return "cancel-redo-id-" + std::to_string(++nextId); },
        [&](palmier::project::ProjectSessionSnapshot snapshot) {
            if (cancelPublication) cancellation.request_stop();
            return std::make_shared<const palmier::project::ProjectSessionSnapshot>(
                std::move(snapshot)
            );
        }
    );
    static_cast<void>(session.splitClips(SplitClipsCommand{
        std::vector<SplitPoint>{{"clip", 60}}, std::nullopt, std::nullopt,
    }));
    static_cast<void>(session.undo());
    const auto baseline = palmier::json::canonical(session.snapshot().document.source());
    cancelPublication = true;
    requireCommandError(
        [&] { static_cast<void>(session.redo(cancellation.get_token())); },
        "cancelled"
    );
    const auto cancelled = session.snapshot();
    require(
        cancelled.revision == 2
            && cancelled.undoDepth == 0
            && cancelled.redoDepth == 1
            && palmier::json::canonical(cancelled.document.source()) == baseline,
        "redo cancellation after publication committed state"
    );
}

void timingPropertiesPropagateAndRestoreSourceSemantics() {
    const std::string source = R"({
        "timelines":[{"id":"timeline","fps":30,"width":1920,"height":1080,"tracks":[
            {"id":"video-track","type":"video","clips":[{
                "id":"video","mediaRef":"media","mediaType":"video","sourceClipType":"video",
                "startFrame":0,"durationFrames":120,"trimStartFrame":0,"trimEndFrame":0,
                "speed":1,"linkGroupId":"linked","fadeInFrames":50,"fadeOutFrames":40,
                "opacityTrack":{"keyframes":[
                    {"frame":-5,"value":0.1},{"frame":30,"value":0.5,"x-key":"keep"},
                    {"frame":90,"value":1}
                ]},
                "rotationTrack":{"keyframes":[{"frame":100,"value":10}]},
                "x-video":{"keep":true}
            }]},
            {"id":"audio-track","type":"audio","clips":[{
                "id":"audio","mediaRef":"media","mediaType":"audio","sourceClipType":"video",
                "startFrame":0,"durationFrames":120,"trimStartFrame":0,"trimEndFrame":0,
                "speed":1,"linkGroupId":"linked","x-audio":"keep"
            }]},
            {"id":"text-track","type":"text","clips":[{
                "id":"text","mediaRef":"text-media","mediaType":"text","sourceClipType":"text",
                "startFrame":0,"durationFrames":120,"trimStartFrame":3,"trimEndFrame":4,
                "speed":1,"linkGroupId":"linked","wordTimings":[
                    {"text":"one","startFrame":20,"endFrame":40,"x-word":"keep"}
                ]
            }]}
        ]}],"activeTimelineId":"timeline","openTimelineIds":["timeline"],"x-root":"keep"
    })";
    const auto document = palmier::project::readProject(source, [] {
        return std::string("unexpected-reader-id");
    });
    int nextId{};
    ProjectSession session(document, [&] { return "properties-id-" + std::to_string(++nextId); });
    const auto baseline = palmier::json::canonical(session.snapshot().document.source());

    const auto result = session.setClipProperties(SetClipPropertiesCommand{
        {"video"}, 60, 10, 20, 2.0,
    });
    require(result.changed && result.actionId == "properties-id-1", "timing action receipt");
    require(result.publication->undoDepth == 1 && result.publication->redoDepth == 0, "timing history");
    require(at(*result.payload, "clips").array().size() == 3, "linked timing receipt clips");

    const auto changed = session.snapshot();
    const auto& video = sourceClip(changed.document, "video");
    const auto& audio = sourceClip(changed.document, "audio");
    const auto& text = sourceClip(changed.document, "text");
    require(integer(at(video, "durationFrames")) == 60, "video duration");
    require(integer(at(video, "trimStartFrame")) == 10, "video trim start");
    require(integer(at(video, "trimEndFrame")) == 20, "video trim end");
    require(integer(at(video, "fadeInFrames")) == 50, "video fade in clamp");
    require(integer(at(video, "fadeOutFrames")) == 10, "video fade out clamp");
    require(at(video, "opacityTrack").find("keyframes")->array().size() == 1, "keyframes clamp");
    require(
        at(at(video, "opacityTrack").find("keyframes")->array().front(), "x-key").string() == "keep",
        "keyframe unknown field"
    );
    require(video.find("rotationTrack") == nullptr, "empty keyframe track should clear");
    require(at(video, "x-video").find("keep")->boolean(), "video unknown field");
    require(integer(at(audio, "durationFrames")) == 60, "audio linked duration");
    require(integer(at(audio, "trimStartFrame")) == 10, "audio linked trim start");
    require(integer(at(audio, "trimEndFrame")) == 20, "audio linked trim end");
    require(at(audio, "x-audio").string() == "keep", "audio unknown field");
    require(integer(at(text, "durationFrames")) == 60, "text linked duration");
    require(integer(at(text, "trimStartFrame")) == 3, "text partner trim must stay unchanged");
    require(integer(at(text, "trimEndFrame")) == 4, "text partner tail trim must stay unchanged");
    const auto& word = at(text, "wordTimings").array().front();
    require(integer(at(word, "startFrame")) == 10, "word timing start rescale");
    require(integer(at(word, "endFrame")) == 20, "word timing end rescale");
    require(at(word, "x-word").string() == "keep", "word timing unknown field");
    const auto changedSource = palmier::json::canonical(changed.document.source());

    require(session.undo().actionId == result.actionId, "timing undo action identity");
    require(
        palmier::json::canonical(session.snapshot().document.source()) == baseline,
        "timing undo exact source"
    );
    require(session.redo().actionId == result.actionId, "timing redo action identity");
    require(
        palmier::json::canonical(session.snapshot().document.source()) == changedSource,
        "timing redo exact source"
    );
}

void timingValidationNoOpAndBranchingPreserveHistory() {
    const std::string source = R"({
        "timelines":[{"id":"timeline","fps":30,"width":1920,"height":1080,"tracks":[{
            "id":"track","type":"video","clips":[{
                "id":"clip","mediaRef":"media","mediaType":"video","sourceClipType":"video",
                "startFrame":0,"durationFrames":120,"trimStartFrame":5,"trimEndFrame":0,"speed":1
            }]
        }]}],"activeTimelineId":"timeline","openTimelineIds":["timeline"]
    })";
    const auto document = palmier::project::readProject(source, [] {
        return std::string("unexpected-reader-id");
    });
    int nextId{};
    ProjectSession session(document, [&] { return "branch-properties-" + std::to_string(++nextId); });
    const auto noOp = session.setClipProperties(SetClipPropertiesCommand{
        {"clip", "clip"}, std::nullopt, 5, std::nullopt, std::nullopt,
    });
    require(!noOp.changed && nextId == 0 && noOp.publication->undoDepth == 0, "timing no-op history");

    const auto retimed = session.setClipProperties(SetClipPropertiesCommand{
        {"clip"}, std::nullopt, std::nullopt, std::nullopt, 2.0,
    });
    require(retimed.changed, "speed should change timing");
    require(integer(at(clipAt(session.getTimeline(), 0), "durationFrames")) == 60, "speed rescales duration");
    static_cast<void>(session.undo());
    require(session.snapshot().redoDepth == 1, "timing undo creates redo");

    requireCommandError(
        [&] {
            static_cast<void>(session.setClipProperties(SetClipPropertiesCommand{
                {"clip"}, 0, std::nullopt, std::nullopt, std::nullopt,
            }));
        },
        "invalidDurationFrames"
    );
    requireCommandError(
        [&] {
            static_cast<void>(session.setClipProperties(SetClipPropertiesCommand{
                {"clip"}, std::nullopt, std::nullopt, std::nullopt,
                std::numeric_limits<double>::infinity(),
            }));
        },
        "invalidSpeed"
    );
    const auto repeatedNoOp = session.setClipProperties(SetClipPropertiesCommand{
        {"clip"}, std::nullopt, 5, std::nullopt, std::nullopt,
    });
    require(!repeatedNoOp.changed && repeatedNoOp.publication->redoDepth == 1, "timing no-op cleared redo");
    static_cast<void>(session.setClipProperties(SetClipPropertiesCommand{
        {"clip"}, std::nullopt, 6, std::nullopt, std::nullopt,
    }));
    require(session.snapshot().redoDepth == 0, "changed timing retained stale redo");
}

void timingRefusesMulticamAndMalformedDependentState() {
    const std::string source = R"({
        "timelines":[{"id":"timeline","fps":30,"width":1920,"height":1080,"tracks":[{
            "id":"track","type":"video","clips":[
                {"id":"multicam","mediaRef":"media","mediaType":"video","sourceClipType":"video",
                 "startFrame":0,"durationFrames":120,"multicamGroupId":"group"},
                {"id":"malformed","mediaRef":"media","mediaType":"video","sourceClipType":"video",
                 "startFrame":200,"durationFrames":120,"opacityTrack":{"keyframes":"bad"}},
                {"id":"sequence","mediaRef":"nested","mediaType":"sequence","sourceClipType":"sequence",
                 "startFrame":400,"durationFrames":120,"speed":1}
            ]
        }]}],"activeTimelineId":"timeline","openTimelineIds":["timeline"]
    })";
    const auto document = palmier::project::readProject(source, [] {
        return std::string("unexpected-reader-id");
    });
    int nextId{};
    ProjectSession session(document, [&] { return "refusal-id-" + std::to_string(++nextId); });
    const auto baseline = palmier::json::canonical(session.snapshot().document.source());
    requireCommandError(
        [&] {
            static_cast<void>(session.setClipProperties(SetClipPropertiesCommand{
                {"multicam"}, std::nullopt, 1, std::nullopt, std::nullopt,
            }));
        },
        "multicamTimingRefused"
    );
    requireCommandError(
        [&] {
            static_cast<void>(session.setClipProperties(SetClipPropertiesCommand{
                {"malformed"}, 60, std::nullopt, std::nullopt, std::nullopt,
            }));
        },
        "unsafeDurationSemantics"
    );
    const auto skipped = session.setClipProperties(SetClipPropertiesCommand{
        {"sequence"}, std::nullopt, std::nullopt, std::nullopt, 2.0,
    });
    require(!skipped.changed, "nested timeline speed should be skipped");
    require(at(*skipped.payload, "notes").array().size() == 1, "nested speed skip note");
    require(nextId == 0, "refused and skipped timing consumed IDs");
    require(
        palmier::json::canonical(session.snapshot().document.source()) == baseline,
        "timing refusal mutated source"
    );
}

void timingCancellationAfterPublicationDoesNotCommit() {
    const std::string source = R"({
        "timelines":[{"id":"timeline","fps":30,"width":1920,"height":1080,"tracks":[{
            "id":"track","type":"video","clips":[{
                "id":"clip","mediaRef":"media","mediaType":"video","sourceClipType":"video",
                "startFrame":0,"durationFrames":120
            }]
        }]}],"activeTimelineId":"timeline","openTimelineIds":["timeline"]
    })";
    const auto document = palmier::project::readProject(source, [] {
        return std::string("unexpected-reader-id");
    });
    std::stop_source cancellation;
    bool cancelPublication{};
    ProjectSession session(
        document,
        [] { return std::string("cancel-properties-id"); },
        [&](palmier::project::ProjectSessionSnapshot snapshot) {
            if (cancelPublication) cancellation.request_stop();
            return std::make_shared<const palmier::project::ProjectSessionSnapshot>(
                std::move(snapshot)
            );
        }
    );
    const auto baseline = palmier::json::canonical(session.snapshot().document.source());
    cancelPublication = true;
    requireCommandError(
        [&] {
            static_cast<void>(session.setClipProperties(
                SetClipPropertiesCommand{
                    {"clip"}, std::nullopt, 10, std::nullopt, std::nullopt,
                },
                cancellation.get_token()
            ));
        },
        "cancelled"
    );
    const auto cancelled = session.snapshot();
    require(
        cancelled.revision == 0
            && cancelled.undoDepth == 0
            && cancelled.redoDepth == 0
            && palmier::json::canonical(cancelled.document.source()) == baseline,
        "property cancellation after publication committed state"
    );
}

}

int wmain(int argumentCount, wchar_t* arguments[]) {
    try {
        if (argumentCount != 2) {
            throw std::runtime_error("expected repository root");
        }
        const std::filesystem::path root(arguments[1]);
        explicitSplitAndUndo(root);
        moveAcrossTrackPrunesAndUndoesExactly();
        linkedMoveAndNoOpShareOneHistory();
        overlappingMovesDoNotConsumeGeneratedIds();
        linkedOverwriteIsRefusedWithoutMutation();
        moveOverwriteSplitsBlockerAtomically();
        moveCancellationDuringPlanningDoesNotCommit();
        removeLinkedGroupPrunesAndUndoesExactly();
        invalidRemovalsDoNotMutateOrConsumeIds();
        removeCancellationDuringPlanningDoesNotCommit();
        invalidMovesDoNotMutate();
        publicationPreparationFailureDoesNotCommit();
        sourceCanariesAndPersistedIdentity();
        unstableWriteParentsAreRefused();
        invalidBatchDoesNotMutate(root);
        duplicateAndMultipleCutsAreOneAction(root);
        trackModeResolvesClip(root);
        generatedRightIdRemainsEditable(root);
        emptySplitArraysAreInvalid(root);
        linkedClipsSplitTogether();
        unsupportedSourceFieldsAreRefused();
        cancellationDoesNotMutate(root);
        cancellationDuringPlanningDoesNotCommit(root);
        extremeTimingIsRefusedBeforeCommit();
        cancelledUndoPreservesHistory(root);
        redoRestoresSplitMoveAndRemoveExactly();
        changedEditInvalidatesRedoButFailuresAndNoOpsPreserveIt();
        persistenceKeepsHistoryAndUsesRestoredStateIdentity();
        redoCancellationAfterPublicationDoesNotCommit();
        timingPropertiesPropagateAndRestoreSourceSemantics();
        timingValidationNoOpAndBranchingPreserveHistory();
        timingRefusesMulticamAndMalformedDependentState();
        timingCancellationAfterPublicationDoesNotCommit();
        std::cout << "PALMIER_PROJECT_SESSION_TESTS_OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "PALMIER_PROJECT_SESSION_TESTS_FAILED " << error.what() << '\n';
        return 1;
    }
}
