#pragma once

#include "palmier/preview/preview_presentation_session.hpp"

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
    Q_INVOKABLE bool requestShutdown();

signals:
    void readyChanged();
    void shutdownCompleteChanged();
    void stateChanged();
    void errorCodeChanged();
    void shutdownReady();

private:
    enum class OperationKind { attach, resize, close };

    struct PendingResize final {
        std::uint64_t surfaceEpoch{};
        std::uint32_t width{};
        std::uint32_t height{};
    };

    struct OperationResult final {
        OperationKind kind{OperationKind::resize};
        std::uint64_t serial{};
        std::uint64_t surfaceEpoch{};
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
    void scheduleClose();
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
    HWND nativeWindow_{};
    std::uint64_t surfaceEpoch_{};
    std::uint64_t operationSerial_{};
    std::uint32_t requestedWidth_{};
    std::uint32_t requestedHeight_{};
    bool refreshingSurface_{};
    bool operationActive_{};
    bool sessionExists_{};
    bool shutdownRequested_{};
    bool shutdownComplete_{};
    bool ready_{};
    QString state_{QStringLiteral("empty")};
    QString errorCode_;
};

}
