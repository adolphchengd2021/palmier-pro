#include "palmier/project/project_session.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using palmier::json::Value;
using palmier::project::CommandError;
using palmier::project::ProjectSession;
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

}

int wmain(int argumentCount, wchar_t* arguments[]) {
    try {
        if (argumentCount != 2) {
            throw std::runtime_error("expected repository root");
        }
        const std::filesystem::path root(arguments[1]);
        explicitSplitAndUndo(root);
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
        std::cout << "PALMIER_PROJECT_SESSION_TESTS_OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "PALMIER_PROJECT_SESSION_TESTS_FAILED " << error.what() << '\n';
        return 1;
    }
}
