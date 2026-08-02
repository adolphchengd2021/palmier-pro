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
bool ProjectEditingController::canRedo() const noexcept { return canRedo_; }
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
    setCanRedo(publication.session->redoDepth > 0);
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

void ProjectEditingController::moveClip(
    const QString& clipId,
    const QString& trackText,
    const QString& frameText
) {
    static const QRegularExpression decimalValue(QStringLiteral("^[0-9]+$"));
    const bool hasTrack = !trackText.isEmpty();
    const bool hasFrame = !frameText.isEmpty();
    bool trackIsValid = !hasTrack;
    bool frameIsValid = !hasFrame;
    const auto track = trackText.toULongLong(&trackIsValid, 10);
    const auto frame = frameText.toLongLong(&frameIsValid, 10);
    if (
        clipId.isEmpty()
        || (!hasTrack && !hasFrame)
        || (hasTrack && (!decimalValue.match(trackText).hasMatch() || !trackIsValid))
        || (hasFrame && (!decimalValue.match(frameText).hasMatch() || !frameIsValid))
    ) {
        setErrorCode(QStringLiteral("invalidArguments"));
        setErrorMessage(QStringLiteral(
            "Choose a clip and enter a non-negative track or frame."
        ));
        emit operationFinished(false);
        return;
    }
    start(
        Operation::move,
        clipId,
        0,
        hasTrack ? std::optional<std::size_t>{static_cast<std::size_t>(track)} : std::nullopt,
        hasFrame ? std::optional<std::int64_t>{frame} : std::nullopt
    );
}

void ProjectEditingController::removeClip(const QString& clipId) {
    if (clipId.isEmpty()) {
        setErrorCode(QStringLiteral("invalidArguments"));
        setErrorMessage(QStringLiteral("Choose a clip to remove."));
        emit operationFinished(false);
        return;
    }
    start(Operation::remove, clipId);
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

void ProjectEditingController::redo() {
    if (!canRedo_) {
        setErrorCode(QStringLiteral("nothingToRedo"));
        setErrorMessage(QStringLiteral("There is no edit to redo."));
        emit operationFinished(false);
        return;
    }
    start(Operation::redo);
}

void ProjectEditingController::start(
    Operation operation,
    QString clipId,
    std::int64_t atFrame,
    std::optional<std::size_t> destinationTrack,
    std::optional<std::int64_t> destinationFrame
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
    connect(watcher, &QFutureWatcher<EditResult>::finished, this, [
        this,
        watcher,
        operation,
        clipId
    ] {
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
        if (succeeded && operation == Operation::remove) emit clipRemoved(clipId);
        if (succeeded && (operation == Operation::undo || operation == Operation::redo)) {
            emit historyRestored();
        }
        emit operationFinished(succeeded);
        if (shutdownRequested_) emit shutdownReady();
    });
    watcher->setFuture(QtConcurrent::run(projectEditPool(), [
        runtime,
        operation,
        stableClipId,
        atFrame,
        destinationTrack,
        destinationFrame,
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
            if (operation == Operation::move) {
                project::MoveClipsCommand command;
                command.moves.push_back({
                    stableClipId,
                    destinationTrack,
                    destinationFrame,
                });
                auto result = runtime->moveClips(
                    std::move(command),
                    generation,
                    cancellation
                );
                return EditResult{std::move(result.session), {}, {}};
            }
            if (operation == Operation::remove) {
                project::RemoveClipsCommand command;
                command.clipIds.push_back(stableClipId);
                auto result = runtime->removeClips(
                    std::move(command),
                    generation,
                    cancellation
                );
                return EditResult{std::move(result.session), {}, {}};
            }
            if (operation == Operation::undo) {
                auto result = runtime->undo(generation, cancellation);
                return EditResult{std::move(result.session), {}, {}};
            }
            auto result = runtime->redo(generation, cancellation);
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
        setCanRedo(false);
        return;
    }
    setCanUndo(publication->session->undoDepth > 0);
    setCanRedo(publication->session->redoDepth > 0);
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

void ProjectEditingController::setCanRedo(bool value) {
    if (canRedo_ == value) return;
    canRedo_ = value;
    emit canRedoChanged();
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
