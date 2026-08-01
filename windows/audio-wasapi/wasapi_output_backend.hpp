#pragma once

#include "palmier/audio/wasapi_output.hpp"

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <stop_token>
#include <vector>

namespace palmier::audio {

struct WasapiClockReading final {
    std::uint64_t devicePosition{};
    std::uint64_t qpc100Nanoseconds{};
    bool precisionDegraded{};
};

class WasapiPcmQueue final {
public:
    explicit WasapiPcmQueue(std::uint32_t capacityFrames, PcmFormat format);

    bool enqueue(std::span<const std::byte> bytes);
    void markEndOfStream();
    void clear();

    [[nodiscard]] std::uint32_t availableFrames() const;
    [[nodiscard]] std::uint32_t freeFrames() const;
    [[nodiscard]] bool endOfStream() const;

private:
    friend class WasapiOutputStateMachine;

    void copyFrames(std::byte* destination, std::uint32_t frameCount) const;
    void commitFrames(std::uint32_t frameCount);

    std::vector<std::byte> storage_;
    std::uint32_t capacityFrames_{};
    PcmFormat format_;
    std::uint32_t headFrame_{};
    std::uint32_t sizeFrames_{};
    bool endOfStream_{};
};

class WasapiOutputBackend {
public:
    virtual ~WasapiOutputBackend() = default;

    virtual HRESULT waitForRenderEvent(
        std::stop_token stopToken,
        std::uint32_t timeoutMilliseconds
    ) noexcept = 0;
    virtual HRESULT loadCurrentPadding(std::uint32_t& paddingFrames) noexcept = 0;
    virtual HRESULT acquireBuffer(
        std::uint32_t frameCount,
        std::byte*& data
    ) noexcept = 0;
    virtual HRESULT releaseBuffer(std::uint32_t frameCount, DWORD flags) noexcept = 0;
    virtual HRESULT start() noexcept = 0;
    virtual HRESULT loadClockPosition(WasapiClockReading& reading) noexcept = 0;
    virtual HRESULT stop() noexcept = 0;
    virtual HRESULT reset() noexcept = 0;
    virtual HRESULT close() noexcept = 0;
};

enum class WasapiOutputCheckpoint {
    afterPadding,
    afterAcquire,
    afterCopy,
    afterRelease,
    afterStart,
    afterStop,
    afterReset,
};

class WasapiOutputCheckpoints {
public:
    virtual ~WasapiOutputCheckpoints() = default;
    virtual void arrive(WasapiOutputCheckpoint checkpoint) noexcept = 0;
};

class WasapiOutputStateMachine final {
public:
    WasapiOutputStateMachine(
        WasapiOutputConfig config,
        WasapiOutputBackend& backend,
        WasapiPcmQueue& queue,
        WasapiOutputCheckpoints* checkpoints = nullptr
    );
    ~WasapiOutputStateMachine() noexcept;

    WasapiOutputReceipt start(std::stop_token stopToken = {});
    WasapiOutputReceipt renderOnce(
        std::uint32_t timeoutMilliseconds,
        std::stop_token stopToken = {}
    );
    WasapiOutputReceipt pause(std::stop_token stopToken = {});
    WasapiOutputReceipt reset(std::stop_token stopToken = {});
    WasapiOutputReceipt close() noexcept;

    [[nodiscard]] WasapiOutputState state() const;
    [[nodiscard]] std::uint64_t generation() const;

private:
    WasapiOutputReceipt fillAvailable(
        WasapiOutputOperation operation,
        bool startup,
        std::stop_token stopToken
    );
    WasapiOutputReceipt receipt(WasapiOutputOperation operation) const;
    WasapiOutputReceipt fail(
        WasapiOutputReceipt value,
        WasapiOutputStage stage,
        HRESULT result
    );
    void checkpoint(WasapiOutputCheckpoint value) const noexcept;

    WasapiOutputConfig config_;
    WasapiOutputBackend& backend_;
    WasapiPcmQueue& queue_;
    WasapiOutputCheckpoints* checkpoints_{};
    WasapiOutputState state_{WasapiOutputState::ready};
    std::uint64_t underrunEvents_{};
    bool clientRunning_{};
    bool backendClosed_{};
};

bool isWasapiOutputInvalidation(HRESULT result);

}
