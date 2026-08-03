#include "palmier/windows/project_export_controller.hpp"

#include "palmier/project/project_reader.hpp"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QTest>
#include <QThread>

#include <condition_variable>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace {

palmier::project::ProjectDocument document() {
    constexpr auto source = R"({
        "timelines":[{
            "id":"timeline","fps":10,"width":64,"height":64,
            "tracks":[{
                "id":"track","type":"video","clips":[{
                    "id":"clip","mediaRef":"media","mediaType":"video",
                    "sourceClipType":"video","startFrame":0,"durationFrames":5,
                    "trimStartFrame":0,"trimEndFrame":0,"speed":1,
                    "opacity":1,"blendMode":"normal"
                }]
            }]
        }],
        "activeTimelineId":"timeline"
    })";
    return palmier::project::readProject(source, [] {
        return std::string("unexpected-generated-id");
    });
}

std::shared_ptr<const palmier::project::ProjectSessionSnapshot> snapshot(
    std::uint64_t revision
) {
    return std::make_shared<const palmier::project::ProjectSessionSnapshot>(
        palmier::project::ProjectSessionSnapshot{
            document(),
            revision,
            revision,
            0,
            0,
        }
    );
}

palmier::exporting::H264ProjectExportReceipt receipt(
    const palmier::exporting::ProjectTimelineH264ExportRequest& request
) {
    return {
        request.destination,
        "h264_mf",
        5,
        5,
        64,
        64,
        10,
    };
}

bool waitForSignal(QSignalSpy& spy) {
    return !spy.isEmpty() || spy.wait(5'000);
}

class ExportGate final {
public:
    using Waiter = std::function<bool(std::stop_token, bool)>;

    Waiter waiter() const {
        const auto state = state_;
        return [state](std::stop_token cancellation, bool honorCancellation) {
            std::unique_lock lock(state->mutex);
            state->entered = true;
            state->condition.notify_all();
            if (!honorCancellation) {
                state->condition.wait(lock, [&] { return state->released; });
                return true;
            }
            return state->condition.wait(
                lock,
                cancellation,
                [&] { return state->released; }
            );
        };
    }

    bool entered() const {
        const std::lock_guard lock(state_->mutex);
        return state_->entered;
    }

    void release() {
        {
            const std::lock_guard lock(state_->mutex);
            state_->released = true;
        }
        state_->condition.notify_all();
    }

private:
    struct State final {
        std::mutex mutex;
        std::condition_variable_any condition;
        bool entered{};
        bool released{};
    };

    std::shared_ptr<State> state_{std::make_shared<State>()};
};

class ExportGateReleaser final {
public:
    explicit ExportGateReleaser(ExportGate& gate) : gate_(&gate) {}
    ~ExportGateReleaser() {
        if (gate_ != nullptr) gate_->release();
    }

    ExportGateReleaser(const ExportGateReleaser&) = delete;
    ExportGateReleaser& operator=(const ExportGateReleaser&) = delete;

    void cancel() noexcept { gate_ = nullptr; }

private:
    ExportGate* gate_;
};

struct ControllerFixture final {
    std::shared_ptr<palmier::windows::ProjectRuntimeMailbox> mailbox{
        std::make_shared<palmier::windows::ProjectRuntimeMailbox>()
    };

    ControllerFixture() {
        mailbox->statePublished({1, snapshot(0)});
    }
};

class ProjectExportControllerTests final : public QObject {
    Q_OBJECT

private slots:
    void ownsOneBackgroundJobAndRefusesDuplicateAdmission() {
        ControllerFixture fixture;
        ExportGate gate;
        const auto waitAtGate = gate.waiter();
        std::atomic_bool ranOffGuiThread{};
        std::atomic_bool receivedTimelineRequest{};
        std::atomic_int calls{};
        palmier::windows::ProjectExportController controller(
            fixture.mailbox,
            [&, waitAtGate](
                const palmier::project::ProjectDocument&,
                const palmier::exporting::ProjectTimelineH264ExportRequest& request,
                const palmier::exporting::H264ExportLimits&,
                std::stop_token cancellation
            ) {
                ++calls;
                ranOffGuiThread = QThread::currentThread()
                    != QCoreApplication::instance()->thread();
                receivedTimelineRequest = request.packagePath
                        == std::filesystem::path(L"C:\\project.palmier")
                    && request.destination.filename()
                        == std::filesystem::path(L"first.mp4");
                if (!waitAtGate(cancellation, true)) {
                    throw palmier::exporting::H264ExportError(
                        palmier::exporting::H264ExportFailureCode::cancelled,
                        "gatedExport",
                        "cancelled"
                    );
                }
                return receipt(request);
            },
            nullptr
        );
        ExportGateReleaser releaseGateOnExit(gate);
        controller.activateProject(L"C:\\project.palmier", 1, 0);
        QSignalSpy refused(
            &controller,
            &palmier::windows::ProjectExportController::requestRefused
        );
        QSignalSpy finished(
            &controller,
            &palmier::windows::ProjectExportController::exportFinished
        );
        controller.exportTimeline(
            QUrl::fromLocalFile(QStringLiteral("C:/first.mp4"))
        );
        QTRY_VERIFY_WITH_TIMEOUT(gate.entered(), 5000);
        QVERIFY(controller.exporting());
        QVERIFY(controller.canCancel());
        controller.exportTimeline(
            QUrl::fromLocalFile(QStringLiteral("C:/second.mp4"))
        );
        QCOMPARE(refused.count(), 1);
        QCOMPARE(refused.at(0).at(0).toString(), QStringLiteral("exportBusy"));
        gate.release();
        releaseGateOnExit.cancel();
        QVERIFY(waitForSignal(finished));
        QCOMPARE(calls.load(), 1);
        QVERIFY(ranOffGuiThread.load());
        QVERIFY(receivedTimelineRequest.load());
        QVERIFY(!controller.exporting());
        QCOMPARE(controller.state(), QStringLiteral("completed"));
        QCOMPARE(controller.outputPath(), QStringLiteral("C:\\first.mp4"));
    }

    void committedOutputFromChangedRevisionIsCompletedOutdated() {
        ControllerFixture fixture;
        ExportGate gate;
        const auto waitAtGate = gate.waiter();
        palmier::windows::ProjectExportController controller(
            fixture.mailbox,
            [&, waitAtGate](
                const palmier::project::ProjectDocument&,
                const palmier::exporting::ProjectTimelineH264ExportRequest& request,
                const palmier::exporting::H264ExportLimits&,
                std::stop_token cancellation
            ) {
                static_cast<void>(waitAtGate(cancellation, false));
                return receipt(request);
            },
            nullptr
        );
        ExportGateReleaser releaseGateOnExit(gate);
        controller.activateProject(L"C:\\project.palmier", 1, 0);
        QSignalSpy finished(
            &controller,
            &palmier::windows::ProjectExportController::exportFinished
        );
        controller.exportTimeline(
            QUrl::fromLocalFile(QStringLiteral("C:/outdated.mp4"))
        );
        QTRY_VERIFY_WITH_TIMEOUT(gate.entered(), 5000);
        fixture.mailbox->statePublished({1, snapshot(1)});
        const auto publication = fixture.mailbox->latest();
        QVERIFY(publication.has_value());
        controller.observeRuntimePublication(*publication);
        QCOMPARE(controller.state(), QStringLiteral("cancelling"));
        gate.release();
        releaseGateOnExit.cancel();
        QVERIFY(waitForSignal(finished));
        QCOMPARE(finished.at(0).at(0).toBool(), true);
        QCOMPARE(controller.state(), QStringLiteral("completedOutdated"));
        QCOMPARE(controller.warningCode(), QStringLiteral("exportedOlderState"));
        QCOMPARE(controller.outputPath(), QStringLiteral("C:\\outdated.mp4"));
        controller.activateProject(L"C:\\project.palmier", 1, 1, true);
        QCOMPARE(controller.state(), QStringLiteral("completedOutdated"));
        QCOMPARE(controller.warningCode(), QStringLiteral("exportedOlderState"));
        QCOMPARE(controller.outputPath(), QStringLiteral("C:\\outdated.mp4"));
    }

    void pendingPresentationRefusesTimelineExport() {
        ControllerFixture fixture;
        std::atomic_int calls{};
        palmier::windows::ProjectExportController controller(
            fixture.mailbox,
            [&](const auto&, const auto& request, const auto&, std::stop_token) {
                ++calls;
                return receipt(request);
            },
            nullptr
        );
        controller.activateProject(L"C:\\project.palmier", 1, 0, false);
        QCOMPARE(controller.state(), QStringLiteral("pendingPresentation"));
        controller.activateProject(L"C:\\project.palmier", 1, 0, true);
        QCOMPARE(controller.state(), QStringLiteral("idle"));
        controller.activateProject(L"C:\\project.palmier", 1, 0, false);
        QSignalSpy finished(
            &controller,
            &palmier::windows::ProjectExportController::exportFinished
        );
        controller.exportTimeline(
            QUrl::fromLocalFile(QStringLiteral("C:/pending.mp4"))
        );
        QCOMPARE(finished.count(), 1);
        QCOMPARE(calls.load(), 0);
        QCOMPARE(controller.state(), QStringLiteral("failed"));
        QCOMPARE(controller.errorCode(), QStringLiteral("presentationPending"));
        controller.activateProject(L"C:\\project.palmier", 1, 0, true);
        QCOMPARE(controller.state(), QStringLiteral("idle"));
        QCOMPARE(controller.errorCode(), QString{});
        QCOMPARE(controller.errorMessage(), QString{});
    }

    void persistenceOnlyPublicationDoesNotCancel() {
        ControllerFixture fixture;
        fixture.mailbox->statePublished({
            1,
            std::make_shared<const palmier::project::ProjectSessionSnapshot>(
                palmier::project::ProjectSessionSnapshot{document(), 0, 1, 0, 0}
            ),
        });
        ExportGate gate;
        const auto waitAtGate = gate.waiter();
        palmier::windows::ProjectExportController controller(
            fixture.mailbox,
            [&, waitAtGate](const auto&, const auto& request, const auto&, std::stop_token cancellation) {
                if (!waitAtGate(cancellation, true)) {
                    throw palmier::exporting::H264ExportError(
                        palmier::exporting::H264ExportFailureCode::cancelled,
                        "gatedExport",
                        "cancelled"
                    );
                }
                return receipt(request);
            },
            nullptr
        );
        ExportGateReleaser releaseGateOnExit(gate);
        controller.activateProject(L"C:\\project.palmier", 1, 0);
        QSignalSpy finished(
            &controller,
            &palmier::windows::ProjectExportController::exportFinished
        );
        controller.exportTimeline(
            QUrl::fromLocalFile(QStringLiteral("C:/saved.mp4"))
        );
        QTRY_VERIFY_WITH_TIMEOUT(gate.entered(), 5000);
        fixture.mailbox->statePublished({
            1,
            std::make_shared<const palmier::project::ProjectSessionSnapshot>(
                palmier::project::ProjectSessionSnapshot{document(), 0, 1, 1, 0}
            ),
        });
        const auto publication = fixture.mailbox->latest();
        QVERIFY(publication.has_value());
        controller.observeRuntimePublication(*publication);
        QCOMPARE(controller.state(), QStringLiteral("exporting"));
        gate.release();
        releaseGateOnExit.cancel();
        QVERIFY(waitForSignal(finished));
        QCOMPARE(controller.state(), QStringLiteral("completed"));
    }

    void completedReceiptBecomesOutdatedAfterLaterRevision() {
        ControllerFixture fixture;
        palmier::windows::ProjectExportController controller(
            fixture.mailbox,
            [](const auto&, const auto& request, const auto&, std::stop_token) {
                return receipt(request);
            },
            nullptr
        );
        controller.activateProject(L"C:\\project.palmier", 1, 0);
        QSignalSpy finished(
            &controller,
            &palmier::windows::ProjectExportController::exportFinished
        );
        controller.exportTimeline(
            QUrl::fromLocalFile(QStringLiteral("C:/current.mp4"))
        );
        QVERIFY(waitForSignal(finished));
        QCOMPARE(controller.state(), QStringLiteral("completed"));
        fixture.mailbox->statePublished({1, snapshot(1)});
        const auto publication = fixture.mailbox->latest();
        QVERIFY(publication.has_value());
        controller.observeRuntimePublication(*publication);
        QCOMPARE(controller.state(), QStringLiteral("completedOutdated"));
        QCOMPARE(controller.warningCode(), QStringLiteral("exportedOlderState"));
        QCOMPARE(controller.outputPath(), QStringLiteral("C:\\current.mp4"));
    }

    void packageChangeCancelsAndMarksCommittedOutputOutdated() {
        ControllerFixture fixture;
        ExportGate gate;
        const auto waitAtGate = gate.waiter();
        palmier::windows::ProjectExportController controller(
            fixture.mailbox,
            [&, waitAtGate](const auto&, const auto& request, const auto&, std::stop_token cancellation) {
                static_cast<void>(waitAtGate(cancellation, false));
                return receipt(request);
            },
            nullptr
        );
        ExportGateReleaser releaseGateOnExit(gate);
        controller.activateProject(L"C:\\first.palmier", 1, 0);
        QSignalSpy finished(
            &controller,
            &palmier::windows::ProjectExportController::exportFinished
        );
        controller.exportTimeline(
            QUrl::fromLocalFile(QStringLiteral("C:/moved.mp4"))
        );
        QTRY_VERIFY_WITH_TIMEOUT(gate.entered(), 5000);
        controller.activateProject(L"C:\\second.palmier", 1, 0);
        QCOMPARE(controller.state(), QStringLiteral("cancelling"));
        gate.release();
        releaseGateOnExit.cancel();
        QVERIFY(waitForSignal(finished));
        QCOMPARE(controller.state(), QStringLiteral("completedOutdated"));
        QCOMPARE(controller.warningCode(), QStringLiteral("exportedOlderState"));
    }

    void shutdownCancelsAndDrainsOneAdmittedJob() {
        ControllerFixture fixture;
        ExportGate gate;
        const auto waitAtGate = gate.waiter();
        palmier::windows::ProjectExportController controller(
            fixture.mailbox,
            [&, waitAtGate](
                const palmier::project::ProjectDocument&,
                const palmier::exporting::ProjectTimelineH264ExportRequest& request,
                const palmier::exporting::H264ExportLimits&,
                std::stop_token cancellation
            ) {
                if (!waitAtGate(cancellation, true)) {
                    throw palmier::exporting::H264ExportError(
                        palmier::exporting::H264ExportFailureCode::cancelled,
                        "gatedExport",
                        "cancelled"
                    );
                }
                return receipt(request);
            },
            nullptr
        );
        ExportGateReleaser releaseGateOnExit(gate);
        controller.activateProject(L"C:\\project.palmier", 1, 0);
        QSignalSpy shutdownReady(
            &controller,
            &palmier::windows::ProjectExportController::shutdownReady
        );
        controller.exportTimeline(
            QUrl::fromLocalFile(QStringLiteral("C:/cancelled.mp4"))
        );
        QTRY_VERIFY_WITH_TIMEOUT(gate.entered(), 5000);
        QVERIFY(!controller.requestShutdown());
        QCOMPARE(controller.state(), QStringLiteral("cancelling"));
        QVERIFY(waitForSignal(shutdownReady));
        QCOMPARE(shutdownReady.count(), 1);
        QVERIFY(controller.requestShutdown());
        QCOMPARE(shutdownReady.count(), 1);
        QVERIFY(!controller.exporting());
        QCOMPARE(controller.state(), QStringLiteral("cancelled"));
        QCOMPARE(controller.outputPath(), QString{});
        releaseGateOnExit.cancel();
    }

    void shutdownRejectsNewAdmission() {
        ControllerFixture fixture;
        std::atomic_int calls{};
        palmier::windows::ProjectExportController controller(
            fixture.mailbox,
            [&](const auto&, const auto& request, const auto&, std::stop_token) {
                ++calls;
                return receipt(request);
            },
            nullptr
        );
        controller.activateProject(L"C:\\project.palmier", 1, 0);
        QVERIFY(controller.requestShutdown());
        QSignalSpy refused(
            &controller,
            &palmier::windows::ProjectExportController::requestRefused
        );
        controller.exportTimeline(
            QUrl::fromLocalFile(QStringLiteral("C:/late.mp4"))
        );
        QCOMPARE(refused.count(), 1);
        QCOMPARE(refused.at(0).at(0).toString(), QStringLiteral("shutdownInProgress"));
        QCOMPARE(calls.load(), 0);
    }

    void mapsEveryExporterFailureCode() {
        using Code = palmier::exporting::H264ExportFailureCode;
        const std::vector<std::pair<Code, QString>> cases{
            {Code::invalidRequest, QStringLiteral("invalidRequest")},
            {Code::unsupportedProject, QStringLiteral("unsupportedProject")},
            {Code::resourceLimitExceeded, QStringLiteral("resourceLimitExceeded")},
            {Code::unsupportedSourceTiming, QStringLiteral("unsupportedSourceTiming")},
            {Code::unsupportedEncoder, QStringLiteral("unsupportedEncoder")},
            {Code::mediaUnavailable, QStringLiteral("mediaUnavailable")},
            {Code::sourceEndedEarly, QStringLiteral("sourceEndedEarly")},
            {Code::encodeFailed, QStringLiteral("encodeFailed")},
            {Code::verificationFailed, QStringLiteral("verificationFailed")},
            {Code::destinationExists, QStringLiteral("destinationExists")},
            {Code::stagingFailed, QStringLiteral("stagingFailed")},
            {Code::cleanupFailed, QStringLiteral("cleanupFailed")},
            {Code::installFailed, QStringLiteral("installFailed")},
        };
        for (const auto& [failure, expected] : cases) {
            ControllerFixture fixture;
            palmier::windows::ProjectExportController controller(
                fixture.mailbox,
                [failure](
                    const palmier::project::ProjectDocument&,
                    const palmier::exporting::ProjectTimelineH264ExportRequest&,
                    const palmier::exporting::H264ExportLimits&,
                    std::stop_token
                ) -> palmier::exporting::H264ProjectExportReceipt {
                    throw palmier::exporting::H264ExportError(
                        failure,
                        "testStage",
                        "test failure"
                    );
                },
                nullptr
            );
            controller.activateProject(L"C:\\project.palmier", 1, 0);
            QSignalSpy finished(
                &controller,
                &palmier::windows::ProjectExportController::exportFinished
            );
            controller.exportTimeline(
                QUrl::fromLocalFile(QStringLiteral("C:/failure.mp4"))
            );
            QVERIFY(waitForSignal(finished));
            QCOMPARE(controller.state(), QStringLiteral("failed"));
            QCOMPARE(controller.errorCode(), expected);
            QCOMPARE(controller.errorStage(), QStringLiteral("testStage"));
            QCOMPARE(controller.outputPath(), QString{});
        }
    }
};

}

QTEST_GUILESS_MAIN(ProjectExportControllerTests)

#include "project_export_controller_tests.moc"
