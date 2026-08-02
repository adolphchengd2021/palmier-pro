#include "palmier/windows/project_editing_controller.hpp"

#include <QtConcurrentRun>
#include <QFutureWatcher>
#include <QRegularExpression>
#include <QThreadPool>

#include <exception>
#include <utility>

namespace palmier::windows {
namespace {

struct EditResult final {
    std::shared_ptr<const project::ProjectSessionSnapshot> session;
    QString errorCode;
    QString errorMessage;
};

QThreadPool* projectEditPool() {
    static auto* pool = [] {
        auto* value = new QThreadPool;
        value->setMaxThreadCount(1);
        value->setExpiryTimeout(-1);
        return value;
    }();
    return pool;
}

}

ProjectEditingController::ProjectEditingController(
    std::shared_ptr<project::ProjectRuntime> runtime,
    std::shared_ptr<ProjectRuntimeMailbox> runtimeMailbox,
    QObject* parent
) : QObject(parent),
    runtime_(std::move(runtime)),
    runtimeMailbox_(std::move(runtimeMailbox)) {}

ProjectEditingController::~ProjectEditingController() {
    stopSource_.request_stop();
}

bool ProjectEditingController::busy() const noexcept { return busy_; }
bool ProjectEditingController::canUndo() const noexcept { return canUndo_; }
QString ProjectEditingController::errorCode() const { return errorCode_; }
QString ProjectEditingController::errorMessage() const { return errorMessage_; }

void ProjectEditingController::activateProject(std::uint64_t generation) {
    if (shutdownRequested_ || generation == 0) return;
    projectGeneration_ = generation;
    refreshFromMailbox();
}

void ProjectEditingController::observeRuntimePublication(
    const ProjectRuntimePublication& publication
) {
    if (
        shutdownRequested_
        || !publication.session
        || publication.projectGeneration != projectGeneration_
    ) {
        return;
    }
    setCanUndo(publication.session->undoDepth > 0);
}

void ProjectEditingController::splitClip(
    const QString& clipId,
    const QString& frameText
) {
    static const QRegularExpression decimalFrame(QStringLiteral("^[0-9]+$"));
    bool frameIsValid{};
    const auto atFrame = frameText.toLongLong(&frameIsValid, 10);
    if (
        clipId.isEmpty()
        || !decimalFrame.match(frameText).hasMatch()
        || !frameIsValid
        || atFrame < 0
    ) {
        setErrorCode(QStringLiteral("invalidArguments"));
        setErrorMessage(QStringLiteral("Choose a clip and enter a non-negative frame."));
        emit operationFinished(false);
        return;
    }
    start(Operation::split, clipId, atFrame);
}

void ProjectEditingController::undo() {
    if (!canUndo_) {
        setErrorCode(QStringLiteral("nothingToUndo"));
        setErrorMessage(QStringLiteral("There is no edit to undo."));
        emit operationFinished(false);
        return;
    }
    start(Operation::undo);
}

void ProjectEditingController::start(
    Operation operation,
    QString clipId,
    std::int64_t atFrame
) {
    if (shutdownRequested_ || busy_) return;
    if (projectGeneration_ == 0) {
        setErrorCode(QStringLiteral("noActiveProject"));
        setErrorMessage(QStringLiteral("Open a project before editing."));
        emit operationFinished(false);
        return;
    }
    setErrorCode({});
    setErrorMessage({});
    setBusy(true);
    stopSource_ = std::stop_source{};
    const auto cancellation = stopSource_.get_token();
    const auto generation = projectGeneration_;
    const auto runtime = runtime_;
    const auto stableClipId = clipId.toStdString();
    auto* watcher = new QFutureWatcher<EditResult>(this);
    connect(watcher, &QFutureWatcher<EditResult>::finished, this, [this, watcher] {
        auto result = watcher->future().takeResult();
        watcher->deleteLater();
        setBusy(false);
        refreshFromMailbox();
        const bool succeeded = result.session != nullptr;
        if (succeeded) {
            setErrorCode({});
            setErrorMessage({});
        } else {
            setErrorCode(std::move(result.errorCode));
            setErrorMessage(std::move(result.errorMessage));
        }
        emit operationFinished(succeeded);
        if (shutdownRequested_) emit shutdownReady();
    });
    watcher->setFuture(QtConcurrent::run(projectEditPool(), [
        runtime,
        operation,
        stableClipId,
        atFrame,
        generation,
        cancellation
    ] {
        try {
            if (operation == Operation::split) {
                project::SplitClipsCommand command;
                command.splits = std::vector<project::SplitPoint>{{stableClipId, atFrame}};
                auto result = runtime->splitClips(
                    std::move(command),
                    generation,
                    cancellation
                );
                return EditResult{std::move(result.session), {}, {}};
            }
            auto result = runtime->undo(generation, cancellation);
            return EditResult{std::move(result.session), {}, {}};
        } catch (const project::CommandError& error) {
            return EditResult{
                {},
                QString::fromStdString(error.code),
                QString::fromUtf8(error.what()),
            };
        } catch (const project::ProjectRuntimeError& error) {
            return EditResult{
                {},
                QString::fromStdString(error.code),
                QString::fromUtf8(error.what()),
            };
        } catch (const std::exception& error) {
            return EditResult{
                {},
                QStringLiteral("projectEditFailed"),
                QString::fromUtf8(error.what()),
            };
        } catch (...) {
            return EditResult{
                {},
                QStringLiteral("projectEditFailed"),
                QStringLiteral("Project edit failed."),
            };
        }
    }));
}

bool ProjectEditingController::requestShutdown() {
    if (shutdownRequested_) return !busy_;
    shutdownRequested_ = true;
    stopSource_.request_stop();
    return !busy_;
}

void ProjectEditingController::refreshFromMailbox() {
    if (!runtimeMailbox_ || projectGeneration_ == 0) {
        setCanUndo(false);
        return;
    }
    const auto publication = runtimeMailbox_->latest();
    if (
        !publication
        || !publication->session
        || publication->projectGeneration != projectGeneration_
    ) {
        setCanUndo(false);
        return;
    }
    setCanUndo(publication->session->undoDepth > 0);
}

void ProjectEditingController::setBusy(bool value) {
    if (busy_ == value) return;
    busy_ = value;
    emit busyChanged();
}

void ProjectEditingController::setCanUndo(bool value) {
    if (canUndo_ == value) return;
    canUndo_ = value;
    emit canUndoChanged();
}

void ProjectEditingController::setErrorCode(QString value) {
    if (errorCode_ == value) return;
    errorCode_ = std::move(value);
    emit errorCodeChanged();
}

void ProjectEditingController::setErrorMessage(QString value) {
    if (errorMessage_ == value) return;
    errorMessage_ = std::move(value);
    emit errorMessageChanged();
}

}
