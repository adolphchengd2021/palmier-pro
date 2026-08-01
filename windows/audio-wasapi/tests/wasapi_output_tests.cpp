#include "palmier/audio/wasapi_output.hpp"

#include "wasapi_output_backend.hpp"
#include "wasapi_native_stream.hpp"

#include <audioclient.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using palmier::audio::WasapiClockReading;
using palmier::audio::WasapiOutputBackend;
using palmier::audio::WasapiOutputCheckpoint;
using palmier::audio::WasapiOutputCheckpoints;
using palmier::audio::WasapiOutputConfig;
using palmier::audio::WasapiOutputOutcome;
using palmier::audio::WasapiOutputStage;
using palmier::audio::WasapiOutputState;
using palmier::audio::WasapiOutputStateMachine;
using palmier::audio::WasapiPcmQueue;
using palmier::audio::waitForWasapiRenderEvent;
using palmier::audio::PcmFormat;
using palmier::audio::PcmSampleEncoding;

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

class ScriptedBackend final : public WasapiOutputBackend {
public:
    std::vector<std::string> calls;
    std::vector<std::byte> buffer = std::vector<std::byte>(64);
    std::uint32_t padding{};
    std::uint32_t acquiredFrames{};
    std::uint32_t releasedFrames{};
    DWORD releasedFlags{};
    DWORD acquireThread{};
    DWORD releaseThread{};
    HRESULT waitResult{S_OK};
    HRESULT paddingResult{S_OK};
    HRESULT acquireResult{S_OK};
    HRESULT releaseResult{S_OK};
    HRESULT startResult{S_OK};
    HRESULT clockResult{S_OK};
    HRESULT stopResult{S_OK};
    HRESULT resetResult{S_OK};
    HRESULT closeResult{S_OK};
    WasapiClockReading clock{240, 12'345, false};

    HRESULT waitForRenderEvent(
        std::stop_token,
        std::uint32_t timeoutMilliseconds
    ) noexcept override {
        calls.emplace_back("wait:" + std::to_string(timeoutMilliseconds));
        return waitResult;
    }

    HRESULT loadCurrentPadding(std::uint32_t& paddingFrames) noexcept override {
        calls.emplace_back("padding");
        if (SUCCEEDED(paddingResult)) {
            paddingFrames = padding;
        }
        return paddingResult;
    }

    HRESULT acquireBuffer(
        std::uint32_t frameCount,
        std::byte*& data
    ) noexcept override {
        calls.emplace_back("acquire:" + std::to_string(frameCount));
        acquiredFrames = frameCount;
        acquireThread = GetCurrentThreadId();
        data = SUCCEEDED(acquireResult) ? buffer.data() : nullptr;
        return acquireResult;
    }

    HRESULT releaseBuffer(std::uint32_t frameCount, DWORD flags) noexcept override {
        calls.emplace_back("release:" + std::to_string(frameCount));
        releasedFrames = frameCount;
        releasedFlags = flags;
        releaseThread = GetCurrentThreadId();
        return releaseResult;
    }

    HRESULT start() noexcept override {
        calls.emplace_back("start");
        return startResult;
    }

    HRESULT loadClockPosition(WasapiClockReading& reading) noexcept override {
        calls.emplace_back("clock");
        if (SUCCEEDED(clockResult)) {
            reading = clock;
        }
        return clockResult;
    }

    HRESULT stop() noexcept override {
        calls.emplace_back("stop");
        return stopResult;
    }

    HRESULT reset() noexcept override {
        calls.emplace_back("reset");
        return resetResult;
    }

    HRESULT close() noexcept override {
        calls.emplace_back("close");
        return closeResult;
    }
};

class CancellingCheckpoint final : public WasapiOutputCheckpoints {
public:
    CancellingCheckpoint(
        WasapiOutputCheckpoint target,
        std::stop_source& source
    ) : target_(target), source_(source) {}

    void arrive(WasapiOutputCheckpoint checkpoint) noexcept override {
        if (checkpoint == target_) {
            source_.request_stop();
        }
    }

private:
    WasapiOutputCheckpoint target_;
    std::stop_source& source_;
};

PcmFormat pcmFormat() {
    return {48'000, 2, 16, 16, 4, 0x3, PcmSampleEncoding::integer, true};
}

WasapiOutputConfig config() {
    return {4, pcmFormat(), 48'000, 7};
}

std::vector<std::byte> frames(std::initializer_list<std::uint32_t> values) {
    std::vector<std::byte> bytes(values.size() * sizeof(std::uint32_t));
    std::memcpy(bytes.data(), values.begin(), bytes.size());
    return bytes;
}

void primesBeforeStartAndCommitsOnlyReleasedPcm() {
    ScriptedBackend backend;
    WasapiPcmQueue queue(8, pcmFormat());
    const auto input = frames({0x01020304, 0x11121314});
    require(queue.enqueue(input), "PCM enqueue failed");
    WasapiOutputStateMachine machine(config(), backend, queue);

    const auto result = machine.start();
    require(result.outcome == WasapiOutputOutcome::changed, "start did not change");
    require(result.previousState == WasapiOutputState::ready, "wrong previous state");
    require(result.currentState == WasapiOutputState::running, "stream not running");
    require(result.mediaFrames == 2 && result.silenceFrames == 2, "prime counts wrong");
    require(result.releasedFrames == 4, "prime was not fully released");
    require(queue.availableFrames() == 0, "released PCM was not committed");
    require(
        backend.calls == std::vector<std::string>{
            "padding", "acquire:4", "release:4", "start", "clock",
        },
        "prime/start order changed"
    );
    require(
        backend.acquireThread == backend.releaseThread,
        "buffer lease crossed threads"
    );
    require(
        std::equal(input.begin(), input.end(), backend.buffer.begin()),
        "PCM bytes were not copied"
    );
    require(
        std::all_of(
            backend.buffer.begin() + static_cast<std::ptrdiff_t>(input.size()),
            backend.buffer.begin() + 16,
            [](std::byte value) { return value == std::byte{0}; }
        ),
        "partial underrun tail was not zeroed"
    );
}

void rendersOnlyAfterAnEventAndCountsUnderrun() {
    ScriptedBackend backend;
    WasapiPcmQueue queue(8, pcmFormat());
    WasapiOutputStateMachine machine(config(), backend, queue);
    require(machine.start().outcome == WasapiOutputOutcome::changed, "start failed");
    backend.calls.clear();
    backend.padding = 2;
    const auto input = frames({0x01020304});
    require(queue.enqueue(input), "render PCM enqueue failed");

    const auto result = machine.renderOnce(125);
    require(result.outcome == WasapiOutputOutcome::changed, "render did not change");
    require(result.availableFrames == 2, "available frame math changed");
    require(result.mediaFrames == 1 && result.silenceFrames == 1, "render fill wrong");
    require(result.underrunEventsDelta == 1, "underrun was not counted");
    require(result.underrunEventsTotal == 1, "underrun total wrong");
    require(backend.calls.front() == "wait:125", "render did not wait first");
    require(backend.calls.back() == "clock", "clock was not sampled after render");
}

void endOfStreamWithoutMediaCompletesBeforeStart() {
    ScriptedBackend backend;
    WasapiPcmQueue queue(8, pcmFormat());
    queue.markEndOfStream();
    WasapiOutputStateMachine machine(config(), backend, queue);
    const auto result = machine.start();
    require(result.outcome == WasapiOutputOutcome::changed, "EOS start failed");
    require(
        machine.state() == WasapiOutputState::completed,
        "empty EOS did not complete"
    );
    require(
        backend.calls == std::vector<std::string>{"padding"},
        "empty EOS acquired or started the endpoint"
    );
}

void endOfStreamReleasesTheExactTailThenCompletes() {
    ScriptedBackend backend;
    WasapiPcmQueue queue(8, pcmFormat());
    const auto input = frames({1, 2, 3});
    require(queue.enqueue(input), "EOS tail enqueue failed");
    queue.markEndOfStream();
    WasapiOutputStateMachine machine(config(), backend, queue);

    const auto started = machine.start();
    require(started.requestedFrames == 3, "EOS tail requested a full packet");
    require(started.mediaFrames == 3 && started.silenceFrames == 0, "EOS tail changed");
    require(started.releasedFrames == 3, "EOS tail release size changed");
    require(backend.acquiredFrames == 3 && backend.releasedFrames == 3, "EOS lease mismatched");
    require(machine.state() == WasapiOutputState::running, "tail did not start output");

    backend.padding = 3;
    const auto draining = machine.renderOnce(125);
    require(draining.outcome == WasapiOutputOutcome::noOp, "padding drain changed state");
    require(machine.state() == WasapiOutputState::running, "padding drain stopped early");

    backend.padding = 0;
    const auto completed = machine.renderOnce(125);
    require(completed.outcome == WasapiOutputOutcome::changed, "EOS did not complete");
    require(completed.currentState == WasapiOutputState::completed, "wrong EOS state");
    require(backend.calls.back() == "stop", "EOS completion did not stop client");
    const auto reset = machine.installGeneration(7, 42);
    require(reset.outcome == WasapiOutputOutcome::changed, "completed reset failed");
    require(reset.generation == 42, "completed reset generation changed");
    require(machine.state() == WasapiOutputState::ready, "completed reset state changed");
}

void cancellationInsideLeaseAbandonsAndRollsBack() {
    for (const auto target : {
        WasapiOutputCheckpoint::afterAcquire,
        WasapiOutputCheckpoint::afterCopy,
    }) {
        ScriptedBackend backend;
        WasapiPcmQueue queue(8, pcmFormat());
        const auto input = frames({1, 2, 3, 4});
        require(queue.enqueue(input), "cancel PCM enqueue failed");
        std::stop_source source;
        CancellingCheckpoint checkpoint(target, source);
        WasapiOutputStateMachine machine(config(), backend, queue, &checkpoint);

        const auto result = machine.start(source.get_token());
        require(result.outcome == WasapiOutputOutcome::cancelled, "lease cancel not reported");
        require(
            std::count(backend.calls.begin(), backend.calls.end(), "release:0") == 1,
            "cancelled lease did not release zero frames exactly once"
        );
        require(
            backend.acquireThread == backend.releaseThread,
            "cancelled lease crossed threads"
        );
        require(queue.availableFrames() == 4, "cancelled PCM was consumed");
        require(
            std::find(backend.calls.begin(), backend.calls.end(), "start")
                == backend.calls.end(),
            "client started after cancellation"
        );
    }
}

void cancellationBeforeWorkMakesNoBackendCall() {
    ScriptedBackend backend;
    WasapiPcmQueue queue(8, pcmFormat());
    WasapiOutputStateMachine machine(config(), backend, queue);
    std::stop_source source;
    source.request_stop();

    const auto result = machine.start(source.get_token());
    require(result.outcome == WasapiOutputOutcome::cancelled, "pre-cancel not reported");
    require(backend.calls.empty(), "pre-cancel reached backend");
    require(machine.state() == WasapiOutputState::ready, "pre-cancel changed state");
}

void cancellationInterruptsTheNativeEventWait() {
    HandleOwner renderEvent(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    HandleOwner registeredEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    require(renderEvent.get() != nullptr, "render event creation failed");
    require(registeredEvent.get() != nullptr, "registration event creation failed");
    std::stop_source source;
    HRESULT waitResult = E_UNEXPECTED;
    {
        std::jthread worker([&] {
            waitResult = waitForWasapiRenderEvent(
                renderEvent.get(),
                source.get_token(),
                1'000,
                registeredEvent.get()
            );
        });
        require(
            WaitForSingleObject(registeredEvent.get(), 1'000) == WAIT_OBJECT_0,
            "native wait did not register cancellation"
        );
        source.request_stop();
    }
    require(
        waitResult == HRESULT_FROM_WIN32(ERROR_CANCELLED),
        "native wait was not interrupted by cancellation"
    );
}

void cancellationAfterReleaseReportsCommittedEffect() {
    ScriptedBackend backend;
    WasapiPcmQueue queue(8, pcmFormat());
    const auto input = frames({1, 2, 3, 4});
    require(queue.enqueue(input), "late cancel PCM enqueue failed");
    std::stop_source source;
    CancellingCheckpoint checkpoint(WasapiOutputCheckpoint::afterRelease, source);
    WasapiOutputStateMachine machine(config(), backend, queue, &checkpoint);

    const auto result = machine.start(source.get_token());
    require(result.outcome == WasapiOutputOutcome::changed, "committed prime was hidden");
    require(result.lateCancellation, "late cancellation was not reported");
    require(queue.availableFrames() == 0, "released PCM was not committed");
    require(machine.state() == WasapiOutputState::primed, "cancelled start was not stopped");
    require(
        std::find(backend.calls.begin(), backend.calls.end(), "start")
            == backend.calls.end(),
        "client started after committed cancellation"
    );
}

void cancellationAfterNativeStartKeepsRunningState() {
    ScriptedBackend backend;
    WasapiPcmQueue queue(8, pcmFormat());
    std::stop_source source;
    CancellingCheckpoint checkpoint(WasapiOutputCheckpoint::afterStart, source);
    WasapiOutputStateMachine machine(config(), backend, queue, &checkpoint);

    const auto result = machine.start(source.get_token());
    require(result.outcome == WasapiOutputOutcome::changed, "native start was hidden");
    require(result.lateCancellation, "post-start cancellation was not reported");
    require(result.currentState == WasapiOutputState::running, "running state was lost");
    require(!result.hasClockSample, "clock was read after late cancellation");
    require(machine.state() == WasapiOutputState::running, "native state drifted");
}

void validatesPaddingBeforeSubtraction() {
    ScriptedBackend backend;
    backend.padding = 5;
    WasapiPcmQueue queue(8, pcmFormat());
    WasapiOutputStateMachine machine(config(), backend, queue);

    const auto result = machine.start();
    require(result.outcome == WasapiOutputOutcome::failed, "bad padding accepted");
    require(result.stage == WasapiOutputStage::paddingInvariant, "wrong failure stage");
    require(machine.state() == WasapiOutputState::failed, "invariant did not fail state");
    require(
        backend.calls == std::vector<std::string>{"padding", "close"},
        "bad padding reached a lease or skipped teardown"
    );
}

void invalidationRejectsTheOldGeneration() {
    ScriptedBackend backend;
    WasapiPcmQueue queue(8, pcmFormat());
    WasapiOutputStateMachine machine(config(), backend, queue);
    require(machine.start().outcome == WasapiOutputOutcome::changed, "start failed");
    require(queue.enqueue(frames({1, 2})), "stale PCM enqueue failed");
    backend.calls.clear();
    backend.paddingResult = AUDCLNT_E_DEVICE_INVALIDATED;

    const auto result = machine.renderOnce(125);
    require(result.outcome == WasapiOutputOutcome::invalidated, "loss not invalidated");
    require(result.stage == WasapiOutputStage::currentPadding, "loss stage changed");
    require(result.generation == 7, "invalidation changed generation");
    require(machine.state() == WasapiOutputState::invalidated, "state not invalidated");
    require(queue.availableFrames() == 0, "invalidation retained application PCM");
    require(
        backend.calls == std::vector<std::string>{"wait:125", "padding", "stop", "close"},
        "invalidation teardown order changed"
    );
    const auto terminalCalls = backend.calls.size();
    require(machine.close().outcome == WasapiOutputOutcome::changed, "terminal close failed");
    require(backend.calls.size() == terminalCalls, "invalidation repeated teardown");
}

void pauseResumeResetAndRepeatedCommandsAreExact() {
    ScriptedBackend backend;
    WasapiPcmQueue queue(8, pcmFormat());
    WasapiOutputStateMachine machine(config(), backend, queue);
    const auto readyCalls = backend.calls.size();
    require(
        machine.installGeneration(7, 42).outcome
            == WasapiOutputOutcome::changed,
        "ready generation install failed"
    );
    require(backend.calls.size() == readyCalls, "ready install touched backend");
    require(machine.start().outcome == WasapiOutputOutcome::changed, "start failed");
    const auto runningCalls = backend.calls.size();
    const auto repeatedStart = machine.start();
    require(repeatedStart.outcome == WasapiOutputOutcome::noOp, "repeat start changed");
    require(backend.calls.size() == runningCalls, "repeat start touched backend");
    require(machine.pause().outcome == WasapiOutputOutcome::changed, "pause failed");
    const auto stoppedCalls = backend.calls.size();
    require(machine.pause().outcome == WasapiOutputOutcome::noOp, "repeat pause changed");
    require(backend.calls.size() == stoppedCalls, "repeat pause touched backend");
    require(machine.start().outcome == WasapiOutputOutcome::changed, "resume failed");
    require(machine.pause().outcome == WasapiOutputOutcome::changed, "second pause failed");
    const auto reset = machine.installGeneration(42, 100);
    require(reset.outcome == WasapiOutputOutcome::changed, "reset failed");
    require(reset.generation == 100, "reset generation wrong");
    require(machine.state() == WasapiOutputState::ready, "reset did not become ready");
}

void exactGenerationDiscardAndInstallAreOrdered() {
    ScriptedBackend backend;
    WasapiPcmQueue queue(8, pcmFormat());
    require(queue.enqueue(frames({1, 2, 3, 4})), "generation PCM enqueue failed");
    WasapiOutputStateMachine machine(config(), backend, queue);
    require(machine.start().outcome == WasapiOutputOutcome::changed, "start failed");
    backend.calls.clear();
    require(queue.enqueue(frames({9, 10})), "queued old-generation PCM failed");

    const auto refused = machine.installGeneration(8, 42);
    require(refused.outcome == WasapiOutputOutcome::refused, "stale owner installed");
    require(
        refused.stage == WasapiOutputStage::generationInvariant,
        "stale owner lost its reason"
    );
    require(backend.calls.empty(), "stale generation touched backend");

    const auto installed = machine.installGeneration(7, 42);
    require(installed.outcome == WasapiOutputOutcome::changed, "install failed");
    require(installed.generation == 42, "exact generation was not installed");
    require(
        backend.calls == std::vector<std::string>{"stop", "reset"},
        "install did not flush the native engine before acknowledgement"
    );
    require(queue.availableFrames() == 0, "install retained application PCM");

    require(machine.start().outcome == WasapiOutputOutcome::changed, "restart failed");
    backend.calls.clear();
    require(queue.enqueue(frames({5, 6})), "discard PCM enqueue failed");
    const auto discarded = machine.discardGeneration(42);
    require(discarded.outcome == WasapiOutputOutcome::changed, "discard failed");
    require(discarded.generation == 42, "discard advanced generation");
    require(queue.availableFrames() == 0, "discard retained PCM");
    require(
        backend.calls == std::vector<std::string>{"stop", "reset"},
        "discard did not flush the native engine"
    );
    backend.calls.clear();
    require(
        machine.discardGeneration(42).outcome == WasapiOutputOutcome::noOp,
        "empty ready discard changed"
    );
    require(backend.calls.empty(), "empty ready discard touched backend");
}

void generationInstallFinishesAfterLateCancellation() {
    for (const auto target : {
        WasapiOutputCheckpoint::afterStop,
        WasapiOutputCheckpoint::afterReset,
    }) {
        ScriptedBackend backend;
        WasapiPcmQueue queue(8, pcmFormat());
        std::stop_source source;
        CancellingCheckpoint checkpoint(target, source);
        WasapiOutputStateMachine machine(config(), backend, queue, &checkpoint);
        require(machine.start().outcome == WasapiOutputOutcome::changed, "checkpoint start failed");
        backend.calls.clear();

        const auto installed = machine.installGeneration(7, 42, source.get_token());
        require(installed.outcome == WasapiOutputOutcome::changed, "late install failed");
        require(installed.lateCancellation, "late install cancellation was hidden");
        require(installed.generation == 42, "late install lost exact generation");
        require(machine.state() == WasapiOutputState::ready, "late install state changed");
        require(
            backend.calls == std::vector<std::string>{"stop", "reset"},
            "late install did not finish one atomic flush"
        );
    }
}

void generationInstallFailureIsAtomic() {
    {
        ScriptedBackend backend;
        WasapiPcmQueue queue(8, pcmFormat());
        WasapiOutputStateMachine machine(config(), backend, queue);
        require(machine.start().outcome == WasapiOutputOutcome::changed, "start failed");
        backend.calls.clear();
        backend.stopResult = E_ACCESSDENIED;

        const auto failed = machine.installGeneration(7, 42);
        require(failed.outcome == WasapiOutputOutcome::failed, "Stop failure hidden");
        require(failed.stage == WasapiOutputStage::stopClient, "Stop failure stage lost");
        require(failed.generation == 7, "Stop failure installed generation");
        require(
            backend.calls == std::vector<std::string>{"stop", "close"},
            "Stop failure reset or skipped close"
        );
    }
    {
        ScriptedBackend backend;
        WasapiPcmQueue queue(8, pcmFormat());
        WasapiOutputStateMachine machine(config(), backend, queue);
        require(machine.start().outcome == WasapiOutputOutcome::changed, "start failed");
        backend.calls.clear();
        backend.resetResult = E_ACCESSDENIED;

        const auto failed = machine.installGeneration(7, 42);
        require(failed.outcome == WasapiOutputOutcome::failed, "Reset failure hidden");
        require(failed.stage == WasapiOutputStage::resetClient, "Reset failure stage lost");
        require(failed.generation == 7, "Reset failure installed generation");
        require(
            backend.calls == std::vector<std::string>{"stop", "reset", "close"},
            "Reset failure teardown order changed"
        );
    }
    {
        ScriptedBackend backend;
        WasapiPcmQueue queue(8, pcmFormat());
        WasapiOutputStateMachine machine(config(), backend, queue);
        require(machine.start().outcome == WasapiOutputOutcome::changed, "start failed");
        backend.calls.clear();
        backend.resetResult = AUDCLNT_E_DEVICE_INVALIDATED;

        const auto invalidated = machine.installGeneration(7, 42);
        require(
            invalidated.outcome == WasapiOutputOutcome::invalidated,
            "Reset invalidation hidden"
        );
        require(invalidated.generation == 7, "invalidation installed generation");
        require(
            backend.calls == std::vector<std::string>{"stop", "reset", "close"},
            "Reset invalidation teardown order changed"
        );
    }
}

void preservesPrecisionAndFailureReceipts() {
    ScriptedBackend degraded;
    degraded.clockResult = S_FALSE;
    WasapiPcmQueue degradedQueue(8, pcmFormat());
    WasapiOutputStateMachine degradedMachine(config(), degraded, degradedQueue);
    const auto sample = degradedMachine.start();
    require(sample.hasClockSample, "S_FALSE clock sample was dropped");
    require(sample.clockSample.precisionDegraded, "S_FALSE precision was hidden");
    require(sample.clockSample.generation == 7, "clock generation changed");

    ScriptedBackend releaseFailure;
    releaseFailure.releaseResult = E_ACCESSDENIED;
    WasapiPcmQueue failedQueue(8, pcmFormat());
    const auto input = frames({1, 2, 3, 4});
    require(failedQueue.enqueue(input), "failure PCM enqueue failed");
    WasapiOutputStateMachine failedMachine(config(), releaseFailure, failedQueue);
    const auto failed = failedMachine.start();
    require(failed.outcome == WasapiOutputOutcome::failed, "release failure hidden");
    require(failed.stage == WasapiOutputStage::releaseBuffer, "release stage lost");
    require(failed.hresult == E_ACCESSDENIED, "release HRESULT lost");
    require(failedQueue.availableFrames() == 4, "failed release consumed PCM");
    require(
        std::count(releaseFailure.calls.begin(), releaseFailure.calls.end(), "release:4") == 1,
        "failed release was retried"
    );
}

void closeStopsRunningStreamAndReleasesBackend() {
    ScriptedBackend backend;
    WasapiPcmQueue queue(8, pcmFormat());
    WasapiOutputStateMachine machine(config(), backend, queue);
    require(machine.start().outcome == WasapiOutputOutcome::changed, "start failed");
    backend.calls.clear();

    const auto result = machine.close();
    require(result.outcome == WasapiOutputOutcome::changed, "close failed");
    require(result.currentState == WasapiOutputState::closed, "close state wrong");
    require(backend.calls == std::vector<std::string>{"stop", "close"}, "close order changed");
    const auto closedCalls = backend.calls.size();
    require(machine.close().outcome == WasapiOutputOutcome::noOp, "repeat close changed");
    require(backend.calls.size() == closedCalls, "repeat close touched backend");
}

void clockFailureStillStopsTheStartedClientOnClose() {
    ScriptedBackend backend;
    backend.clockResult = E_ACCESSDENIED;
    WasapiPcmQueue queue(8, pcmFormat());
    WasapiOutputStateMachine machine(config(), backend, queue);
    const auto started = machine.start();
    require(started.outcome == WasapiOutputOutcome::failed, "clock failure hidden");
    require(started.stage == WasapiOutputStage::clockPosition, "clock stage lost");
    require(
        backend.calls.size() >= 2
            && backend.calls[backend.calls.size() - 2] == "stop"
            && backend.calls.back() == "close",
        "terminal clock failure did not stop and close"
    );
    const auto terminalCalls = backend.calls.size();
    machine.close();
    require(backend.calls.size() == terminalCalls, "terminal close repeated teardown");
}

}

int main() {
    try {
        primesBeforeStartAndCommitsOnlyReleasedPcm();
        rendersOnlyAfterAnEventAndCountsUnderrun();
        endOfStreamWithoutMediaCompletesBeforeStart();
        endOfStreamReleasesTheExactTailThenCompletes();
        cancellationInsideLeaseAbandonsAndRollsBack();
        cancellationBeforeWorkMakesNoBackendCall();
        cancellationInterruptsTheNativeEventWait();
        cancellationAfterReleaseReportsCommittedEffect();
        cancellationAfterNativeStartKeepsRunningState();
        validatesPaddingBeforeSubtraction();
        invalidationRejectsTheOldGeneration();
        pauseResumeResetAndRepeatedCommandsAreExact();
        exactGenerationDiscardAndInstallAreOrdered();
        generationInstallFinishesAfterLateCancellation();
        generationInstallFailureIsAtomic();
        preservesPrecisionAndFailureReceipts();
        closeStopsRunningStreamAndReleasesBackend();
        clockFailureStillStopsTheStartedClientOnClose();
        std::cout << "WASAPI output state tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
