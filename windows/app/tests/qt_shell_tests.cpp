#include "palmier/windows/project_load_coordinator.hpp"
#include "palmier/windows/project_projection_loader.hpp"
#include "palmier/project/project_reader.hpp"

#include <QGuiApplication>
#include <QMetaObject>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTest>
#include <QVariantMap>
#include <QWindow>

#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

std::filesystem::path fixture(const char* name) {
    return std::filesystem::path(PALMIER_REPOSITORY_ROOT)
        / "fixtures" / "contracts" / "projects" / name;
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

class QtShellTests final : public QObject {
    Q_OBJECT

private slots:
    void readerMapsCurrentProject() {
        palmier::windows::ProjectLoadCoordinator coordinator;
        coordinator.openFolder(QUrl::fromLocalFile(QString::fromStdWString(
            fixture("current-multitimeline.palmier").wstring()
        )));
        QTRY_COMPARE_WITH_TIMEOUT(coordinator.state(), QStringLiteral("loaded"), 5000);
        const auto& project = coordinator.model()->project();
        QCOMPARE(project.timelines.size(), std::size_t{1});
        QCOMPARE(project.activeTimelineId, std::string("timeline-main"));
        QCOMPARE(project.timelines.front().tracks.front().clips.front().id, std::string("clip-main-1"));
    }

    void modelPublishesReadOnlyTrackLayout() {
        palmier::windows::ReadOnlyTimelineModel model;
        auto projection = oneTrackProjection("timeline-main", "Main");
        auto& track = projection.timelines.front().tracks.front();
        track.clips = {
            {"clip-main-1", "video", 0, 150, 0.0, 0.5},
            {"clip-main-2", "video", 150, 60, 0.5, 0.2},
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
        coordinator.openFolder(QUrl::fromLocalFile(QStringLiteral("C:/first.palmier")));
        QTRY_VERIFY_WITH_TIMEOUT(!coordinator.loading(), 5000);
        const auto initialCount = coordinator.model()->rowCount();
        coordinator.openFolder(QUrl::fromLocalFile(QStringLiteral("C:/second.palmier")));
        QTRY_VERIFY_WITH_TIMEOUT(!coordinator.loading(), 5000);
        QCOMPARE(coordinator.model()->rowCount(), initialCount);
        QVERIFY(!coordinator.errorMessage().isEmpty());
        coordinator.openFolder(QUrl::fromLocalFile(QStringLiteral("C:/third.palmier")));
        QTRY_COMPARE_WITH_TIMEOUT(coordinator.state(), QStringLiteral("loaded"), 5000);
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
                    std::mutex mutex;
                    std::condition_variable_any condition;
                    std::unique_lock lock(mutex);
                    condition.wait(lock, cancellation, [] { return false; });
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
                std::mutex mutex;
                std::condition_variable_any condition;
                std::unique_lock lock(mutex);
                condition.wait(lock, cancellation, [] { return false; });
                cancellationObserved.release();
                throw std::runtime_error("cancelled");
            },
            nullptr
        );
        coordinator.openFolder(QUrl::fromLocalFile(QStringLiteral("C:/cancel.palmier")));
        coordinator.cancelLoading();
        QCOMPARE(coordinator.state(), QStringLiteral("cancelling"));
        QTRY_VERIFY_WITH_TIMEOUT(cancellationObserved.tryAcquire(), 5000);
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
                std::mutex mutex;
                std::condition_variable_any condition;
                std::unique_lock lock(mutex);
                condition.wait(lock, cancellation, [] { return false; });
                cancellationObserved.release();
                throw palmier::windows::ProjectProjectionError("cancelled", "cancelled");
            },
            nullptr
        );
        QSignalSpy shutdownReady(&coordinator, &palmier::windows::ProjectLoadCoordinator::shutdownReady);
        coordinator.openFolder(QUrl::fromLocalFile(QStringLiteral("C:/shutdown.palmier")));
        QVERIFY(!coordinator.requestShutdown());
        QCOMPARE(coordinator.state(), QStringLiteral("cancelling"));
        QTRY_VERIFY_WITH_TIMEOUT(cancellationObserved.tryAcquire(), 5000);
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
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("projectCoordinator"), &coordinator);
        engine.load(QUrl::fromLocalFile(QStringLiteral(PALMIER_QML_MAIN_FILE)));
        QVERIFY2(!engine.rootObjects().isEmpty(), "Main QML did not load");
        auto* root = engine.rootObjects().front();
        QVERIFY(root->findChild<QObject*>(QStringLiteral("timelineTracks")) != nullptr);
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
                std::mutex mutex;
                std::condition_variable_any condition;
                std::unique_lock lock(mutex);
                condition.wait(lock, cancellation, [] { return false; });
                cancellationObserved.release();
                throw palmier::windows::ProjectProjectionError("cancelled", "cancelled");
            },
            nullptr
        );
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("projectCoordinator"), &coordinator);
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
        QTRY_VERIFY_WITH_TIMEOUT(cancellationObserved.tryAcquire(), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(shutdownReady.count(), 1, 5000);
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
