#include "palmier/audio/wasapi_output_worker.hpp"

#include "wasapi_native_stream.hpp"
#include "wasapi_output_worker_testing.hpp"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace {

using palmier::audio::PcmFormat;
using palmier::audio::PcmSampleEncoding;
using palmier::audio::WasapiClockReading;
using palmier::audio::WasapiMixFormat;
using palmier::audio::WasapiOutputOutcome;
using palmier::audio::WasapiOutputStage;
using palmier::audio::WasapiOutputState;
using palmier::audio::WasapiOutputWorker;
using palmier::audio::WasapiOutputWorkerStream;
using palmier::audio::WasapiSharedModePeriods;
using palmier::audio::WasapiWorkerPcmBlock;
using palmier::audio::WasapiWorkerPcmOutcome;
using palmier::audio::WasapiWorkerPcmStage;
using palmier::audio::WasapiWorkerConfigurationOutcome;
using palmier::audio::waitForWasapiRenderEvent;

class HandleOwner final {
public:
    explicit HandleOwner(HANDLE value) : value_(value) {}
    ~HandleOwner() {
        if (value_ != nullptr) {
            CloseHandle(value_);
        }
    }

    [[nodiscard]] HANDLE get() const { return value_; }

    HandleOwner(const HandleOwner&) = delete;
    HandleOwner& operator=(const HandleOwner&) = delete;

private:
    HANDLE value_{};
};

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

PcmFormat pcmFormat() {
    return {48'000, 2, 16, 16, 4, 0x3, PcmSampleEncoding::integer, true};
}

std::vector<std::byte> frames(std::initializer_list<std::uint32_t> values) {
    std::vector<std::byte> bytes(values.size() * sizeof(std::uint32_t));
    std::memcpy(bytes.data(), values.begin(), bytes.size());
    return bytes;
}

struct StreamState final {
    HandleOwner renderEvent{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    HandleOwner enteredWait{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    std::mutex mutex;
    std::vector<std::string> calls;
    std::vector<DWORD> threads;
    std::atomic_bool allowRenderEvents{};
    DWORD constructedThread{};
    DWORD destroyedThread{};
    HRESULT createEnumeratorResult{S_OK};
    std::atomic<HRESULT> paddingResult{S_OK};
};

class ScriptedWorkerStream final : public WasapiOutputWorkerStream {
public:
    explicit ScriptedWorkerStream(std::shared_ptr<StreamState> state)
        : state_(std::move(state)) {
        state_->constructedThread = GetCurrentThreadId();
    }

    ~ScriptedWorkerStream() override {
        state_->destroyedThread = GetCurrentThreadId();
    }

    HRESULT initializeApartment() override { return call("initialize-com"); }
    HRESULT createEnumerator() override {
        call("create-enumerator");
        return state_->createEnumeratorResult;
    }
    HRESULT selectDefaultRenderEndpoint(std::string& endpointId) override {
        endpointId = "scripted-endpoint";
        return call("select-endpoint");
    }
    HRESULT activateAudioClient() override { return call("activate-client"); }
    HRESULT loadMixFormat(WasapiMixFormat& format) override {
        format = pcmFormat();
        return call("mix-format");
    }
    HRESULT setClientProperties() override { return call("client-properties"); }
    HRESULT loadSharedModePeriods(WasapiSharedModePeriods& periods) override {
        periods = {4, 1, 1, 8};
        return call("periods");
    }
    HRESULT initializeSharedAudioStream(std::uint32_t) override {
        return call("initialize-stream");
    }
    HRESULT loadBufferFrames(std::uint32_t& bufferFrames) override {
        bufferFrames = 4;
        return call("buffer-frames");
    }
    HRESULT attachRenderEvent() override { return call("attach-event"); }
    HRESULT loadRenderService() override { return call("render-service"); }
    HRESULT loadClockService() override { return call("clock-service"); }
    HRESULT loadClockFrequency(std::uint64_t& frequency) override {
        frequency = 48'000;
        return call("clock-frequency");
    }

    HRESULT waitForRenderEvent(
        std::stop_token stopToken,
        std::uint32_t timeoutMilliseconds
    ) noexcept override {
        record("wait");
        if (state_->allowRenderEvents.load()) {
            return S_OK;
        }
        return waitForWasapiRenderEvent(
            state_->renderEvent.get(),
            stopToken,
            timeoutMilliseconds,
            state_->enteredWait.get()
        );
    }
    HRESULT loadCurrentPadding(std::uint32_t& paddingFrames) noexcept override {
        paddingFrames = 0;
        call("padding");
        return state_->paddingResult.load();
    }
    HRESULT acquireBuffer(
        std::uint32_t frameCount,
        std::byte*& data
    ) noexcept override {
        record("acquire");
        acquired_.resize(static_cast<std::size_t>(frameCount) * pcmFormat().blockAlign);
        data = acquired_.data();
        return S_OK;
    }
    HRESULT releaseBuffer(std::uint32_t, DWORD) noexcept override {
        return call("release");
    }
    HRESULT start() noexcept override { return call("start"); }
    HRESULT loadClockPosition(WasapiClockReading& reading) noexcept override {
        reading = {240, 12'345, false};
        return call("clock");
    }
    HRESULT stop() noexcept override { return call("stop"); }
    HRESULT reset() noexcept override { return call("reset"); }
    HRESULT close() noexcept override { return call("close"); }

private:
    HRESULT call(const std::string& value) noexcept {
        record(value);
        return S_OK;
    }

    void record(const std::string& value) noexcept {
        std::lock_guard lock(state_->mutex);
        state_->calls.push_back(value);
        state_->threads.push_back(GetCurrentThreadId());
    }

    std::shared_ptr<StreamState> state_;
    std::vector<std::byte> acquired_;
};

WasapiWorkerPcmBlock block(
    std::uint64_t generation,
    std::uint64_t startOutputSample,
    std::initializer_list<std::uint32_t> values
) {
    return {
        generation,
        startOutputSample,
        static_cast<std::uint32_t>(values.size()),
        pcmFormat(),
        frames(values),
    };
}

void controlPreemptsRenderWaitAndKeepsNativeOwnership() {
    auto state = std::make_shared<StreamState>();
    require(state->renderEvent.get() != nullptr, "render event creation failed");
    require(state->enteredWait.get() != nullptr, "wait event creation failed");
    {
        WasapiOutputWorker worker([state] {
            return std::make_unique<ScriptedWorkerStream>(state);
        });
        const auto configuration = worker.configuration();
        require(
            configuration.outcome == WasapiWorkerConfigurationOutcome::available,
            "worker configuration was unavailable"
        );
        require(configuration.pcmFormat == pcmFormat(), "worker format changed");
        require(configuration.bufferFrames == 4, "worker buffer size changed");
        require(configuration.clockFrequency == 48'000, "worker clock changed");
        const auto started = worker.start(1);
        require(started.outcome == WasapiOutputOutcome::changed, "worker start failed");
        require(started.currentState == WasapiOutputState::running, "worker not running");
        require(
            WaitForSingleObject(state->enteredWait.get(), 1'000) == WAIT_OBJECT_0,
            "worker did not enter its render wait"
        );

        const auto accepted = worker.submit(block(1, 100, {1, 2}));
        require(
            accepted.outcome == WasapiWorkerPcmOutcome::accepted,
            "worker rejected contiguous PCM"
        );
        require(accepted.nextOutputSample == 102, "worker sample cursor changed");
        const auto discontinuous = worker.submit(block(1, 103, {3}));
        require(
            discontinuous.outcome == WasapiWorkerPcmOutcome::refused,
            "worker accepted discontinuous PCM"
        );
        require(
            discontinuous.stage == WasapiWorkerPcmStage::sampleInvariant,
            "worker lost the sample refusal reason"
        );

        const auto stale = worker.pause(2);
        require(stale.outcome == WasapiOutputOutcome::refused, "stale control changed state");
        require(stale.stage == WasapiOutputStage::generationInvariant, "stale reason lost");
        const auto installed = worker.installGeneration(1, 42);
        require(installed.outcome == WasapiOutputOutcome::changed, "install failed");
        require(installed.generation == 42, "worker invented a generation");
        const auto oldTerminal = worker.waitForTerminal(1);
        require(
            oldTerminal.outcome == WasapiOutputOutcome::refused,
            "replaced generation waiter did not terminate"
        );
        require(
            oldTerminal.stage == WasapiOutputStage::generationInvariant,
            "replaced generation waiter lost its reason"
        );
        require(worker.close().outcome == WasapiOutputOutcome::changed, "close failed");
    }

    require(state->constructedThread != 0, "stream construction was not observed");
    require(
        state->destroyedThread == state->constructedThread,
        "stream destruction crossed the device thread"
    );
    std::lock_guard lock(state->mutex);
    require(!state->threads.empty(), "stream calls were not observed");
    for (const DWORD thread : state->threads) {
        require(thread == state->constructedThread, "native call crossed the device thread");
    }
    const auto stop = std::find(state->calls.begin(), state->calls.end(), "stop");
    const auto reset = std::find(state->calls.begin(), state->calls.end(), "reset");
    require(stop != state->calls.end(), "generation install skipped Stop");
    require(reset != state->calls.end() && stop < reset, "generation Reset order changed");
}

void orderedPcmAndEndOfStreamReachOneTerminalReceipt() {
    auto state = std::make_shared<StreamState>();
    state->allowRenderEvents.store(true);
    WasapiOutputWorker worker([state] {
        return std::make_unique<ScriptedWorkerStream>(state);
    });
    const auto accepted = worker.submit(block(1, 500, {10, 20}));
    require(accepted.outcome == WasapiWorkerPcmOutcome::accepted, "PCM was rejected");
    const auto ended = worker.markEndOfStream(1, 502);
    require(ended.outcome == WasapiWorkerPcmOutcome::accepted, "EOS was rejected");
    require(ended.endOfStream, "EOS receipt lost its terminal marker");
    require(
        worker.markEndOfStream(1, 502).outcome
            == WasapiWorkerPcmOutcome::noOp,
        "repeated EOS was not a no-op"
    );

    const auto started = worker.start(1);
    require(started.outcome == WasapiOutputOutcome::changed, "EOS start failed");
    const auto terminal = worker.waitForTerminal(1);
    require(terminal.currentState == WasapiOutputState::completed, "EOS did not complete");
    require(terminal.generation == 1, "terminal generation changed");
    require(
        worker.submit(block(1, 502, {30})).outcome
            == WasapiWorkerPcmOutcome::refused,
        "completed generation accepted more PCM"
    );
    require(worker.close().outcome == WasapiOutputOutcome::changed, "close failed");
    require(
        worker.waitForTerminal(1).currentState == WasapiOutputState::completed,
        "close overwrote the generation terminal receipt"
    );
}

void fullReadyQueueReturnsRetryWithoutSpinning() {
    auto state = std::make_shared<StreamState>();
    WasapiOutputWorker worker([state] {
        return std::make_unique<ScriptedWorkerStream>(state);
    });
    require(
        worker.submit(block(1, 0, {1, 2, 3, 4})).outcome
            == WasapiWorkerPcmOutcome::accepted,
        "ready prebuffer was rejected"
    );
    const auto retry = worker.submit(block(1, 4, {5}));
    require(retry.outcome == WasapiWorkerPcmOutcome::failed, "full ready queue blocked");
    require(
        retry.stage == WasapiWorkerPcmStage::capacityInvariant,
        "full ready queue lost its retry reason"
    );
    require(
        retry.hresult == HRESULT_FROM_WIN32(ERROR_RETRY),
        "full ready queue changed its retry HRESULT"
    );
    require(worker.close().outcome == WasapiOutputOutcome::changed, "close failed");
}

void completedDiscardDoesNotReuseTheOldTerminal() {
    auto state = std::make_shared<StreamState>();
    state->allowRenderEvents.store(true);
    WasapiOutputWorker worker([state] {
        return std::make_unique<ScriptedWorkerStream>(state);
    });
    require(
        worker.submit(block(1, 0, {1})).outcome
            == WasapiWorkerPcmOutcome::accepted,
        "first PCM was rejected"
    );
    require(
        worker.markEndOfStream(1, 1).outcome
            == WasapiWorkerPcmOutcome::accepted,
        "first EOS was rejected"
    );
    require(worker.start(1).outcome == WasapiOutputOutcome::changed, "start failed");
    require(
        worker.waitForTerminal(1).currentState == WasapiOutputState::completed,
        "first generation did not complete"
    );
    require(
        worker.discardGeneration(1).outcome == WasapiOutputOutcome::changed,
        "completed discard failed"
    );

    state->allowRenderEvents.store(false);
    require(
        worker.submit(block(1, 10, {2})).outcome
            == WasapiWorkerPcmOutcome::accepted,
        "second PCM was rejected"
    );
    require(
        worker.markEndOfStream(1, 11).outcome
            == WasapiWorkerPcmOutcome::accepted,
        "second EOS was rejected"
    );
    require(worker.start(1).outcome == WasapiOutputOutcome::changed, "restart failed");
    std::stop_source source;
    source.request_stop();
    require(
        worker.waitForTerminal(1, source.get_token()).outcome
            == WasapiOutputOutcome::cancelled,
        "discard reused the old terminal receipt"
    );
    require(worker.close().outcome == WasapiOutputOutcome::changed, "close failed");
}

void generationBarrierRejectsPendingHandoffBeforeAcknowledgement() {
    auto state = std::make_shared<StreamState>();
    WasapiOutputWorker worker([state] {
        return std::make_unique<ScriptedWorkerStream>(state);
    });
    require(worker.start(1).outcome == WasapiOutputOutcome::changed, "start failed");
    require(
        WaitForSingleObject(state->enteredWait.get(), 1'000) == WAIT_OBJECT_0,
        "worker did not enter its first wait"
    );
    require(ResetEvent(state->enteredWait.get()) != FALSE, "wait reset failed");
    require(
        worker.submit(block(1, 0, {1, 2, 3, 4})).outcome
            == WasapiWorkerPcmOutcome::accepted,
        "full queue block was rejected"
    );
    require(
        WaitForSingleObject(state->enteredWait.get(), 1'000) == WAIT_OBJECT_0,
        "worker did not resume its render wait"
    );
    require(ResetEvent(state->enteredWait.get()) != FALSE, "second wait reset failed");

    std::optional<palmier::audio::WasapiWorkerPcmReceipt> pendingReceipt;
    std::jthread pending([&worker, &pendingReceipt] {
        pendingReceipt = worker.submit(block(1, 4, {5}));
    });
    require(
        WaitForSingleObject(state->enteredWait.get(), 1'000) == WAIT_OBJECT_0,
        "full handoff did not return to the render wait"
    );
    const auto installed = worker.installGeneration(1, 42);
    require(installed.outcome == WasapiOutputOutcome::changed, "barrier install failed");
    pending.join();
    require(pendingReceipt.has_value(), "pending handoff lost its receipt");
    require(
        pendingReceipt->outcome == WasapiWorkerPcmOutcome::refused,
        "barrier acknowledged before rejecting the old handoff"
    );
    require(
        pendingReceipt->stage == WasapiWorkerPcmStage::generationInvariant,
        "barrier lost the old handoff reason"
    );
    require(worker.close().outcome == WasapiOutputOutcome::changed, "close failed");
}

void cancellationRemovesAnUnadmittedHandoff() {
    auto state = std::make_shared<StreamState>();
    WasapiOutputWorker worker([state] {
        return std::make_unique<ScriptedWorkerStream>(state);
    });
    require(worker.start(1).outcome == WasapiOutputOutcome::changed, "start failed");
    require(
        WaitForSingleObject(state->enteredWait.get(), 1'000) == WAIT_OBJECT_0,
        "worker did not enter its first wait"
    );
    require(ResetEvent(state->enteredWait.get()) != FALSE, "wait reset failed");
    require(
        worker.submit(block(1, 0, {1, 2, 3, 4})).outcome
            == WasapiWorkerPcmOutcome::accepted,
        "full queue block was rejected"
    );
    require(
        WaitForSingleObject(state->enteredWait.get(), 1'000) == WAIT_OBJECT_0,
        "worker did not resume its render wait"
    );
    require(ResetEvent(state->enteredWait.get()) != FALSE, "second wait reset failed");

    std::stop_source source;
    std::optional<palmier::audio::WasapiWorkerPcmReceipt> pendingReceipt;
    std::jthread pending([&worker, &pendingReceipt, &source] {
        pendingReceipt = worker.submit(block(1, 4, {5}), source.get_token());
    });
    require(
        WaitForSingleObject(state->enteredWait.get(), 1'000) == WAIT_OBJECT_0,
        "pending handoff did not reach bounded backpressure"
    );
    source.request_stop();
    pending.join();
    require(pendingReceipt.has_value(), "cancelled handoff lost its receipt");
    require(
        pendingReceipt->outcome == WasapiWorkerPcmOutcome::cancelled,
        "unadmitted handoff committed after cancellation"
    );
    require(
        worker.installGeneration(1, 42).outcome == WasapiOutputOutcome::changed,
        "cancelled handoff blocked the generation barrier"
    );
    require(worker.close().outcome == WasapiOutputOutcome::changed, "close failed");
}

void setupFailureReturnsReceiptsWithoutStartingNativeOutput() {
    auto state = std::make_shared<StreamState>();
    state->createEnumeratorResult = E_ACCESSDENIED;
    WasapiOutputWorker worker([state] {
        return std::make_unique<ScriptedWorkerStream>(state);
    });
    const auto started = worker.start(1);
    require(started.outcome == WasapiOutputOutcome::failed, "setup failure was hidden");
    require(started.hresult == E_ACCESSDENIED, "setup HRESULT changed");
    const auto configuration = worker.configuration();
    require(
        configuration.outcome == WasapiWorkerConfigurationOutcome::failed,
        "setup configuration failure was hidden"
    );
    require(configuration.hresult == E_ACCESSDENIED, "setup configuration HRESULT changed");
    const auto terminal = worker.waitForTerminal(1);
    require(terminal.outcome == WasapiOutputOutcome::failed, "terminal setup state hung");
    require(terminal.hresult == E_ACCESSDENIED, "terminal setup HRESULT changed");
    const auto pcm = worker.submit(block(1, 0, {1}));
    require(pcm.outcome == WasapiWorkerPcmOutcome::failed, "failed worker accepted PCM");
    require(worker.close().outcome == WasapiOutputOutcome::changed, "failed worker hung on close");
    std::lock_guard lock(state->mutex);
    require(
        std::count(state->calls.begin(), state->calls.end(), "close") == 1,
        "setup failure did not explicitly close its partial stream"
    );
}

void terminalInvalidationMakesConfigurationUnavailable() {
    auto state = std::make_shared<StreamState>();
    state->paddingResult.store(AUDCLNT_E_DEVICE_INVALIDATED);
    WasapiOutputWorker worker([state] {
        return std::make_unique<ScriptedWorkerStream>(state);
    });
    const auto started = worker.start(1);
    require(started.outcome == WasapiOutputOutcome::invalidated, "invalidation was hidden");
    const auto configuration = worker.configuration();
    require(
        configuration.outcome == WasapiWorkerConfigurationOutcome::unavailable,
        "invalidated configuration remained available"
    );
    require(
        configuration.hresult == AUDCLNT_E_DEVICE_INVALIDATED,
        "configuration lost invalidation HRESULT"
    );
    require(worker.close().outcome == WasapiOutputOutcome::changed, "close failed");
}

void concurrentCloseJoinsTheDeviceThreadExactlyOnce() {
    auto state = std::make_shared<StreamState>();
    WasapiOutputWorker worker([state] {
        return std::make_unique<ScriptedWorkerStream>(state);
    });
    require(worker.start(1).outcome == WasapiOutputOutcome::changed, "start failed");
    require(
        WaitForSingleObject(state->enteredWait.get(), 1'000) == WAIT_OBJECT_0,
        "worker did not enter its render wait"
    );
    std::barrier ready(3);
    std::optional<palmier::audio::WasapiOutputReceipt> first;
    std::optional<palmier::audio::WasapiOutputReceipt> second;
    std::jthread firstClose([&] {
        ready.arrive_and_wait();
        first = worker.close();
    });
    std::jthread secondClose([&] {
        ready.arrive_and_wait();
        second = worker.close();
    });
    ready.arrive_and_wait();
    firstClose.join();
    secondClose.join();
    require(first.has_value() && second.has_value(), "concurrent close lost a receipt");
    require(
        first->outcome == WasapiOutputOutcome::changed
            && second->outcome == WasapiOutputOutcome::changed,
        "concurrent close returned inconsistent outcomes"
    );
    std::lock_guard lock(state->mutex);
    require(
        std::count(state->calls.begin(), state->calls.end(), "close") == 1,
        "concurrent close repeated native teardown"
    );
}

}

int main() {
    try {
        controlPreemptsRenderWaitAndKeepsNativeOwnership();
        orderedPcmAndEndOfStreamReachOneTerminalReceipt();
        fullReadyQueueReturnsRetryWithoutSpinning();
        completedDiscardDoesNotReuseTheOldTerminal();
        generationBarrierRejectsPendingHandoffBeforeAcknowledgement();
        cancellationRemovesAnUnadmittedHandoff();
        setupFailureReturnsReceiptsWithoutStartingNativeOutput();
        terminalInvalidationMakesConfigurationUnavailable();
        concurrentCloseJoinsTheDeviceThreadExactlyOnce();
        std::cout << "WASAPI output worker tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
