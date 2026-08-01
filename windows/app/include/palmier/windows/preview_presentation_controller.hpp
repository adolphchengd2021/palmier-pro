#pragma once

#include "palmier/preview/preview_presentation_session.hpp"
#include "palmier/windows/preview_source.hpp"

#include <QObject>
#include <QString>
#include <QWindow>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>

namespace palmier::windows {
namespace detail {

class QtPreviewSessionPort {
public:
    virtual ~QtPreviewSessionPort() = default;

    virtual preview::PreviewPresentationReceipt resize(
        std::uint32_t width,
        std::uint32_t height,
        std::stop_token cancellation
    ) = 0;
    virtual preview::PreviewPresentationReceipt play(
        const PreviewMediaCandidateProjection& candidate,
        std::stop_token cancellation
    ) = 0;
    virtual preview::PreviewPresentationReceipt tick(
        std::uint64_t expectedGeneration,
        std::stop_token cancellation
    ) = 0;
    virtual preview::PreviewPresentationReceipt cancel(
        std::uint64_t expectedGeneration
    ) = 0;
    virtual preview::PreviewPresentationReceipt close() = 0;
};

using QtPreviewSessionFactory = std::function<std::unique_ptr<QtPreviewSessionPort>(HWND)>;

struct QtPreviewWorkerState;

}

class PreviewNativeWindow final : public QWindow {
    Q_OBJECT

public:
    explicit PreviewNativeWindow(QWindow* parent = nullptr);

signals:
    void nativeSurfaceChanged();
    void nativeSurfaceAboutToBeDestroyed();

protected:
    bool event(QEvent* event) override;
    void exposeEvent(QExposeEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
};

class PreviewPresentationController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QWindow* window READ window CONSTANT)
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)
    Q_PROPERTY(bool shutdownComplete READ shutdownComplete NOTIFY shutdownCompleteChanged)
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString errorCode READ errorCode NOTIFY errorCodeChanged)

public:
    explicit PreviewPresentationController(QObject* parent = nullptr);
    PreviewPresentationController(
        render::D3d11PreviewDriver driver,
        QObject* parent
    );
    PreviewPresentationController(
        detail::QtPreviewSessionFactory factory,
        QObject* parent
    );
    ~PreviewPresentationController() override;

    QWindow* window() noexcept;
    bool ready() const noexcept;
    bool shutdownComplete() const noexcept;
    QString state() const;
    QString errorCode() const;
    preview::PreviewPresentationReceipt latestPlaybackReceipt() const noexcept;
    void replaceProjectPreview(
        std::uint64_t projectGeneration,
        ProjectPreviewProjection preview
    );
    Q_INVOKABLE bool requestShutdown();

signals:
    void readyChanged();
    void shutdownCompleteChanged();
    void stateChanged();
    void errorCodeChanged();
    void shutdownReady();

private:
    enum class OperationKind { attach, resize, play, tick, cancel, close };

    struct PendingResize final {
        std::uint64_t surfaceEpoch{};
        std::uint32_t width{};
        std::uint32_t height{};
    };

    struct PendingPreview final {
        std::uint64_t sourceSerial{};
        std::uint64_t projectGeneration{};
        ProjectPreviewProjection preview;
    };

    struct OperationResult final {
        OperationKind kind{OperationKind::resize};
        std::uint64_t serial{};
        std::uint64_t surfaceEpoch{};
        std::uint64_t sourceSerial{};
        std::uint64_t playbackGeneration{};
        preview::PreviewPresentationReceipt receipt;
        bool sessionExists{};
    };

    void refreshNativeSurface();
    void nativeSurfaceWillBeDestroyed();
    void scheduleAttach(
        std::uint64_t surfaceEpoch,
        HWND window,
        std::uint32_t width,
        std::uint32_t height
    );
    void scheduleResize(PendingResize request);
    void schedulePlay(PendingPreview request);
    void scheduleTick(
        std::uint64_t sourceSerial,
        std::uint64_t playbackGeneration
    );
    void scheduleCancel(
        std::uint64_t sourceSerial,
        std::uint64_t playbackGeneration
    );
    void scheduleClose();
    void scheduleNextTick(
        std::uint64_t sourceSerial,
        std::uint64_t playbackGeneration,
        std::int64_t framesPerSecond
    );
    void servicePendingWork();
    void publishUnavailablePreview(const ProjectPreviewProjection& preview);
    void completeOperation(OperationResult result);
    void completeShutdown(bool notify = true, QString errorCode = {});
    void setReady(bool value);
    void setState(QString value);
    void setErrorCode(QString value);

    detail::QtPreviewSessionFactory factory_;
    std::shared_ptr<detail::QtPreviewWorkerState> workerState_;
    QObject* dispatcher_{};
    std::unique_ptr<PreviewNativeWindow> window_;
    std::shared_ptr<std::stop_source> activeCancellation_;
    std::optional<PendingResize> pendingResize_;
    std::optional<PendingPreview> desiredPreview_;
    HWND nativeWindow_{};
    std::uint64_t surfaceEpoch_{};
    std::uint64_t operationSerial_{};
    std::uint64_t sourceSerial_{};
    std::uint64_t activePreviewSerial_{};
    std::uint64_t suppressedPreviewSerial_{};
    std::uint64_t playbackGeneration_{};
    std::uint32_t requestedWidth_{};
    std::uint32_t requestedHeight_{};
    bool refreshingSurface_{};
    bool operationActive_{};
    std::optional<OperationKind> activeOperationKind_;
    bool tickScheduled_{};
    bool tickPending_{};
    bool sessionExists_{};
    bool shutdownRequested_{};
    bool shutdownComplete_{};
    bool ready_{};
    QString state_{QStringLiteral("empty")};
    QString errorCode_;
    preview::PreviewPresentationReceipt latestPlaybackReceipt_;
};

}
