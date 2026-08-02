#include "palmier/windows/project_load_coordinator.hpp"
#include "palmier/windows/project_editing_controller.hpp"
#include "palmier/windows/project_export_controller.hpp"
#include "palmier/windows/project_persistence_controller.hpp"
#include "palmier/windows/project_projection_loader.hpp"
#include "palmier/windows/preview_presentation_controller.hpp"
#include "palmier/project/project_reader.hpp"

#include <QGuiApplication>
#include <QMetaObject>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QSemaphore>
#include <QSemaphoreReleaser>
#include <QSignalSpy>
#include <QTest>
#include <QThread>
#include <QVariantMap>
#include <QWindow>

#include <condition_variable>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <mutex>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

class FakePreviewShutdownCoordinator final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QWindow* window READ window CONSTANT)

public:
    QWindow* window() noexcept { return &window_; }
    Q_INVOKABLE bool requestShutdown() {
        requested_ = true;
        return ready_;
    }
    void completeShutdown() {
        if (ready_) return;
        ready_ = true;
        emit shutdownReady();
    }
    bool requested() const noexcept { return requested_; }

signals:
    void shutdownReady();

private:
    QWindow window_;
    bool requested_{};
    bool ready_{};
};

std::filesystem::path fixture(const char* name) {
    return std::filesystem::path(PALMIER_REPOSITORY_ROOT)
        / "fixtures" / "contracts" / "projects" / name;
}

void waitForCancellation(std::stop_token cancellation) {
    std::mutex mutex;
    std::condition_variable_any condition;
    std::stop_callback callback{
        cancellation,
        [&condition, &mutex] {
            const std::lock_guard lock(mutex);
            condition.notify_all();
        },
    };
    std::unique_lock lock(mutex);
    condition.wait(lock, [cancellation] { return cancellation.stop_requested(); });
}

palmier::windows::ProjectProjection oneTrackProjection(
    std::string timelineId,
    std::string timelineName
) {
    palmier::windows::ProjectProjection projection;
    projection.activeTimelineId = timelineId;
    projection.timelines.push_back({
        std::move(timelineId),
        std::move(timelineName),
        30,
        0,
        {{"track-1", "video", {}, {}}},
    });
    return projection;
}

palmier::windows::ProjectProjection projectionFromJson(
    std::string_view source,
    std::stop_token cancellation
) {
    auto document = palmier::project::readProject(
        source,
        [] { return std::string("generated"); },
        cancellation
    );
    return palmier::windows::projectDocumentForReadOnlyTimeline(document, cancellation);
}

std::string projectJsonWithTrackClipCounts(
    const std::vector<std::size_t>& trackClipCounts,
    std::string_view timelineId
) {
    std::string source = R"({"timelines":[{"id":")" + std::string(timelineId)
        + R"(","name":"Timeline","fps":30,"width":1920,"height":1080,"tracks":[)";
    for (std::size_t trackIndex = 0; trackIndex < trackClipCounts.size(); ++trackIndex) {
        if (trackIndex > 0) source += ',';
        source += R"({"id":"track-)" + std::to_string(trackIndex)
            + R"(","type":"video","clips":[)";
        for (std::size_t clipIndex = 0; clipIndex < trackClipCounts[trackIndex]; ++clipIndex) {
            if (clipIndex > 0) source += ',';
            source += R"({"id":"clip-)" + std::to_string(trackIndex) + '-'
                + std::to_string(clipIndex)
                + R"(","mediaRef":"media","startFrame":)" + std::to_string(clipIndex)
                + R"(,"durationFrames":1})";
        }
        source += "]}";
    }
    source += R"(]}],"activeTimelineId":")" + std::string(timelineId)
        + R"(","openTimelineIds":[")" + std::string(timelineId) + R"("]})";
    return source;
}

std::string projectJsonWithClips(std::size_t clipCount, std::string_view timelineId) {
    return projectJsonWithTrackClipCounts({clipCount}, timelineId);
}

constexpr std::string_view splittableProjectJson = R"({"timelines":[{"id":"timeline","name":"Timeline","fps":30,"width":1920,"height":1080,"tracks":[{"id":"track","type":"video","clips":[{"id":"clip","mediaRef":"media","mediaType":"video","sourceClipType":"video","startFrame":0,"durationFrames":20,"speed":1,"opacity":1,"blendMode":"normal"}]}]}],"activeTimelineId":"timeline","openTimelineIds":["timeline"]})";

class QtShellTests final : public QObject {
    Q_OBJECT

private slots:
    void editingControllerSplitsAndUndoesByStableId() {
        auto mailbox = std::make_shared<palmier::windows::ProjectRuntimeMailbox>();
        auto runtime = std::make_shared<palmier::project::ProjectRuntime>(mailbox);
        auto document = palmier::project::readProject(
            splittableProjectJson,
            [] { return std::string("generated"); }
        );
        static_cast<void>(runtime->install(
            std::move(document),
            4,
            [nextId = 0]() mutable {
                return "ui-generated-" + std::to_string(++nextId);
            }
        ));
        palmier::windows::ProjectEditingController editing(runtime, mailbox, nullptr);
        editing.activateProject(4);
        QSignalSpy finished(
            &editing,
            &palmier::windows::ProjectEditingController::operationFinished
        );
        QSignalSpy historyRestored(
            &editing,
            &palmier::windows::ProjectEditingController::historyRestored
        );

        editing.splitClip(QStringLiteral("clip"), QStringLiteral("10"));
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
        QCOMPARE(finished.at(0).at(0).toBool(), true);
        QVERIFY(editing.canUndo());
        auto snapshot = runtime->snapshot(4);
        QCOMPARE(
            snapshot.session->document.project().timelines.front().tracks.front().clips.size(),
            std::size_t{2}
        );
        QCOMPARE(snapshot.session->undoDepth, std::size_t{1});

        editing.undo();
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, 5000);
        QCOMPARE(finished.at(1).at(0).toBool(), true);
        QCOMPARE(historyRestored.count(), 1);
        QVERIFY(!editing.canUndo());
        QVERIFY(editing.canRedo());
        snapshot = runtime->snapshot(4);
        QCOMPARE(
            snapshot.session->document.project().timelines.front().tracks.front().clips.size(),
            std::size_t{1}
        );
        QCOMPARE(snapshot.session->undoDepth, std::size_t{0});

        editing.redo();
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 3, 5000);
        QCOMPARE(finished.at(2).at(0).toBool(), true);
        QCOMPARE(historyRestored.count(), 2);
        QVERIFY(editing.canUndo());
        QVERIFY(!editing.canRedo());
        snapshot = runtime->snapshot(4);
        QCOMPARE(
            snapshot.session->document.project().timelines.front().tracks.front().clips.size(),
            std::size_t{2}
        );
        QCOMPARE(snapshot.session->undoDepth, std::size_t{1});
        QCOMPARE(snapshot.session->redoDepth, std::size_t{0});

        const std::vector<QString> malformedFrames{
            QStringLiteral("-1"),
            QStringLiteral("+10"),
            QStringLiteral(" 10"),
            QString::fromUtf8("١٠"),
            QStringLiteral("9223372036854775808"),
        };
        for (const auto& frame : malformedFrames) {
            editing.splitClip(QStringLiteral("clip"), frame);
            QCOMPARE(finished.last().at(0).toBool(), false);
            QCOMPARE(editing.errorCode(), QStringLiteral("invalidArguments"));
        }
        QCOMPARE(finished.count(), 3 + static_cast<int>(malformedFrames.size()));
        editing.splitClip(
            QStringLiteral("clip"),
            QStringLiteral("9223372036854775807")
        );
        QTRY_COMPARE_WITH_TIMEOUT(
            finished.count(),
            4 + static_cast<int>(malformedFrames.size()),
            5000
        );
        QCOMPARE(editing.errorCode(), QStringLiteral("invalidSplitFrame"));
        QCOMPARE(runtime->snapshot(4).session->revision, std::uint64_t{3});
        QVERIFY(editing.requestShutdown());
    }

    void editingControllerMovesByStableIdAndPreservesNoOpHistory() {
        auto mailbox = std::make_shared<palmier::windows::ProjectRuntimeMailbox>();
        auto runtime = std::make_shared<palmier::project::ProjectRuntime>(mailbox);
        auto document = palmier::project::readProject(
            splittableProjectJson,
            [] { return std::string("generated"); }
        );
        static_cast<void>(runtime->install(
            std::move(document),
            14,
            [nextId = 0]() mutable {
                return "ui-move-generated-" + std::to_string(++nextId);
            }
        ));
        palmier::windows::ProjectEditingController editing(runtime, mailbox, nullptr);
        editing.activateProject(14);
        QSignalSpy finished(
            &editing,
            &palmier::windows::ProjectEditingController::operationFinished
        );

        editing.moveClip(QStringLiteral("clip"), {}, QStringLiteral("30"));
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
        QCOMPARE(finished.at(0).at(0).toBool(), true);
        auto snapshot = runtime->snapshot(14);
        QCOMPARE(
            snapshot.session->document.project().timelines.front()
                .tracks.front().clips.front().startFrame,
            std::int64_t{30}
        );
        QCOMPARE(snapshot.session->revision, std::uint64_t{1});
        QCOMPARE(snapshot.session->undoDepth, std::size_t{1});

        editing.moveClip(QStringLiteral("clip"), QStringLiteral("0"), QStringLiteral("30"));
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, 5000);
        QCOMPARE(finished.at(1).at(0).toBool(), true);
        snapshot = runtime->snapshot(14);
        QCOMPARE(snapshot.session->revision, std::uint64_t{1});
        QCOMPARE(snapshot.session->undoDepth, std::size_t{1});

        editing.moveClip(QStringLiteral("clip"), {}, {});
        QCOMPARE(finished.count(), 3);
        QCOMPARE(finished.last().at(0).toBool(), false);
        QCOMPARE(editing.errorCode(), QStringLiteral("invalidArguments"));
        editing.moveClip(QStringLiteral("clip"), QStringLiteral("1"), {});
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 4, 5000);
        QCOMPARE(finished.last().at(0).toBool(), false);
        QCOMPARE(editing.errorCode(), QStringLiteral("trackNotFound"));
        snapshot = runtime->snapshot(14);
        QCOMPARE(snapshot.session->revision, std::uint64_t{1});
        QCOMPARE(snapshot.session->undoDepth, std::size_t{1});

        editing.undo();
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 5, 5000);
        QCOMPARE(finished.last().at(0).toBool(), true);
        snapshot = runtime->snapshot(14);
        QCOMPARE(
            snapshot.session->document.project().timelines.front()
                .tracks.front().clips.front().startFrame,
            std::int64_t{0}
        );
        QCOMPARE(snapshot.session->undoDepth, std::size_t{0});
        QVERIFY(editing.requestShutdown());
    }

    void editingControllerSetsClipTimingAndUndoes() {
        auto mailbox = std::make_shared<palmier::windows::ProjectRuntimeMailbox>();
        auto runtime = std::make_shared<palmier::project::ProjectRuntime>(mailbox);
        auto document = palmier::project::readProject(
            splittableProjectJson,
            [] { return std::string("generated"); }
        );
        static_cast<void>(runtime->install(
            std::move(document),
            16,
            [] { return std::string("ui-properties-action"); }
        ));
        palmier::windows::ProjectEditingController editing(runtime, mailbox, nullptr);
        editing.activateProject(16);
        QSignalSpy finished(
            &editing,
            &palmier::windows::ProjectEditingController::operationFinished
        );

        editing.setClipTiming(
            QStringLiteral("clip"),
            QStringLiteral("10"),
            QStringLiteral("2"),
            QStringLiteral("3"),
            QStringLiteral("2.0")
        );
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
        QCOMPARE(finished.at(0).at(0).toBool(), true);
        auto snapshot = runtime->snapshot(16);
        const auto& clip = snapshot.session->document.project().timelines.front()
            .tracks.front().clips.front();
        QCOMPARE(clip.durationFrames, std::int64_t{10});
        QCOMPARE(clip.trimStartFrame, std::int64_t{2});
        QCOMPARE(clip.trimEndFrame, std::int64_t{3});
        QCOMPARE(clip.speed, 2.0);
        QCOMPARE(snapshot.session->undoDepth, std::size_t{1});

        editing.setClipTiming(QStringLiteral("clip"), {}, {}, {}, {});
        QCOMPARE(finished.count(), 2);
        QCOMPARE(finished.last().at(0).toBool(), false);
        QCOMPARE(editing.errorCode(), QStringLiteral("invalidArguments"));
        editing.setClipTiming(
            QStringLiteral("clip"),
            QStringLiteral("0"),
            {},
            {},
            {}
        );
        QCOMPARE(finished.count(), 3);
        QCOMPARE(finished.last().at(0).toBool(), false);
        editing.setClipTiming(
            QStringLiteral("clip"),
            {},
            QStringLiteral("-1"),
            {},
            {}
        );
        QCOMPARE(finished.count(), 4);
        QCOMPARE(finished.last().at(0).toBool(), false);
        editing.setClipTiming(
            QStringLiteral("clip"),
            {},
            {},
            {},
            QStringLiteral("0")
        );
        QCOMPARE(finished.count(), 5);
        QCOMPARE(finished.last().at(0).toBool(), false);
        QCOMPARE(runtime->snapshot(16).session->revision, std::uint64_t{1});

        editing.undo();
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 6, 5000);
        snapshot = runtime->snapshot(16);
        QCOMPARE(
            snapshot.session->document.project().timelines.front()
                .tracks.front().clips.front().durationFrames,
            std::int64_t{20}
        );
        QVERIFY(editing.requestShutdown());
    }

    void editingControllerRemovesByStableIdAndUndoes() {
        auto mailbox = std::make_shared<palmier::windows::ProjectRuntimeMailbox>();
        auto runtime = std::make_shared<palmier::project::ProjectRuntime>(mailbox);
        auto document = palmier::project::readProject(
            splittableProjectJson,
            [] { return std::string("generated"); }
        );
        static_cast<void>(runtime->install(
            std::move(document),
            15,
            [] { return std::string("ui-remove-action"); }
        ));
        palmier::windows::ProjectEditingController editing(runtime, mailbox, nullptr);
        editing.activateProject(15);
        QSignalSpy finished(
            &editing,
            &palmier::windows::ProjectEditingController::operationFinished
        );
        QSignalSpy clipRemoved(
            &editing,
            &palmier::windows::ProjectEditingController::clipRemoved
        );

        editing.removeClip(QStringLiteral("clip"));
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
        QCOMPARE(finished.at(0).at(0).toBool(), true);
        QCOMPARE(clipRemoved.count(), 1);
        QCOMPARE(clipRemoved.front().front().toString(), QStringLiteral("clip"));
        auto snapshot = runtime->snapshot(15);
        QCOMPARE(snapshot.session->document.project().timelines.front().tracks.size(), std::size_t{0});
        QCOMPARE(snapshot.session->revision, std::uint64_t{1});
        QCOMPARE(snapshot.session->undoDepth, std::size_t{1});

        editing.removeClip(QStringLiteral("missing"));
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, 5000);
        QCOMPARE(finished.last().at(0).toBool(), false);
        QCOMPARE(editing.errorCode(), QStringLiteral("clipNotFound"));
        snapshot = runtime->snapshot(15);
        QCOMPARE(snapshot.session->revision, std::uint64_t{1});
        QCOMPARE(snapshot.session->undoDepth, std::size_t{1});

        editing.undo();
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 3, 5000);
        QCOMPARE(finished.last().at(0).toBool(), true);
        snapshot = runtime->snapshot(15);
        QCOMPARE(snapshot.session->document.project().timelines.front().tracks.size(), std::size_t{1});
        QCOMPARE(
            snapshot.session->document.project().timelines.front()
                .tracks.front().clips.front().id.value,
            std::string("clip")
        );
        QCOMPARE(snapshot.session->undoDepth, std::size_t{0});
        QVERIFY(editing.requestShutdown());
    }

    void persistenceShutdownRefreshesAuthoritativeDirtyState() {
        auto mailbox = std::make_shared<palmier::windows::ProjectRuntimeMailbox>();
        auto runtime = std::make_shared<palmier::project::ProjectRuntime>(mailbox);
        auto document = palmier::project::readProject(
            splittableProjectJson,
            [] { return std::string("generated"); }
        );
        static_cast<void>(runtime->install(
            std::move(document),
            6,
            [nextId = 0]() mutable {
                return "close-generated-" + std::to_string(++nextId);
            }
        ));
        palmier::windows::ProjectPersistenceController persistence(
            runtime,
            mailbox,
            nullptr
        );
        persistence.activateProject(L"C:/close-race.palmier", 6);
        QVERIFY(!persistence.dirty());
        static_cast<void>(runtime->splitClips({
            std::vector<palmier::project::SplitPoint>{{"clip", 10}},
            std::nullopt,
            std::nullopt,
        }));
        QVERIFY(!persistence.dirty());

        QVERIFY(!persistence.requestShutdown());
        QVERIFY(persistence.dirty());
        QCOMPARE(persistence.errorCode(), QStringLiteral("unsavedChanges"));
    }

    void persistenceSaveRunsOffGuiAndShutdownWaits() {
        auto mailbox = std::make_shared<palmier::windows::ProjectRuntimeMailbox>();
        auto runtime = std::make_shared<palmier::project::ProjectRuntime>(mailbox);
        auto document = palmier::project::readProject(
            splittableProjectJson,
            [] { return std::string("generated"); }
        );
        static_cast<void>(runtime->install(
            std::move(document),
            7,
            [nextId = 0]() mutable {
                return "save-generated-" + std::to_string(++nextId);
            }
        ));
        const auto split = runtime->splitClips({
            std::vector<palmier::project::SplitPoint>{{"clip", 10}},
            std::nullopt,
            std::nullopt,
        });
        QSemaphore writerEntered;
        QSemaphore writerGate;
        QSemaphoreReleaser releaseWriterOnExit(writerGate);
        QThread* writerThread{};
        palmier::windows::ProjectPersistenceController persistence(
            runtime,
            mailbox,
            [&](
                palmier::project::ProjectRuntime& targetRuntime,
                const std::filesystem::path&,
                std::optional<std::uint64_t> generation,
                std::stop_token
            ) {
                writerThread = QThread::currentThread();
                writerEntered.release();
                writerGate.acquire();
                const auto acknowledged = targetRuntime.markPersisted(split.session->stateId);
                return palmier::project::ProjectPackageWriteReceipt{
                    generation.value_or(0),
                    acknowledged.session->revision,
                    acknowledged.session->stateId,
                    1,
                    true,
                    acknowledged.session->dirty(),
                    palmier::project::ProjectPackageWriteWarning::none,
                };
            },
            nullptr
        );
        persistence.activateProject(L"C:/save-test.palmier", 7);
        persistence.observeRuntimePublication(*mailbox->latest());
        QVERIFY(persistence.dirty());
        QSignalSpy saveFinished(
            &persistence,
            &palmier::windows::ProjectPersistenceController::saveFinished
        );
        QSignalSpy shutdownReady(
            &persistence,
            &palmier::windows::ProjectPersistenceController::shutdownReady
        );

        persistence.save();
        QTRY_COMPARE_WITH_TIMEOUT(writerEntered.available(), 1, 5000);
        QVERIFY(persistence.saving());
        QVERIFY(writerThread != QThread::currentThread());
        QVERIFY(!persistence.requestShutdown(true));
        QCOMPARE(shutdownReady.count(), 0);

        writerGate.release();
        static_cast<void>(releaseWriterOnExit.cancel());
        QTRY_COMPARE_WITH_TIMEOUT(saveFinished.count(), 1, 5000);
        QCOMPARE(saveFinished.at(0).at(0).toBool(), true);
        QVERIFY(!persistence.saving());
        QVERIFY(!persistence.dirty());
        QCOMPARE(shutdownReady.count(), 1);
    }

    void persistenceFailurePreservesDirtyState() {
        auto mailbox = std::make_shared<palmier::windows::ProjectRuntimeMailbox>();
        auto runtime = std::make_shared<palmier::project::ProjectRuntime>(mailbox);
        auto document = palmier::project::readProject(
            splittableProjectJson,
            [] { return std::string("generated"); }
        );
        static_cast<void>(runtime->install(
            std::move(document),
            3,
            [nextId = 0]() mutable {
                return "failure-generated-" + std::to_string(++nextId);
            }
        ));
        static_cast<void>(runtime->splitClips({
            std::vector<palmier::project::SplitPoint>{{"clip", 10}},
            std::nullopt,
            std::nullopt,
        }));
        palmier::windows::ProjectPersistenceController persistence(
            runtime,
            mailbox,
            [](
                palmier::project::ProjectRuntime&,
                const std::filesystem::path&,
                std::optional<std::uint64_t>,
                std::stop_token
            ) -> palmier::project::ProjectPackageWriteReceipt {
                throw palmier::project::ProjectPackageWriteError(
                    "writeFailed",
                    "write",
                    "injected save failure"
                );
            },
            nullptr
        );
        persistence.activateProject(L"C:/failure-test.palmier", 3);
        QSignalSpy saveFinished(
            &persistence,
            &palmier::windows::ProjectPersistenceController::saveFinished
        );

        persistence.save();
        QTRY_COMPARE_WITH_TIMEOUT(saveFinished.count(), 1, 5000);
        QCOMPARE(saveFinished.at(0).at(0).toBool(), false);
        QVERIFY(persistence.dirty());
        QCOMPARE(persistence.errorCode(), QStringLiteral("writeFailed"));
        QVERIFY(!persistence.requestShutdown());
        QCOMPARE(persistence.errorCode(), QStringLiteral("unsavedChanges"));
    }

    void persistenceCancellationReachesAdmittedSave() {
        auto mailbox = std::make_shared<palmier::windows::ProjectRuntimeMailbox>();
        auto runtime = std::make_shared<palmier::project::ProjectRuntime>(mailbox);
        auto document = palmier::project::readProject(
            splittableProjectJson,
            [] { return std::string("generated"); }
        );
        static_cast<void>(runtime->install(
            std::move(document),
            13,
            [nextId = 0]() mutable {
                return "cancel-generated-" + std::to_string(++nextId);
            }
        ));
        static_cast<void>(runtime->splitClips({
            std::vector<palmier::project::SplitPoint>{{"clip", 10}},
            std::nullopt,
            std::nullopt,
        }));
        QSemaphore writerEntered;
        palmier::windows::ProjectPersistenceController persistence(
            runtime,
            mailbox,
            [&writerEntered](
                palmier::project::ProjectRuntime&,
                const std::filesystem::path&,
                std::optional<std::uint64_t>,
                std::stop_token cancellation
            ) -> palmier::project::ProjectPackageWriteReceipt {
                writerEntered.release();
                waitForCancellation(cancellation);
                throw palmier::project::ProjectPackageWriteError(
                    "cancelled",
                    "testCancellation",
                    "project save was cancelled"
                );
            },
            nullptr
        );
        persistence.activateProject(L"C:/cancel-save.palmier", 13);
        persistence.observeRuntimePublication(*mailbox->latest());
        QSignalSpy saveFinished(
            &persistence,
            &palmier::windows::ProjectPersistenceController::saveFinished
        );

        persistence.save();
        QTRY_COMPARE_WITH_TIMEOUT(writerEntered.available(), 1, 5000);
        persistence.cancelSave();
        QTRY_COMPARE_WITH_TIMEOUT(saveFinished.count(), 1, 5000);
        QCOMPARE(saveFinished.at(0).at(0).toBool(), false);
        QCOMPARE(persistence.errorCode(), QStringLiteral("cancelled"));
        QVERIFY(persistence.dirty());
    }

    void persistenceCommittedWarningRemainsObservable_data() {
        QTest::addColumn<bool>("runtimeAcknowledged");
        QTest::addColumn<QString>("expectedWarning");
        QTest::newRow("newer edits remain")
            << true << QStringLiteral("saveCommittedNewerChangesRemain");
        QTest::newRow("runtime acknowledgement failed")
            << false << QStringLiteral("saveCommittedRuntimeNotAcknowledged");
    }

    void persistenceCommittedWarningRemainsObservable() {
        QFETCH(bool, runtimeAcknowledged);
        QFETCH(QString, expectedWarning);
        auto mailbox = std::make_shared<palmier::windows::ProjectRuntimeMailbox>();
        auto runtime = std::make_shared<palmier::project::ProjectRuntime>(mailbox);
        auto document = palmier::project::readProject(
            splittableProjectJson,
            [] { return std::string("generated"); }
        );
        static_cast<void>(runtime->install(
            std::move(document),
            5,
            [nextId = 0]() mutable {
                return "warning-generated-" + std::to_string(++nextId);
            }
        ));
        const auto split = runtime->splitClips({
            std::vector<palmier::project::SplitPoint>{{"clip", 10}},
            std::nullopt,
            std::nullopt,
        });
        const auto splitRevision = split.session->revision;
        const auto splitStateId = split.session->stateId;
        palmier::windows::ProjectPersistenceController persistence(
            runtime,
            mailbox,
            [runtimeAcknowledged, splitRevision, splitStateId](
                palmier::project::ProjectRuntime&,
                const std::filesystem::path&,
                std::optional<std::uint64_t> generation,
                std::stop_token
            ) {
                return palmier::project::ProjectPackageWriteReceipt{
                    generation.value_or(0),
                    splitRevision,
                    splitStateId,
                    1,
                    runtimeAcknowledged,
                    true,
                    runtimeAcknowledged
                        ? palmier::project::ProjectPackageWriteWarning::none
                        : palmier::project::ProjectPackageWriteWarning::runtimeClosedAfterSave,
                };
            },
            nullptr
        );
        persistence.activateProject(L"C:/warning-test.palmier", 5);
        QSignalSpy saveFinished(
            &persistence,
            &palmier::windows::ProjectPersistenceController::saveFinished
        );

        persistence.save();
        QTRY_COMPARE_WITH_TIMEOUT(saveFinished.count(), 1, 5000);
        QCOMPARE(saveFinished.at(0).at(0).toBool(), true);
        QVERIFY(persistence.dirty());
        QCOMPARE(persistence.warningCode(), expectedWarning);
        QVERIFY(!persistence.warningMessage().isEmpty());
    }

    void persistenceWorkerRetainsRuntimeAfterControllerTeardown() {
        auto mailbox = std::make_shared<palmier::windows::ProjectRuntimeMailbox>();
        auto runtime = std::make_shared<palmier::project::ProjectRuntime>(mailbox);
        auto document = palmier::project::readProject(
            splittableProjectJson,
            [] { return std::string("generated"); }
        );
        static_cast<void>(runtime->install(
            std::move(document),
            9,
            [nextId = 0]() mutable {
                return "teardown-generated-" + std::to_string(++nextId);
            }
        ));
        static_cast<void>(runtime->splitClips({
            std::vector<palmier::project::SplitPoint>{{"clip", 10}},
            std::nullopt,
            std::nullopt,
        }));
        QSemaphore writerEntered;
        QSemaphore writerGate;
        QSemaphoreReleaser releaseWriterOnExit(writerGate);
        auto persistence = std::make_unique<
            palmier::windows::ProjectPersistenceController
        >(
            runtime,
            mailbox,
            [&](
                palmier::project::ProjectRuntime& targetRuntime,
                const std::filesystem::path&,
                std::optional<std::uint64_t> generation,
                std::stop_token
            ) {
                writerEntered.release();
                writerGate.acquire();
                const auto snapshot = targetRuntime.snapshot(generation);
                return palmier::project::ProjectPackageWriteReceipt{
                    snapshot.projectGeneration,
                    snapshot.session->revision,
                    snapshot.session->stateId,
                    1,
                    false,
                    true,
                    palmier::project::ProjectPackageWriteWarning::runtimeAcknowledgementFailed,
                };
            },
            nullptr
        );
        persistence->activateProject(L"C:/teardown-test.palmier", 9);
        persistence->save();
        QTRY_COMPARE_WITH_TIMEOUT(writerEntered.available(), 1, 5000);
        std::weak_ptr<palmier::project::ProjectRuntime> runtimeLifetime = runtime;

        persistence.reset();
        runtime.reset();
        QVERIFY(!runtimeLifetime.expired());
        writerGate.release();
        static_cast<void>(releaseWriterOnExit.cancel());
        QTRY_VERIFY_WITH_TIMEOUT(runtimeLifetime.expired(), 5000);
    }

    void dirtyRuntimeRefusesProjectReplacement() {
        auto mailbox = std::make_shared<palmier::windows::ProjectRuntimeMailbox>();
        palmier::project::ProjectRuntime runtime(mailbox);
        palmier::windows::ProjectLoadCoordinator coordinator(
            runtime,
            mailbox,
            [nextId = 0]() mutable {
                return "replace-generated-" + std::to_string(++nextId);
            },
            nullptr
        );
        coordinator.openFolder(QUrl::fromLocalFile(QString::fromStdWString(
            fixture("current-multitimeline.palmier").wstring()
        )));
        QTRY_COMPARE_WITH_TIMEOUT(coordinator.state(), QStringLiteral("loaded"), 5000);
        static_cast<void>(runtime.splitClips({
            std::vector<palmier::project::SplitPoint>{{"clip-main-1", 75}},
            std::nullopt,
            std::nullopt,
        }));

        coordinator.openFolder(QUrl::fromLocalFile(QStringLiteral("C:/replacement.palmier")));
        QCOMPARE(coordinator.errorCode(), QStringLiteral("unsavedChanges"));
        QCOMPARE(coordinator.committedGeneration(), std::uint64_t{1});
        const auto publication = mailbox->latest();
        QVERIFY(publication.has_value());
        QCOMPARE(publication->projectGeneration, std::uint64_t{1});
        QVERIFY(publication->session->dirty());
        static_cast<void>(runtime.markPersisted(publication->session->stateId));
        coordinator.observeRuntimePublication(*mailbox->latest());
        QCOMPARE(coordinator.state(), QStringLiteral("loaded"));
        QVERIFY(coordinator.errorCode().isEmpty());
        QVERIFY(coordinator.requestShutdown());
    }

    void runtimeMailboxPublishesUndoAndPersistenceIdentity() {
        auto mailbox = std::make_shared<palmier::windows::ProjectRuntimeMailbox>();
        palmier::project::ProjectRuntime runtime(mailbox);
        auto document = palmier::project::readProject(
            splittableProjectJson,
            [] { return std::string("generated"); }
        );
        const auto installed = runtime.install(
            std::move(document),
            1,
            [nextId = 0]() mutable {
                return "split-generated-" + std::to_string(++nextId);
            }
        );
        auto publication = mailbox->latest();
        QVERIFY(publication.has_value());
        QCOMPARE(publication->token, std::uint64_t{1});
        QCOMPARE(publication->session.get(), installed.session.get());

        const auto split = runtime.splitClips({
            std::vector<palmier::project::SplitPoint>{{"clip", 10}},
            std::nullopt,
            std::nullopt,
        });
        publication = mailbox->latest();
        QVERIFY(publication.has_value());
        QCOMPARE(publication->token, std::uint64_t{2});
        QCOMPARE(publication->session->revision, std::uint64_t{1});
        QCOMPARE(publication->session->stateId, std::uint64_t{1});
        QCOMPARE(publication->session->persistedStateId, std::uint64_t{0});
        QCOMPARE(publication->session->undoDepth, std::size_t{1});

        static_cast<void>(runtime.markPersisted(split.session->stateId));
        publication = mailbox->latest();
        QVERIFY(publication.has_value());
        QCOMPARE(publication->token, std::uint64_t{3});
        QCOMPARE(publication->session->revision, std::uint64_t{1});
        QCOMPARE(publication->session->stateId, std::uint64_t{1});
        QCOMPARE(publication->session->persistedStateId, std::uint64_t{1});
        QCOMPARE(publication->session->undoDepth, std::size_t{1});

        static_cast<void>(runtime.undo());
        publication = mailbox->latest();
        QVERIFY(publication.has_value());
        QCOMPARE(publication->token, std::uint64_t{4});
        QCOMPARE(publication->session->revision, std::uint64_t{2});
        QCOMPARE(publication->session->stateId, std::uint64_t{0});
        QCOMPARE(publication->session->persistedStateId, std::uint64_t{1});
        QCOMPARE(publication->session->undoDepth, std::size_t{0});
    }

    void runtimeMutationRefreshesQtProjectionAndInvalidatesPreview() {
        auto mailbox = std::make_shared<palmier::windows::ProjectRuntimeMailbox>();
        palmier::project::ProjectRuntime runtime(mailbox);
        auto projectionGate = std::make_shared<QSemaphore>();
        palmier::windows::ProjectRuntimeProjectionBridge bridge(
            mailbox,
            [projectionGate](const palmier::windows::ProjectRuntimePublication& publication) {
                if (publication.session && publication.session->revision == 1) {
                    projectionGate->acquire();
                }
            },
            nullptr
        );
        palmier::windows::ProjectLoadCoordinator coordinator(
            runtime,
            mailbox,
            [nextId = 0]() mutable {
                return "qt-runtime-generated-" + std::to_string(++nextId);
            },
            nullptr
        );
        QSemaphoreReleaser releaseProjectionOnExit(*projectionGate);
        connect(
            &bridge,
            &palmier::windows::ProjectRuntimeProjectionBridge::publicationObserved,
            &coordinator,
            [&] {
                const auto publication = bridge.takeObservedPublication();
                if (publication) coordinator.observeRuntimePublication(*publication);
            }
        );
        connect(
            &bridge,
            &palmier::windows::ProjectRuntimeProjectionBridge::projectionReady,
            &coordinator,
            [&] {
                auto update = bridge.takeReadyUpdateIfCurrent();
                if (update) coordinator.applyRuntimeProjection(std::move(*update));
            }
        );
        coordinator.openFolder(QUrl::fromLocalFile(QString::fromStdWString(
            fixture("current-multitimeline.palmier").wstring()
        )));
        QTRY_COMPARE_WITH_TIMEOUT(coordinator.state(), QStringLiteral("loaded"), 5000);
        QCOMPARE(coordinator.committedGeneration(), std::uint64_t{1});
        QCOMPARE(coordinator.committedRevision(), std::uint64_t{0});
        QVERIFY(coordinator.presentationReady());

        std::exception_ptr mutationFailure;
        std::jthread mutation([&] {
            try {
                static_cast<void>(runtime.splitClips({
                    std::vector<palmier::project::SplitPoint>{{"clip-main-1", 75}},
                    std::nullopt,
                    std::nullopt,
                }));
            } catch (...) {
                mutationFailure = std::current_exception();
            }
        });
        QTRY_VERIFY_WITH_TIMEOUT(!coordinator.presentationReady(), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(
            coordinator.committedPreview().availability,
            palmier::windows::PreviewCandidateAvailability::invalidated,
            5000
        );
        QCOMPARE(coordinator.committedRevision(), std::uint64_t{0});
        QCOMPARE(
            coordinator.model()->data(
                coordinator.model()->index(0, 0),
                palmier::windows::ReadOnlyTimelineModel::ClipItemsRole
            ).toList().size(),
            1
        );
        projectionGate->release();
        static_cast<void>(releaseProjectionOnExit.cancel());
        QTRY_COMPARE_WITH_TIMEOUT(coordinator.committedRevision(), std::uint64_t{1}, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(coordinator.presentationReady(), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(coordinator.model()->rowCount(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(
            coordinator.model()->data(
                coordinator.model()->index(0, 0),
                palmier::windows::ReadOnlyTimelineModel::ClipItemsRole
            ).toList().size(),
            2,
            5000
        );
        mutation.join();
        if (mutationFailure) std::rethrow_exception(mutationFailure);
        QVERIFY(coordinator.requestShutdown());
        QVERIFY(bridge.requestShutdown());
    }

    void cancellationAfterRuntimeInstallCannotRollbackCommit() {
        palmier::windows::ProjectLoadCoordinator* coordinatorPointer = nullptr;
        std::atomic<bool> checkpointFired{};
        auto mailbox = std::make_shared<palmier::windows::ProjectRuntimeMailbox>([&] {
            if (checkpointFired.exchange(true)) return;
            QMetaObject::invokeMethod(
                coordinatorPointer,
                [coordinatorPointer] { coordinatorPointer->cancelLoading(); },
                Qt::BlockingQueuedConnection
            );
        });
        palmier::project::ProjectRuntime runtime(mailbox);
        palmier::windows::ProjectLoadCoordinator coordinator(
            runtime,
            mailbox,
            [nextId = 0]() mutable {
                return "qt-runtime-generated-" + std::to_string(++nextId);
            },
            nullptr
        );
        coordinatorPointer = &coordinator;
        coordinator.openFolder(QUrl::fromLocalFile(QString::fromStdWString(
            fixture("current-multitimeline.palmier").wstring()
        )));
        QTRY_VERIFY_WITH_TIMEOUT(checkpointFired.load(), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(coordinator.state(), QStringLiteral("loaded"), 5000);
        QCOMPARE(coordinator.committedGeneration(), std::uint64_t{1});
        const auto publication = mailbox->latest();
        QVERIFY(publication.has_value());
        QCOMPARE(publication->projectGeneration, std::uint64_t{1});
        QCOMPARE(
            coordinator.model()->project().activeTimelineId,
            std::string("timeline-main")
        );
        QVERIFY(coordinator.requestShutdown());
    }

    void persistencePublicationRetagsInFlightProjection() {
        QSemaphore projectionEntered;
        QSemaphore projectionGate;
        QSemaphoreReleaser releaseGateOnExit(projectionGate);
        auto mailbox = std::make_shared<palmier::windows::ProjectRuntimeMailbox>();
        palmier::project::ProjectRuntime runtime(mailbox);
        palmier::windows::ProjectRuntimeProjectionBridge bridge(
            mailbox,
            [&](const palmier::windows::ProjectRuntimePublication& publication) {
                if (
                    publication.session
                    && publication.session->revision == 1
                    && publication.session->persistedStateId == 0
                ) {
                    projectionEntered.release();
                    projectionGate.acquire();
                }
            },
            nullptr
        );
        QSignalSpy projectionReady(
            &bridge,
            &palmier::windows::ProjectRuntimeProjectionBridge::projectionReady
        );
        auto document = palmier::project::readProject(
            splittableProjectJson,
            [] { return std::string("generated"); }
        );
        static_cast<void>(runtime.install(
            std::move(document),
            1,
            [nextId = 0]() mutable {
                return "split-generated-" + std::to_string(++nextId);
            }
        ));
        QTRY_COMPARE_WITH_TIMEOUT(projectionReady.count(), 1, 5000);
        QVERIFY(bridge.takeReadyUpdateIfCurrent().has_value());
        projectionReady.clear();

        std::exception_ptr mutationFailure;
        std::uint64_t splitStateId{};
        std::jthread mutation([&] {
            try {
                const auto split = runtime.splitClips({
                    std::vector<palmier::project::SplitPoint>{{"clip", 10}},
                    std::nullopt,
                    std::nullopt,
                });
                splitStateId = split.session->stateId;
            } catch (...) {
                mutationFailure = std::current_exception();
            }
        });
        mutation.join();
        if (mutationFailure) std::rethrow_exception(mutationFailure);
        QTRY_COMPARE_WITH_TIMEOUT(projectionEntered.available(), 1, 5000);

        std::exception_ptr persistenceFailure;
        std::jthread persistence([&] {
            try {
                static_cast<void>(runtime.markPersisted(splitStateId));
            } catch (...) {
                persistenceFailure = std::current_exception();
            }
        });
        persistence.join();
        if (persistenceFailure) std::rethrow_exception(persistenceFailure);
        const auto latest = mailbox->latest();
        QVERIFY(latest.has_value());
        QCOMPARE(latest->token, std::uint64_t{3});
        QCOMPARE(latest->session->persistedStateId, splitStateId);

        projectionGate.release();
        static_cast<void>(releaseGateOnExit.cancel());
        QTRY_COMPARE_WITH_TIMEOUT(projectionReady.count(), 1, 5000);
        const auto update = bridge.takeReadyUpdateIfCurrent();
        QVERIFY(update.has_value());
        QCOMPARE(update->publication.token, latest->token);
        QCOMPARE(update->publication.session->persistedStateId, splitStateId);
        QVERIFY(update->project.has_value());
        QCOMPARE(update->project->timelines.front().tracks.front().clips.size(), std::size_t{2});
        QVERIFY(bridge.requestShutdown());
    }

    void supersededInstalledProjectWaitsForLatestLoadOutcome_data() {
        QTest::addColumn<bool>("projectionFails");
        QTest::newRow("projection succeeds") << false;
        QTest::newRow("projection fails") << true;
    }

    void supersededInstalledProjectWaitsForLatestLoadOutcome() {
        QFETCH(bool, projectionFails);
        palmier::windows::ProjectLoadCoordinator* coordinatorPointer = nullptr;
        std::atomic<bool> openedReplacement{};
        QSemaphore replacementEntered;
        QSemaphore replacementGate;
        QSemaphoreReleaser releaseReplacementOnExit(replacementGate);
        auto mailbox = std::make_shared<palmier::windows::ProjectRuntimeMailbox>([&] {
            if (openedReplacement.exchange(true)) return;
            QMetaObject::invokeMethod(
                coordinatorPointer,
                [coordinatorPointer] {
                    coordinatorPointer->openFolder(
                        QUrl::fromLocalFile(QStringLiteral("C:/replacement.palmier"))
                    );
                },
                Qt::BlockingQueuedConnection
            );
        });
        palmier::project::ProjectRuntime runtime(mailbox);
        palmier::windows::ProjectRuntimeProjectionBridge bridge(
            mailbox,
            [projectionFails](const palmier::windows::ProjectRuntimePublication&) {
                if (projectionFails) {
                    throw std::runtime_error("deferred projection failed");
                }
            },
            nullptr
        );
        palmier::windows::ProjectLoadCoordinator coordinator(
            runtime,
            mailbox,
            [nextId = 0]() mutable {
                return "qt-runtime-generated-" + std::to_string(++nextId);
            },
            [&](const std::filesystem::path& path, std::stop_token cancellation)
                -> palmier::windows::ProjectLoadCandidate {
                if (path.filename() == L"replacement.palmier") {
                    replacementEntered.release();
                    replacementGate.acquire();
                    throw std::runtime_error("replacement failed");
                }
                if (path.filename() == L"cancel.palmier") {
                    waitForCancellation(cancellation);
                    throw palmier::windows::ProjectProjectionError(
                        "cancelled",
                        "cancelled"
                    );
                }
                return palmier::windows::loadProjectCandidate(
                    fixture("current-multitimeline.palmier"),
                    cancellation
                );
            },
            nullptr
        );
        coordinatorPointer = &coordinator;
        connect(
            &bridge,
            &palmier::windows::ProjectRuntimeProjectionBridge::publicationObserved,
            &coordinator,
            [&] {
                const auto publication = bridge.takeObservedPublication();
                if (publication) coordinator.observeRuntimePublication(*publication);
            }
        );
        connect(
            &bridge,
            &palmier::windows::ProjectRuntimeProjectionBridge::projectionReady,
            &coordinator,
            [&] {
                auto update = bridge.takeReadyUpdateIfCurrent();
                if (update) coordinator.applyRuntimeProjection(std::move(*update));
            }
        );
        QSignalSpy projectCommitted(
            &coordinator,
            &palmier::windows::ProjectLoadCoordinator::projectCommitted
        );
        QSignalSpy projectionReady(
            &bridge,
            &palmier::windows::ProjectRuntimeProjectionBridge::projectionReady
        );
        coordinator.openFolder(QUrl::fromLocalFile(QStringLiteral("C:/first.palmier")));
        QTRY_COMPARE_WITH_TIMEOUT(replacementEntered.available(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(projectionReady.count(), 1, 5000);
        QCOMPARE(projectCommitted.count(), 0);
        QCOMPARE(coordinator.committedGeneration(), std::uint64_t{0});

        replacementGate.release();
        static_cast<void>(releaseReplacementOnExit.cancel());
        QTRY_COMPARE_WITH_TIMEOUT(coordinator.state(), QStringLiteral("failed"), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(projectCommitted.count(), 1, 5000);
        QCOMPARE(coordinator.committedGeneration(), std::uint64_t{1});
        if (projectionFails) {
            QCOMPARE(coordinator.model()->rowCount(), 0);
        } else {
            QCOMPARE(
                coordinator.model()->project().activeTimelineId,
                std::string("timeline-main")
            );
        }
        QCOMPARE(coordinator.errorMessage(), QStringLiteral("replacement failed"));
        if (projectionFails) {
            coordinator.openFolder(
                QUrl::fromLocalFile(QStringLiteral("C:/cancel.palmier"))
            );
            QTRY_VERIFY_WITH_TIMEOUT(coordinator.loading(), 5000);
            coordinator.cancelLoading();
            QTRY_COMPARE_WITH_TIMEOUT(coordinator.state(), QStringLiteral("failed"), 5000);
            QCOMPARE(
                coordinator.errorMessage(),
                QStringLiteral("deferred projection failed")
            );
        }
        QVERIFY(coordinator.requestShutdown());
        QVERIFY(bridge.requestShutdown());
    }

    void readerMapsCurrentProject() {
        palmier::windows::ProjectLoadCoordinator coordinator;
        QSignalSpy projectCommitted(
            &coordinator,
            &palmier::windows::ProjectLoadCoordinator::projectCommitted
        );
        coordinator.openFolder(QUrl::fromLocalFile(QString::fromStdWString(
            fixture("current-multitimeline.palmier").wstring()
        )));
        QTRY_COMPARE_WITH_TIMEOUT(coordinator.state(), QStringLiteral("loaded"), 5000);
        const auto& project = coordinator.model()->project();
        QCOMPARE(project.timelines.size(), std::size_t{1});
        QCOMPARE(project.activeTimelineId, std::string("timeline-main"));
        QCOMPARE(project.timelines.front().tracks.front().clips.front().id, std::string("clip-main-1"));
        QCOMPARE(
            project.preview.availability,
            palmier::windows::PreviewCandidateAvailability::offline
        );
        QCOMPARE(project.preview.reasonCode, std::string("mediaFileUnavailable"));
        QVERIFY(!project.preview.candidate.has_value());
        QCOMPARE(projectCommitted.count(), 1);
        QCOMPARE(coordinator.committedGeneration(), std::uint64_t{1});
        QCOMPARE(
            coordinator.committedPreview().reasonCode,
            std::string("mediaFileUnavailable")
        );
    }

    void saveAsRebasesOnlyProjectPreviewSources() {
        const std::filesystem::path source(L"C:/source.palmier");
        const std::filesystem::path destination(L"C:/destination.palmier");
        auto projectProjection = oneTrackProjection("project", "Project");
        projectProjection.preview = {
            palmier::windows::PreviewCandidateAvailability::available,
            {},
            palmier::windows::PreviewMediaCandidateProjection{
                source / L"Media" / L"clip.mp4",
                {},
                true,
                palmier::project::MediaSourceKind::project,
            },
        };
        palmier::windows::ProjectLoadCoordinator projectCoordinator(
            [projectProjection](const std::filesystem::path&, std::stop_token) {
                return projectProjection;
            },
            nullptr
        );
        projectCoordinator.openFolder(QUrl::fromLocalFile(QString::fromStdWString(source.wstring())));
        QTRY_COMPARE_WITH_TIMEOUT(projectCoordinator.state(), QStringLiteral("loaded"), 5000);
        projectCoordinator.adoptPackagePath(destination, 1);
        QVERIFY(projectCoordinator.committedPreview().candidate.has_value());
        QVERIFY(
            projectCoordinator.committedPreview().candidate->inputPath
                == destination / L"Media" / L"clip.mp4"
        );

        auto externalProjection = oneTrackProjection("external", "External");
        externalProjection.preview = {
            palmier::windows::PreviewCandidateAvailability::available,
            {},
            palmier::windows::PreviewMediaCandidateProjection{
                source / L"external.mp4",
                {},
                true,
                palmier::project::MediaSourceKind::external,
            },
        };
        palmier::windows::ProjectLoadCoordinator externalCoordinator(
            [externalProjection](const std::filesystem::path&, std::stop_token) {
                return externalProjection;
            },
            nullptr
        );
        externalCoordinator.openFolder(QUrl::fromLocalFile(QString::fromStdWString(source.wstring())));
        QTRY_COMPARE_WITH_TIMEOUT(externalCoordinator.state(), QStringLiteral("loaded"), 5000);
        externalCoordinator.adoptPackagePath(destination, 1);
        QVERIFY(externalCoordinator.committedPreview().candidate.has_value());
        QVERIFY(
            externalCoordinator.committedPreview().candidate->inputPath
                == source / L"external.mp4"
        );
    }

    void stablePreviewCandidateUsesPersistedIds() {
        palmier::windows::ProjectPreviewProjection preview;
        std::exception_ptr failure;
        std::jthread worker([&] {
            try {
                constexpr auto source = R"({"timelines":[{"id":"timeline","name":"Timeline","fps":30,"width":1920,"height":1080,"tracks":[{"id":"track-a","type":"video","clips":[{"id":"clip-late","mediaRef":"media-late","mediaType":"video","sourceClipType":"video","startFrame":40,"durationFrames":30},{"id":"clip-first","mediaRef":"media-first","mediaType":"video","sourceClipType":"video","startFrame":5,"durationFrames":30,"opacity":0.75,"transform":{"centerX":0.25,"centerY":0.75,"width":0.5,"height":0.25,"rotation":15,"flipHorizontal":false,"flipVertical":false},"effects":[{"type":"color.exposure","params":{"ev":{"value":1}}}]}]}]}],"activeTimelineId":"timeline","openTimelineIds":["timeline"]})";
                const auto document = palmier::project::readProject(
                    source,
                    [] { return std::string("generated"); }
                );
                palmier::project::MediaManifest manifest{{
                    {
                        "media-unsafe",
                        "video",
                        {palmier::project::MediaSourceKind::project, "project.json"},
                        true,
                    },
                    {
                        "media-late",
                        "video",
                        {palmier::project::MediaSourceKind::project, "project.json"},
                        true,
                    },
                    {
                        "media-first",
                        "video",
                        {palmier::project::MediaSourceKind::project, "project.json"},
                        true,
                    },
                }};
                preview = palmier::windows::projectPreviewForActiveTimeline(
                    document,
                    manifest,
                    fixture("current-multitimeline.palmier"),
                    {}
                );
            } catch (...) {
                failure = std::current_exception();
            }
        });
        worker.join();
        if (failure) std::rethrow_exception(failure);
        QCOMPARE(
            preview.availability,
            palmier::windows::PreviewCandidateAvailability::available
        );
        QVERIFY(preview.candidate.has_value());
        const auto& layer = preview.candidate->renderLayer;
        QCOMPARE(layer.timelineId, std::string("timeline"));
        QCOMPARE(layer.trackId, std::string("track-a"));
        QCOMPARE(layer.clipId, std::string("clip-first"));
        QCOMPARE(layer.mediaId, std::string("media-first"));
        QCOMPARE(layer.timelineStartFrame, std::int64_t{5});
        QCOMPARE(layer.transform.centerX, 0.25F);
        QCOMPARE(layer.transform.centerY, 0.75F);
        QCOMPARE(layer.transform.width, 0.5F);
        QCOMPARE(layer.transform.height, 0.25F);
        QCOMPARE(layer.transform.rotationDegrees, 15.0F);
        QCOMPARE(layer.opacity, 0.75F);
        QVERIFY(layer.exposureEv == std::optional<float>{1.0F});
    }

    void unsupportedVisualPropertiesAreNotSilentlyDropped() {
        palmier::windows::ProjectPreviewProjection preview;
        std::exception_ptr failure;
        std::jthread worker([&] {
            try {
                constexpr auto source = R"({"timelines":[{"id":"timeline","fps":30,"width":1920,"height":1080,"tracks":[{"id":"track","type":"video","clips":[{"id":"clip","mediaRef":"media","mediaType":"video","sourceClipType":"video","startFrame":0,"durationFrames":30,"crop":{"left":0.1,"top":0,"right":0,"bottom":0}}]}]}],"activeTimelineId":"timeline"})";
                const auto document = palmier::project::readProject(
                    source,
                    [] { return std::string("generated"); }
                );
                const palmier::project::MediaManifest manifest{{{
                    "media",
                    "video",
                    {palmier::project::MediaSourceKind::project, "project.json"},
                    true,
                }}};
                preview = palmier::windows::projectPreviewForActiveTimeline(
                    document,
                    manifest,
                    fixture("current-multitimeline.palmier"),
                    {}
                );
            } catch (...) {
                failure = std::current_exception();
            }
        });
        worker.join();
        if (failure) std::rethrow_exception(failure);
        QCOMPARE(
            preview.availability,
            palmier::windows::PreviewCandidateAvailability::unsupported
        );
        QCOMPARE(preview.reasonCode, std::string("unsupportedMasking"));
        QVERIFY(!preview.candidate.has_value());
    }

    void malformedEarlierVisualCannotBeSkippedForLaterCandidate() {
        palmier::windows::ProjectPreviewProjection preview;
        std::exception_ptr failure;
        std::jthread worker([&] {
            try {
                constexpr auto source = R"({"timelines":[{"id":"timeline","fps":30,"width":1920,"height":1080,"tracks":[{"id":"track","type":"video","clips":[{"id":"malformed","mediaRef":"media-one","mediaType":"video","sourceClipType":"video","startFrame":0,"durationFrames":30,"transform":{"width":"bad"}},{"id":"later","mediaRef":"media-two","mediaType":"video","sourceClipType":"video","startFrame":30,"durationFrames":30}]}]}],"activeTimelineId":"timeline"})";
                const auto document = palmier::project::readProject(
                    source,
                    [] { return std::string("generated"); }
                );
                const palmier::project::MediaManifest manifest{{{
                    "media-two",
                    "video",
                    {palmier::project::MediaSourceKind::project, "project.json"},
                    true,
                }}};
                preview = palmier::windows::projectPreviewForActiveTimeline(
                    document,
                    manifest,
                    fixture("current-multitimeline.palmier"),
                    {}
                );
            } catch (...) {
                failure = std::current_exception();
            }
        });
        worker.join();
        if (failure) std::rethrow_exception(failure);
        QCOMPARE(
            preview.availability,
            palmier::windows::PreviewCandidateAvailability::unsupported
        );
        QCOMPARE(preview.reasonCode, std::string("malformedVisualProperty"));
        QVERIFY(!preview.candidate.has_value());
    }

    void trimmedTimingRemainsTheFirstPreviewCandidate() {
        palmier::windows::ProjectPreviewProjection preview;
        std::exception_ptr failure;
        std::jthread worker([&] {
            try {
                constexpr auto source = R"({"timelines":[{"id":"timeline","fps":30,"width":1920,"height":1080,"tracks":[{"id":"track","type":"video","clips":[{"id":"trimmed","mediaRef":"media-one","mediaType":"video","sourceClipType":"video","startFrame":0,"durationFrames":30,"trimStartFrame":1},{"id":"later","mediaRef":"media-two","mediaType":"video","sourceClipType":"video","startFrame":30,"durationFrames":30}]}]}],"activeTimelineId":"timeline"})";
                const auto document = palmier::project::readProject(
                    source,
                    [] { return std::string("generated"); }
                );
                const palmier::project::MediaManifest manifest{{
                    {
                        "media-one",
                        "video",
                        {palmier::project::MediaSourceKind::project, "project.json"},
                        true,
                    },
                    {
                        "media-two",
                        "video",
                        {palmier::project::MediaSourceKind::project, "project.json"},
                        true,
                    },
                }};
                preview = palmier::windows::projectPreviewForActiveTimeline(
                    document,
                    manifest,
                    fixture("current-multitimeline.palmier"),
                    {}
                );
            } catch (...) {
                failure = std::current_exception();
            }
        });
        worker.join();
        if (failure) std::rethrow_exception(failure);
        QCOMPARE(
            preview.availability,
            palmier::windows::PreviewCandidateAvailability::available
        );
        QVERIFY(preview.candidate.has_value());
        QCOMPARE(preview.candidate->renderLayer.clipId, std::string("trimmed"));
        QCOMPARE(preview.candidate->renderLayer.sourceStartFrame, std::int64_t{1});
    }

    void overlappingVisualLayerIsExplicitlyRefused() {
        palmier::windows::ProjectPreviewProjection preview;
        std::exception_ptr failure;
        std::jthread worker([&] {
            try {
                constexpr auto source = R"({"timelines":[{"id":"timeline","fps":30,"width":1920,"height":1080,"tracks":[{"id":"bottom-track","type":"video","clips":[{"id":"bottom","mediaRef":"media-bottom","mediaType":"video","sourceClipType":"video","startFrame":0,"durationFrames":30}]},{"id":"top-track","type":"text","clips":[{"id":"top","mediaRef":"media-top","mediaType":"text","sourceClipType":"text","startFrame":10,"durationFrames":10}]}]}],"activeTimelineId":"timeline"})";
                const auto document = palmier::project::readProject(
                    source,
                    [] { return std::string("generated"); }
                );
                const palmier::project::MediaManifest manifest{{{
                    "media-bottom",
                    "video",
                    {palmier::project::MediaSourceKind::project, "project.json"},
                    true,
                }}};
                preview = palmier::windows::projectPreviewForActiveTimeline(
                    document,
                    manifest,
                    fixture("current-multitimeline.palmier"),
                    {}
                );
            } catch (...) {
                failure = std::current_exception();
            }
        });
        worker.join();
        if (failure) std::rethrow_exception(failure);
        QCOMPARE(
            preview.availability,
            palmier::windows::PreviewCandidateAvailability::unsupported
        );
        QCOMPARE(preview.reasonCode, std::string("overlappingVisibleLayer"));
        QVERIFY(!preview.candidate.has_value());
    }

    void modelPublishesReadOnlyTrackLayout() {
        palmier::windows::ReadOnlyTimelineModel model;
        auto projection = oneTrackProjection("timeline-main", "Main");
        auto& track = projection.timelines.front().tracks.front();
        track.clips = {
            {"clip-main-1", "video", 0, 150, 0, 0, 1.0, 0.0, 0.5},
            {"clip-main-2", "video", 150, 60, 0, 0, 1.0, 0.5, 0.2},
        };
        track.clipItems = {
            QVariantMap{
                {QStringLiteral("stableId"), QStringLiteral("clip-main-1")},
                {QStringLiteral("startFrameText"), QStringLiteral("0")},
                {QStringLiteral("durationFramesText"), QStringLiteral("150")},
                {QStringLiteral("offsetRatio"), 0.0},
                {QStringLiteral("extentRatio"), 0.5},
            },
            QVariantMap{
                {QStringLiteral("stableId"), QStringLiteral("clip-main-2")},
                {QStringLiteral("startFrameText"), QStringLiteral("150")},
                {QStringLiteral("durationFramesText"), QStringLiteral("60")},
                {QStringLiteral("offsetRatio"), 0.5},
                {QStringLiteral("extentRatio"), 0.2},
            },
        };
        model.replace(std::move(projection));
        QCOMPARE(model.rowCount(), 1);
        const auto trackIndex = model.index(0, 0);
        QVERIFY(trackIndex.isValid());
        QVERIFY(!(model.flags(trackIndex) & Qt::ItemIsEditable));
        QCOMPARE(model.data(trackIndex, palmier::windows::ReadOnlyTimelineModel::StableIdRole).toString(), QStringLiteral("track-1"));
        QCOMPARE(model.data(trackIndex, palmier::windows::ReadOnlyTimelineModel::TrackTypeRole).toString(), QStringLiteral("video"));
        const auto items = model.data(
            trackIndex,
            palmier::windows::ReadOnlyTimelineModel::ClipItemsRole
        ).toList();
        QCOMPARE(items.size(), 2);
        QCOMPARE(items.front().toMap().value(QStringLiteral("stableId")).toString(), QStringLiteral("clip-main-1"));
        QCOMPARE(items.back().toMap().value(QStringLiteral("startFrameText")).toString(), QStringLiteral("150"));
        QCOMPARE(items.back().toMap().value(QStringLiteral("durationFramesText")).toString(), QStringLiteral("60"));
        QCOMPARE(items.back().toMap().value(QStringLiteral("offsetRatio")).toDouble(), 0.5);
        QCOMPARE(items.back().toMap().value(QStringLiteral("extentRatio")).toDouble(), 0.2);
    }

    void failurePreservesPreviousModel() {
        int call = 0;
        palmier::windows::ProjectLoadCoordinator coordinator(
            [&call](const std::filesystem::path&, std::stop_token) {
                ++call;
                if (call == 2) throw std::runtime_error("fixture failure");
                return oneTrackProjection(
                    call == 1 ? "timeline-first" : "timeline-third",
                    call == 1 ? "First" : "Third"
                );
            },
            nullptr
        );
        QSignalSpy projectCommitted(
            &coordinator,
            &palmier::windows::ProjectLoadCoordinator::projectCommitted
        );
        coordinator.openFolder(QUrl::fromLocalFile(QStringLiteral("C:/first.palmier")));
        QTRY_VERIFY_WITH_TIMEOUT(!coordinator.loading(), 5000);
        QCOMPARE(projectCommitted.count(), 1);
        QCOMPARE(coordinator.committedGeneration(), std::uint64_t{1});
        const auto initialCount = coordinator.model()->rowCount();
        coordinator.openFolder(QUrl::fromLocalFile(QStringLiteral("C:/second.palmier")));
        QTRY_VERIFY_WITH_TIMEOUT(!coordinator.loading(), 5000);
        QCOMPARE(projectCommitted.count(), 1);
        QCOMPARE(coordinator.committedGeneration(), std::uint64_t{1});
        QCOMPARE(coordinator.model()->rowCount(), initialCount);
        QVERIFY(!coordinator.errorMessage().isEmpty());
        coordinator.openFolder(QUrl::fromLocalFile(QStringLiteral("C:/third.palmier")));
        QTRY_COMPARE_WITH_TIMEOUT(coordinator.state(), QStringLiteral("loaded"), 5000);
        QCOMPARE(projectCommitted.count(), 2);
        QCOMPARE(coordinator.committedGeneration(), std::uint64_t{3});
        QCOMPARE(
            coordinator.model()->project().activeTimelineId,
            std::string("timeline-third")
        );
        QVERIFY(coordinator.errorMessage().isEmpty());
        QVERIFY(coordinator.errorCode().isEmpty());
        QVERIFY(coordinator.errorJsonPointer().isEmpty());
    }

    void staleGenerationCannotReplaceNewerProject() {
        palmier::windows::ProjectLoadCoordinator* coordinatorPointer = nullptr;
        palmier::windows::ProjectLoadCoordinator coordinator(
            [&coordinatorPointer](const std::filesystem::path& path, std::stop_token cancellation) {
                if (path.filename() == L"first.palmier") {
                    QMetaObject::invokeMethod(
                        coordinatorPointer,
                        [coordinatorPointer] {
                            coordinatorPointer->openFolder(
                                QUrl::fromLocalFile(QStringLiteral("C:/second.palmier"))
                            );
                        },
                        Qt::QueuedConnection
                    );
                    return palmier::windows::loadProjectProjection(
                        fixture("current-multitimeline.palmier"), cancellation
                    );
                }
                return palmier::windows::loadProjectProjection(
                    fixture("legacy-bare-timeline.palmier"), cancellation
                );
            },
            nullptr
        );
        coordinatorPointer = &coordinator;
        coordinator.openFolder(QUrl::fromLocalFile(QStringLiteral("C:/first.palmier")));
        QTRY_COMPARE_WITH_TIMEOUT(coordinator.state(), QStringLiteral("loaded"), 5000);
        QCOMPARE(coordinator.model()->project().activeTimelineId, std::string("timeline-legacy"));
    }

    void consecutiveOpensKeepOnlyLatestPendingRequest() {
        std::vector<std::wstring> loadedPaths;
        std::mutex loadedPathsMutex;
        palmier::windows::ProjectLoadCoordinator coordinator(
            [&loadedPaths, &loadedPathsMutex](
                const std::filesystem::path& path,
                std::stop_token cancellation
            ) {
                {
                    const std::lock_guard lock(loadedPathsMutex);
                    loadedPaths.push_back(path.filename().wstring());
                }
                if (path.filename() == L"first.palmier") {
                    waitForCancellation(cancellation);
                }
                return palmier::windows::loadProjectProjection(
                    fixture("legacy-bare-timeline.palmier"), cancellation
                );
            },
            nullptr
        );
        coordinator.openFolder(QUrl::fromLocalFile(QStringLiteral("C:/first.palmier")));
        coordinator.openFolder(QUrl::fromLocalFile(QStringLiteral("C:/second.palmier")));
        coordinator.openFolder(QUrl::fromLocalFile(QStringLiteral("C:/third.palmier")));
        QTRY_COMPARE_WITH_TIMEOUT(coordinator.state(), QStringLiteral("loaded"), 5000);
        const std::lock_guard lock(loadedPathsMutex);
        QCOMPARE(loadedPaths.size(), std::size_t{2});
        QVERIFY(loadedPaths.front() == L"first.palmier");
        QVERIFY(loadedPaths.back() == L"third.palmier");
    }

    void cancellationReachesReader() {
        QSemaphore cancellationObserved;
        palmier::windows::ProjectLoadCoordinator coordinator(
            [&cancellationObserved](const std::filesystem::path&, std::stop_token cancellation)
                -> palmier::windows::ProjectProjection {
                waitForCancellation(cancellation);
                cancellationObserved.release();
                throw std::runtime_error("cancelled");
            },
            nullptr
        );
        coordinator.openFolder(QUrl::fromLocalFile(QStringLiteral("C:/cancel.palmier")));
        coordinator.cancelLoading();
        QCOMPARE(coordinator.state(), QStringLiteral("cancelling"));
        QTRY_COMPARE_WITH_TIMEOUT(cancellationObserved.available(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(coordinator.state(), QStringLiteral("empty"), 5000);
        QCOMPARE(coordinator.model()->rowCount(), 0);
    }

    void cancellationAfterWorkBeforeCommitRejectsResult() {
        palmier::windows::ProjectLoadCoordinator* coordinatorPointer = nullptr;
        palmier::windows::ProjectLoadCoordinator coordinator(
            [](const std::filesystem::path&, std::stop_token) {
                return oneTrackProjection("must-not-commit", "Cancelled");
            },
            [&coordinatorPointer] {
                QMetaObject::invokeMethod(
                    coordinatorPointer,
                    [coordinatorPointer] { coordinatorPointer->cancelLoading(); },
                    Qt::BlockingQueuedConnection
                );
            },
            nullptr
        );
        coordinatorPointer = &coordinator;
        coordinator.openFolder(QUrl::fromLocalFile(QStringLiteral("C:/late-cancel.palmier")));
        QTRY_COMPARE_WITH_TIMEOUT(coordinator.state(), QStringLiteral("empty"), 5000);
        QCOMPARE(coordinator.model()->rowCount(), 0);
    }

    void shutdownWaitsForAdmittedWorker() {
        QSemaphore cancellationObserved;
        palmier::windows::ProjectLoadCoordinator coordinator(
            [&cancellationObserved](const std::filesystem::path&, std::stop_token cancellation)
                -> palmier::windows::ProjectProjection {
                waitForCancellation(cancellation);
                cancellationObserved.release();
                throw palmier::windows::ProjectProjectionError("cancelled", "cancelled");
            },
            nullptr
        );
        QSignalSpy shutdownReady(&coordinator, &palmier::windows::ProjectLoadCoordinator::shutdownReady);
        coordinator.openFolder(QUrl::fromLocalFile(QStringLiteral("C:/shutdown.palmier")));
        QVERIFY(!coordinator.requestShutdown());
        QCOMPARE(coordinator.state(), QStringLiteral("cancelling"));
        QTRY_COMPARE_WITH_TIMEOUT(cancellationObserved.available(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(shutdownReady.count(), 1, 5000);
        QVERIFY(coordinator.requestShutdown());
        QCOMPARE(coordinator.state(), QStringLiteral("empty"));
    }

    void unsafeClipIsSkippedWithoutRejectingProject() {
        constexpr auto source = R"({"timelines":[{"id":"timeline","name":"Timeline","fps":30,"width":1920,"height":1080,"tracks":[{"type":"video","clips":[{"id":"bad","mediaRef":"media","startFrame":-1,"durationFrames":10},{"id":"good","mediaRef":"media","startFrame":5,"durationFrames":10}]}]}],"activeTimelineId":"timeline","openTimelineIds":["timeline"]})";
        palmier::windows::ProjectLoadCoordinator coordinator(
            [source](const std::filesystem::path&, std::stop_token cancellation) {
                return projectionFromJson(source, cancellation);
            },
            nullptr
        );
        coordinator.openFolder(QUrl::fromLocalFile(QStringLiteral("C:/unsafe.palmier")));
        QTRY_COMPARE_WITH_TIMEOUT(
            coordinator.state(),
            QStringLiteral("loadedWithWarnings"),
            5000
        );
        const auto index = coordinator.model()->index(0, 0);
        const auto clips = coordinator.model()->data(
            index,
            palmier::windows::ReadOnlyTimelineModel::ClipItemsRole
        ).toList();
        QCOMPARE(clips.size(), 1);
        QCOMPARE(
            clips.front().toMap().value(QStringLiteral("stableId")).toString(),
            QStringLiteral("good")
        );
        QVERIFY(coordinator.warningSummary().contains(QStringLiteral("synthesizedId")));
        QVERIFY(coordinator.warningSummary().contains(QStringLiteral("1 unsafe clip(s) omitted")));
    }

    void denseTimelineIsExplicitlyRejected() {
        const auto exactTrack = projectJsonWithClips(
            palmier::windows::maximumProjectedClipsPerTrack,
            "exact-track"
        );
        const auto overTrack = projectJsonWithClips(
            palmier::windows::maximumProjectedClipsPerTrack + 1,
            "over-track"
        );
        const std::vector<std::size_t> exactTotalCounts(
            palmier::windows::maximumProjectedClipsPerTimeline
                / palmier::windows::maximumProjectedClipsPerTrack,
            palmier::windows::maximumProjectedClipsPerTrack
        );
        auto overTotalCounts = exactTotalCounts;
        overTotalCounts.push_back(1);
        const auto exactTotal = projectJsonWithTrackClipCounts(exactTotalCounts, "exact-total");
        const auto overTotal = projectJsonWithTrackClipCounts(overTotalCounts, "over-total");
        const auto overTracks = projectJsonWithTrackClipCounts(
            std::vector<std::size_t>(
                palmier::windows::maximumProjectedTracksPerTimeline + 1,
                0
            ),
            "over-tracks"
        );
        palmier::windows::ProjectLoadCoordinator coordinator(
            [exactTrack, overTrack, exactTotal, overTotal, overTracks](
                const std::filesystem::path& path,
                std::stop_token cancellation
            ) {
                if (path.filename() == L"exact-track.palmier") {
                    return projectionFromJson(exactTrack, cancellation);
                }
                if (path.filename() == L"over-track.palmier") {
                    return projectionFromJson(overTrack, cancellation);
                }
                if (path.filename() == L"exact-total.palmier") {
                    return projectionFromJson(exactTotal, cancellation);
                }
                if (path.filename() == L"over-total.palmier") {
                    return projectionFromJson(overTotal, cancellation);
                }
                if (path.filename() == L"over-tracks.palmier") {
                    return projectionFromJson(overTracks, cancellation);
                }
                return oneTrackProjection("recovered", "Recovered");
            },
            nullptr
        );

        coordinator.openFolder(QUrl::fromLocalFile(QStringLiteral("C:/exact-track.palmier")));
        QTRY_COMPARE_WITH_TIMEOUT(coordinator.state(), QStringLiteral("loaded"), 10000);
        QCOMPARE(coordinator.model()->rowCount(), 1);

        coordinator.openFolder(QUrl::fromLocalFile(QStringLiteral("C:/over-track.palmier")));
        QTRY_COMPARE_WITH_TIMEOUT(coordinator.state(), QStringLiteral("failed"), 5000);
        QCOMPARE(coordinator.errorCode(), QStringLiteral("timelineTooDense"));
        QCOMPARE(coordinator.model()->project().activeTimelineId, std::string("exact-track"));

        coordinator.openFolder(QUrl::fromLocalFile(QStringLiteral("C:/exact-total.palmier")));
        QTRY_COMPARE_WITH_TIMEOUT(coordinator.state(), QStringLiteral("loaded"), 10000);
        QCOMPARE(
            coordinator.model()->rowCount(),
            static_cast<int>(exactTotalCounts.size())
        );

        coordinator.openFolder(QUrl::fromLocalFile(QStringLiteral("C:/over-tracks.palmier")));
        QTRY_COMPARE_WITH_TIMEOUT(coordinator.state(), QStringLiteral("failed"), 5000);
        QCOMPARE(coordinator.errorCode(), QStringLiteral("timelineTooDense"));
        QCOMPARE(coordinator.model()->project().activeTimelineId, std::string("exact-total"));

        coordinator.openFolder(QUrl::fromLocalFile(QStringLiteral("C:/over-total.palmier")));
        QTRY_COMPARE_WITH_TIMEOUT(coordinator.state(), QStringLiteral("failed"), 10000);
        QCOMPARE(coordinator.errorCode(), QStringLiteral("timelineTooDense"));
        QCOMPARE(coordinator.model()->project().activeTimelineId, std::string("exact-total"));

        coordinator.openFolder(QUrl::fromLocalFile(QStringLiteral("C:/recovered.palmier")));
        QTRY_COMPARE_WITH_TIMEOUT(coordinator.state(), QStringLiteral("loaded"), 5000);
        QCOMPARE(coordinator.model()->project().activeTimelineId, std::string("recovered"));
        QVERIFY(coordinator.errorCode().isEmpty());
        QVERIFY(coordinator.errorJsonPointer().isEmpty());
    }

    void structuredErrorsPreserveStableDetails() {
        palmier::windows::ProjectLoadCoordinator domainCoordinator(
            [](const std::filesystem::path&, std::stop_token cancellation) {
                return projectionFromJson(R"({"timelines":[]})", cancellation);
            },
            nullptr
        );
        domainCoordinator.openFolder(QUrl::fromLocalFile(QStringLiteral("C:/invalid.palmier")));
        QTRY_COMPARE_WITH_TIMEOUT(domainCoordinator.state(), QStringLiteral("failed"), 5000);
        QCOMPARE(domainCoordinator.errorCode(), QStringLiteral("emptyTimelines"));
        QCOMPARE(domainCoordinator.errorJsonPointer(), QStringLiteral("/timelines"));

        palmier::windows::ProjectLoadCoordinator packageCoordinator;
        packageCoordinator.openFolder(QUrl::fromLocalFile(QString::fromStdWString(
            (fixture("current-multitimeline.palmier") / "nested-missing.palmier").wstring()
        )));
        QTRY_COMPARE_WITH_TIMEOUT(packageCoordinator.state(), QStringLiteral("failed"), 5000);
        QCOMPARE(packageCoordinator.errorCode(), QStringLiteral("invalidPackagePath"));
        QVERIFY(packageCoordinator.errorJsonPointer().isEmpty());
    }

    void diagnosticsProduceLoadedWithWarnings() {
        auto projection = oneTrackProjection("timeline-warning", "Warning");
        projection.diagnosticCount = 1;
        projection.firstDiagnostic = {"synthesizedId", "/timelines/0/id"};
        palmier::windows::ProjectLoadCoordinator coordinator(
            [projection](const std::filesystem::path&, std::stop_token) { return projection; },
            nullptr
        );
        coordinator.openFolder(QUrl::fromLocalFile(QStringLiteral("C:/warning.palmier")));
        QTRY_COMPARE_WITH_TIMEOUT(coordinator.state(), QStringLiteral("loadedWithWarnings"), 5000);
        QVERIFY(coordinator.warningSummary().contains(QStringLiteral("synthesizedId")));
        QCOMPARE(coordinator.model()->rowCount(), 1);
    }

    void qmlLoadsOffscreen() {
        palmier::windows::ProjectLoadCoordinator coordinator(
            [](const std::filesystem::path& path, std::stop_token) {
                if (path.filename() == L"missing.palmier") {
                    throw std::runtime_error("fixture failure");
                }
                auto projection = oneTrackProjection("timeline", "Timeline");
                if (path.filename() == L"warning.palmier") {
                    projection.diagnosticCount = 1;
                    projection.firstDiagnostic = {"synthesizedId", "/timelines/0/id"};
                }
                return projection;
            },
            nullptr
        );
        palmier::windows::PreviewPresentationController previewController;
        auto persistenceMailbox = std::make_shared<palmier::windows::ProjectRuntimeMailbox>();
        auto persistenceRuntime = std::make_shared<palmier::project::ProjectRuntime>(
            persistenceMailbox
        );
        palmier::windows::ProjectPersistenceController persistenceController(
            persistenceRuntime,
            persistenceMailbox,
            nullptr
        );
        palmier::windows::ProjectEditingController editingController(
            persistenceRuntime,
            persistenceMailbox,
            nullptr
        );
        palmier::windows::ProjectExportController exportController(persistenceMailbox);
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("projectCoordinator"), &coordinator);
        engine.rootContext()->setContextProperty(
            QStringLiteral("editingCoordinator"),
            &editingController
        );
        engine.rootContext()->setContextProperty(
            QStringLiteral("persistenceCoordinator"),
            &persistenceController
        );
        engine.rootContext()->setContextProperty(
            QStringLiteral("exportCoordinator"),
            &exportController
        );
        engine.rootContext()->setContextProperty(
            QStringLiteral("previewCoordinator"),
            &previewController
        );
        engine.load(QUrl::fromLocalFile(QStringLiteral(PALMIER_QML_MAIN_FILE)));
        QVERIFY2(!engine.rootObjects().isEmpty(), "Main QML did not load");
        auto* root = engine.rootObjects().front();
        QVERIFY(root->findChild<QObject*>(QStringLiteral("timelineTracks")) != nullptr);
        QVERIFY(
            root->findChild<QObject*>(QStringLiteral("exportSelectedClipButton"))
                != nullptr
        );
        QVERIFY(root->findChild<QObject*>(QStringLiteral("cancelExportButton")) != nullptr);
        QVERIFY(root->findChild<QObject*>(QStringLiteral("exportCloseDialog")) != nullptr);
        QVERIFY(root->findChild<QObject*>(QStringLiteral("saveAsButton")) != nullptr);
        QVERIFY(root->findChild<QObject*>(QStringLiteral("saveAsDialog")) != nullptr);
        QVERIFY(root->findChild<QObject*>(QStringLiteral("cancelSaveButton")) != nullptr);
        QVERIFY(root->findChild<QObject*>(QStringLiteral("moveTrackField")) != nullptr);
        QVERIFY(root->findChild<QObject*>(QStringLiteral("moveFrameField")) != nullptr);
        QVERIFY(root->findChild<QObject*>(QStringLiteral("moveClipButton")) != nullptr);
        QVERIFY(root->findChild<QObject*>(QStringLiteral("removeClipButton")) != nullptr);
        QVERIFY(root->findChild<QObject*>(QStringLiteral("undoButton")) != nullptr);
        QVERIFY(root->findChild<QObject*>(QStringLiteral("redoButton")) != nullptr);
        QVERIFY(root->findChild<QObject*>(QStringLiteral("durationFramesField")) != nullptr);
        QVERIFY(root->findChild<QObject*>(QStringLiteral("trimStartFrameField")) != nullptr);
        QVERIFY(root->findChild<QObject*>(QStringLiteral("trimEndFrameField")) != nullptr);
        QVERIFY(root->findChild<QObject*>(QStringLiteral("clipSpeedField")) != nullptr);
        QVERIFY(root->findChild<QObject*>(QStringLiteral("setClipTimingButton")) != nullptr);
        QVERIFY(root->findChild<QObject*>(QStringLiteral("previewViewport")) != nullptr);
        QVERIFY(root->findChild<QObject*>(QStringLiteral("pausePreviewButton")) != nullptr);
        QVERIFY(root->findChild<QObject*>(QStringLiteral("resumePreviewButton")) != nullptr);
        QVERIFY(root->findChild<QObject*>(QStringLiteral("previousPreviewFrameButton")) != nullptr);
        QVERIFY(root->findChild<QObject*>(QStringLiteral("nextPreviewFrameButton")) != nullptr);
        QVERIFY(root->findChild<QObject*>(QStringLiteral("previewFrameField")) != nullptr);
        QVERIFY(root->findChild<QObject*>(QStringLiteral("seekPreviewFrameButton")) != nullptr);
        QVERIFY(root->findChild<QObject*>(QStringLiteral("previewTransportState")) != nullptr);
        QVERIFY(root->findChild<QObject*>(QStringLiteral("previewErrorState")) != nullptr);
        QVERIFY(root->findChild<QObject*>(QStringLiteral("emptyState")) != nullptr);
        auto* emptyState = root->findChild<QObject*>(QStringLiteral("emptyState"));
        auto* loadedState = root->findChild<QObject*>(QStringLiteral("loadedState"));
        auto* errorState = root->findChild<QObject*>(QStringLiteral("errorState"));
        auto* warningState = root->findChild<QObject*>(QStringLiteral("warningState"));
        QVERIFY(loadedState != nullptr);
        QVERIFY(errorState != nullptr);
        QVERIFY(warningState != nullptr);
        QVERIFY(emptyState->property("visible").toBool());

        coordinator.openFolder(QUrl::fromLocalFile(QStringLiteral("C:/loaded.palmier")));
        QTRY_COMPARE_WITH_TIMEOUT(coordinator.state(), QStringLiteral("loaded"), 5000);
        QVERIFY(loadedState->property("visible").toBool());

        coordinator.openFolder(QUrl::fromLocalFile(QStringLiteral("C:/warning.palmier")));
        QTRY_COMPARE_WITH_TIMEOUT(
            coordinator.state(),
            QStringLiteral("loadedWithWarnings"),
            5000
        );
        QVERIFY(warningState->property("visible").toBool());

        coordinator.openFolder(QUrl::fromLocalFile(QStringLiteral("C:/missing.palmier")));
        QTRY_COMPARE_WITH_TIMEOUT(coordinator.state(), QStringLiteral("failed"), 5000);
        QVERIFY(errorState->property("visible").toBool());
        QVERIFY(coordinator.model()->rowCount() > 0);
    }

    void qmlCloseWaitsForActiveWorker() {
        QSemaphore cancellationObserved;
        palmier::windows::ProjectLoadCoordinator coordinator(
            [&cancellationObserved](const std::filesystem::path&, std::stop_token cancellation)
                -> palmier::windows::ProjectProjection {
                waitForCancellation(cancellation);
                cancellationObserved.release();
                throw palmier::windows::ProjectProjectionError("cancelled", "cancelled");
            },
            nullptr
        );
        FakePreviewShutdownCoordinator previewController;
        auto persistenceMailbox = std::make_shared<palmier::windows::ProjectRuntimeMailbox>();
        auto persistenceRuntime = std::make_shared<palmier::project::ProjectRuntime>(
            persistenceMailbox
        );
        palmier::windows::ProjectPersistenceController persistenceController(
            persistenceRuntime,
            persistenceMailbox,
            nullptr
        );
        palmier::windows::ProjectEditingController editingController(
            persistenceRuntime,
            persistenceMailbox,
            nullptr
        );
        palmier::windows::ProjectExportController exportController(persistenceMailbox);
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("projectCoordinator"), &coordinator);
        engine.rootContext()->setContextProperty(
            QStringLiteral("editingCoordinator"),
            &editingController
        );
        engine.rootContext()->setContextProperty(
            QStringLiteral("persistenceCoordinator"),
            &persistenceController
        );
        engine.rootContext()->setContextProperty(
            QStringLiteral("exportCoordinator"),
            &exportController
        );
        engine.rootContext()->setContextProperty(
            QStringLiteral("previewCoordinator"),
            &previewController
        );
        engine.load(QUrl::fromLocalFile(QStringLiteral(PALMIER_QML_MAIN_FILE)));
        QVERIFY2(!engine.rootObjects().isEmpty(), "Main QML did not load");
        auto* window = qobject_cast<QWindow*>(engine.rootObjects().front());
        QVERIFY(window != nullptr);
        QSignalSpy shutdownReady(&coordinator, &palmier::windows::ProjectLoadCoordinator::shutdownReady);

        coordinator.openFolder(QUrl::fromLocalFile(QStringLiteral("C:/closing.palmier")));
        QTRY_VERIFY_WITH_TIMEOUT(coordinator.loading(), 5000);
        QVERIFY(window->isVisible());
        window->close();
        QVERIFY(window->isVisible());
        QVERIFY(previewController.requested());
        QTRY_COMPARE_WITH_TIMEOUT(cancellationObserved.available(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(shutdownReady.count(), 1, 5000);
        QVERIFY(window->isVisible());
        previewController.completeShutdown();
        QTRY_VERIFY_WITH_TIMEOUT(!window->isVisible(), 5000);
    }
};

}

int main(int argc, char* argv[]) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    QGuiApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    QtShellTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "qt_shell_tests.moc"
