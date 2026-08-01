#include "palmier/audio/wasapi_output_worker.hpp"

#include "wasapi_native_stream.hpp"
#include "wasapi_output_worker_testing.hpp"

#include <Windows.h>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <variant>

namespace palmier::audio {
namespace {

constexpr HRESULT cancelledResult = HRESULT_FROM_WIN32(ERROR_CANCELLED);

class NativeWorkerStream final : public WasapiOutputWorkerStream {
public:
    HRESULT initializeApartment() override { return stream_.initializeApartment(); }
    HRESULT createEnumerator() override { return stream_.createEnumerator(); }
    HRESULT selectDefaultRenderEndpoint(std::string& endpointId) override {
        return stream_.selectDefaultRenderEndpoint(endpointId);
    }
    HRESULT activateAudioClient() override { return stream_.activateAudioClient(); }
    HRESULT loadMixFormat(WasapiMixFormat& format) override {
        return stream_.loadMixFormat(format);
    }
    HRESULT setClientProperties() override { return stream_.setClientProperties(); }
    HRESULT loadSharedModePeriods(WasapiSharedModePeriods& periods) override {
        return stream_.loadSharedModePeriods(periods);
    }
    HRESULT initializeSharedAudioStream(std::uint32_t periodFrames) override {
        return stream_.initializeSharedAudioStream(periodFrames);
    }
    HRESULT loadBufferFrames(std::uint32_t& bufferFrames) override {
        return stream_.loadBufferFrames(bufferFrames);
    }
    HRESULT attachRenderEvent() override { return stream_.attachRenderEvent(); }
    HRESULT loadRenderService() override { return stream_.loadRenderService(); }
    HRESULT loadClockService() override { return stream_.loadClockService(); }
    HRESULT loadClockFrequency(std::uint64_t& frequency) override {
        return stream_.loadClockFrequency(frequency);
    }
    HRESULT waitForRenderEvent(
        std::stop_token stopToken,
        std::uint32_t timeoutMilliseconds
    ) noexcept override {
        return stream_.waitForRenderEvent(stopToken, timeoutMilliseconds);
    }
    HRESULT loadCurrentPadding(std::uint32_t& frames) noexcept override {
        return stream_.loadCurrentPadding(frames);
    }
    HRESULT acquireBuffer(
        std::uint32_t frameCount,
        std::byte*& data
    ) noexcept override {
        return stream_.acquireBuffer(frameCount, data);
    }
    HRESULT releaseBuffer(std::uint32_t frames, DWORD flags) noexcept override {
        return stream_.releaseBuffer(frames, flags);
    }
    HRESULT start() noexcept override { return stream_.start(); }
    HRESULT loadClockPosition(WasapiClockReading& reading) noexcept override {
        return stream_.loadClockPosition(reading);
    }
    HRESULT stop() noexcept override { return stream_.stop(); }
    HRESULT reset() noexcept override { return stream_.reset(); }
    HRESULT close() noexcept override { return stream_.close(); }

private:
    WasapiNativeStream stream_;
};

struct ControlCompletion final {
    std::mutex mutex;
    std::condition_variable condition;
    std::optional<WasapiOutputReceipt> value;
};

struct PcmCompletion final {
    std::mutex mutex;
    std::condition_variable_any condition;
    std::optional<WasapiWorkerPcmReceipt> value;
};

enum class ControlKind {
    start,
    pause,
    discardGeneration,
    installGeneration,
    close,
};

struct ControlCommand final {
    ControlKind kind{ControlKind::close};
    std::uint64_t expectedGeneration{};
    std::uint64_t nextGeneration{};
    std::shared_ptr<ControlCompletion> completion;
};

struct EndOfStream final {
    std::uint64_t generation{};
    std::uint64_t finalOutputSample{};
};

using PcmPayload = std::variant<WasapiWorkerPcmBlock, EndOfStream>;

struct PcmCommand final {
    PcmPayload payload;
    std::shared_ptr<PcmCompletion> completion;
};

WasapiOutputOperation operation(ControlKind kind) {
    switch (kind) {
    case ControlKind::start: return WasapiOutputOperation::start;
    case ControlKind::pause: return WasapiOutputOperation::pause;
    case ControlKind::discardGeneration:
        return WasapiOutputOperation::discardGeneration;
    case ControlKind::installGeneration:
        return WasapiOutputOperation::installGeneration;
    case ControlKind::close: return WasapiOutputOperation::close;
    }
    return WasapiOutputOperation::close;
}

WasapiOutputReceipt unavailableReceipt(
    ControlKind kind,
    HRESULT result,
    std::uint64_t generation = 0
) {
    WasapiOutputReceipt value;
    value.operation = operation(kind);
    value.outcome = WasapiOutputOutcome::failed;
    value.previousState = WasapiOutputState::failed;
    value.currentState = WasapiOutputState::failed;
    value.hresult = result;
    value.generation = generation;
    return value;
}

WasapiOutputReceipt generationRefusal(
    ControlKind kind,
    WasapiOutputState state,
    std::uint64_t generation
) {
    auto value = unavailableReceipt(kind, E_INVALIDARG, generation);
    value.outcome = WasapiOutputOutcome::refused;
    value.previousState = state;
    value.currentState = state;
    value.stage = WasapiOutputStage::generationInvariant;
    return value;
}

void complete(
    const std::shared_ptr<ControlCompletion>& completion,
    WasapiOutputReceipt value
) {
    {
        std::lock_guard lock(completion->mutex);
        completion->value = std::move(value);
    }
    completion->condition.notify_all();
}

void complete(
    const std::shared_ptr<PcmCompletion>& completion,
    WasapiWorkerPcmReceipt value
) {
    {
        std::lock_guard lock(completion->mutex);
        completion->value = std::move(value);
    }
    completion->condition.notify_all();
}

}

class WasapiOutputWorker::Impl final {
public:
    explicit Impl(WasapiOutputWorkerStreamFactory factory)
        : factory_(validatedFactory(std::move(factory))),
          worker_([this](std::stop_token token) {
              run(token);
          }) {}

    ~Impl() noexcept {
        try {
            close();
        } catch (...) {
            worker_.request_stop();
            requestRenderCancellation();
            condition_.notify_all();
            if (worker_.joinable()) {
                try {
                    worker_.join();
                } catch (...) {
                }
            }
        }
    }

    WasapiOutputReceipt control(
        ControlKind kind,
        std::uint64_t expectedGeneration = 0,
        std::uint64_t nextGeneration = 0
    ) {
        auto completion = std::make_shared<ControlCompletion>();
        {
            std::lock_guard lock(mutex_);
            if (joined_ || (closeStarted_ && kind != ControlKind::close)) {
                if (kind == ControlKind::close && closeReceipt_.has_value()) {
                    return *closeReceipt_;
                }
                return unavailableReceipt(kind, E_ILLEGAL_METHOD_CALL);
            }
            constexpr std::size_t maxPendingControls = 16;
            if (kind != ControlKind::close
                && controls_.size() >= maxPendingControls) {
                auto value = unavailableReceipt(kind, HRESULT_FROM_WIN32(ERROR_RETRY));
                value.outcome = WasapiOutputOutcome::refused;
                return value;
            }
            controls_.push_back({kind, expectedGeneration, nextGeneration, completion});
            interruptRenderLocked();
        }
        condition_.notify_all();
        std::unique_lock completionLock(completion->mutex);
        completion->condition.wait(
            completionLock,
            [&completion] { return completion->value.has_value(); }
        );
        return *completion->value;
    }

    WasapiWorkerConfiguration configuration(std::stop_token stopToken) {
        std::unique_lock lock(mutex_);
        const bool ready = condition_.wait(
            lock,
            stopToken,
            [this] {
                return configuration_.has_value()
                    || startupUnavailable_
                    || closeStarted_
                    || joined_;
            }
        );
        if (!ready || stopToken.stop_requested()) {
            return {
                WasapiWorkerConfigurationOutcome::cancelled,
                cancelledResult,
            };
        }
        if (configuration_.has_value() && !closeStarted_ && !joined_) {
            return *configuration_;
        }
        if (startupUnavailable_) {
            return {
                startupOutcome_,
                startupFailure_,
            };
        }
        return {
            WasapiWorkerConfigurationOutcome::closed,
            E_ILLEGAL_METHOD_CALL,
        };
    }

    WasapiWorkerClockReceipt clockPosition(
        std::uint64_t expectedGeneration
    ) {
        std::lock_guard lock(mutex_);
        if (closeStarted_ || joined_) {
            return {
                WasapiWorkerClockOutcome::closed,
                E_ILLEGAL_METHOD_CALL,
                currentGeneration_,
            };
        }
        if (startupUnavailable_) {
            return {
                startupOutcome_ == WasapiWorkerConfigurationOutcome::unavailable
                    ? WasapiWorkerClockOutcome::unavailable
                    : WasapiWorkerClockOutcome::failed,
                startupFailure_,
                currentGeneration_,
            };
        }
        if (expectedGeneration == 0
            || expectedGeneration != currentGeneration_) {
            return {
                WasapiWorkerClockOutcome::refused,
                E_INVALIDARG,
                currentGeneration_,
            };
        }
        if (configuration_.has_value()
            && configuration_->outcome
                == WasapiWorkerConfigurationOutcome::unavailable) {
            return {
                WasapiWorkerClockOutcome::unavailable,
                configuration_->hresult,
                currentGeneration_,
            };
        }
        if (configuration_.has_value()
            && configuration_->outcome
                == WasapiWorkerConfigurationOutcome::failed) {
            return {
                WasapiWorkerClockOutcome::failed,
                configuration_->hresult,
                currentGeneration_,
            };
        }
        if (!latestClockSample_.has_value()
            || latestClockSample_->generation != expectedGeneration) {
            return {
                WasapiWorkerClockOutcome::noSample,
                E_PENDING,
                currentGeneration_,
            };
        }
        return {
            WasapiWorkerClockOutcome::available,
            S_OK,
            currentGeneration_,
            true,
            *latestClockSample_,
        };
    }

    WasapiWorkerPcmReceipt submit(
        PcmPayload payload,
        std::stop_token stopToken
    ) {
        auto completion = std::make_shared<PcmCompletion>();
        {
            std::unique_lock lock(mutex_);
            const bool available = condition_.wait(
                lock,
                stopToken,
                [this] {
                    return !pcm_.has_value() || closeStarted_ || joined_;
                }
            );
            if (!available || stopToken.stop_requested()) {
                return cancelledPcm();
            }
            if (closeStarted_ || joined_) {
                return closedPcm();
            }
            pcm_ = PcmCommand{std::move(payload), completion};
            interruptRenderLocked();
        }
        condition_.notify_all();
        std::unique_lock completionLock(completion->mutex);
        const bool completed = completion->condition.wait(
            completionLock,
            stopToken,
            [&completion] { return completion->value.has_value(); }
        );
        if (!completed) {
            {
                std::lock_guard lock(mutex_);
                if (pcm_.has_value()
                    && pcm_->completion == completion) {
                    pcm_.reset();
                    condition_.notify_all();
                    return cancelledPcm();
                }
            }
            completion->condition.wait(
                completionLock,
                [&completion] { return completion->value.has_value(); }
            );
        }
        return *completion->value;
    }

    WasapiOutputReceipt waitForTerminal(
        std::uint64_t generation,
        std::stop_token stopToken
    ) {
        std::unique_lock lock(mutex_);
        const bool completed = condition_.wait(
            lock,
            stopToken,
            [this, generation] {
                return terminalForGenerationLocked(generation).has_value()
                    || (currentGeneration_ != 0
                        && currentGeneration_ != generation)
                    || startupUnavailable_
                    || joined_;
            }
        );
        if (completed) {
            const auto terminal = terminalForGenerationLocked(generation);
            if (terminal.has_value()) {
                return *terminal;
            }
        }
        if (currentGeneration_ != 0 && currentGeneration_ != generation) {
            return generationRefusal(
                ControlKind::close,
                WasapiOutputState::failed,
                currentGeneration_
            );
        }
        if (startupUnavailable_) {
            return unavailableReceipt(
                ControlKind::close,
                startupFailure_,
                generation
            );
        }
        if (joined_ && closeReceipt_.has_value()) {
            return *closeReceipt_;
        }
        auto value = unavailableReceipt(
            ControlKind::close,
            stopToken.stop_requested() ? cancelledResult : E_ILLEGAL_METHOD_CALL,
            generation
        );
        if (stopToken.stop_requested()) {
            value.outcome = WasapiOutputOutcome::cancelled;
        }
        return value;
    }

    WasapiOutputReceipt close() {
        bool workerAlreadyExited = false;
        {
            std::unique_lock lock(mutex_);
            if (closeFinished_) {
                return closeReceipt_.value_or(
                    unavailableReceipt(ControlKind::close, S_OK)
                );
            }
            if (closeStarted_) {
                condition_.wait(lock, [this] { return closeFinished_; });
                return closeReceipt_.value_or(
                    unavailableReceipt(ControlKind::close, S_OK)
                );
            }
            closeStarted_ = true;
            workerAlreadyExited = joined_;
        }
        condition_.notify_all();

        WasapiOutputReceipt value;
        if (workerAlreadyExited) {
            value = closeReceipt_.value_or(
                unavailableReceipt(ControlKind::close, E_ILLEGAL_METHOD_CALL)
            );
        } else {
            try {
                value = control(ControlKind::close);
            } catch (const std::bad_alloc&) {
                value = unavailableReceipt(ControlKind::close, E_OUTOFMEMORY);
                worker_.request_stop();
                requestRenderCancellation();
                condition_.notify_all();
            } catch (...) {
                value = unavailableReceipt(ControlKind::close, E_UNEXPECTED);
                worker_.request_stop();
                requestRenderCancellation();
                condition_.notify_all();
            }
        }
        try {
            if (worker_.joinable()) {
                worker_.join();
            }
        } catch (...) {
            value = unavailableReceipt(ControlKind::close, E_UNEXPECTED);
        }
        {
            std::lock_guard lock(mutex_);
            joined_ = true;
            closeFinished_ = true;
            closeReceipt_ = value;
        }
        condition_.notify_all();
        return value;
    }

private:
    static WasapiOutputWorkerStreamFactory validatedFactory(
        WasapiOutputWorkerStreamFactory factory
    ) {
        if (!factory) {
            throw std::invalid_argument("missing WASAPI worker stream factory");
        }
        return factory;
    }

    static WasapiWorkerPcmReceipt cancelledPcm() {
        return {
            WasapiWorkerPcmOutcome::cancelled,
            WasapiWorkerPcmStage::none,
            cancelledResult,
        };
    }

    static WasapiWorkerPcmReceipt closedPcm() {
        return {
            WasapiWorkerPcmOutcome::refused,
            WasapiWorkerPcmStage::closed,
            E_ILLEGAL_METHOD_CALL,
        };
    }

    void interruptRenderLocked() {
        if (renderWaitActive_) {
            renderCancellation_.request_stop();
        }
    }

    void requestRenderCancellation() {
        std::lock_guard lock(mutex_);
        renderCancellation_.request_stop();
    }

    void setStartupFailure(
        WasapiWorkerConfigurationOutcome outcome,
        HRESULT result
    ) {
        {
            std::lock_guard lock(mutex_);
            startupOutcome_ = outcome;
            startupFailure_ = result;
            startupUnavailable_ = true;
        }
        condition_.notify_all();
    }

    void recordTerminal(const WasapiOutputReceipt& value) {
        if (value.currentState == WasapiOutputState::completed
            || value.currentState == WasapiOutputState::failed
            || value.currentState == WasapiOutputState::invalidated
            || value.currentState == WasapiOutputState::closed) {
            std::lock_guard lock(mutex_);
            if (value.currentState == WasapiOutputState::closed) {
                closeReceipt_ = value;
            } else {
                if (configuration_.has_value()
                    && value.currentState == WasapiOutputState::failed) {
                    configuration_->outcome =
                        WasapiWorkerConfigurationOutcome::failed;
                    configuration_->hresult = value.hresult;
                    latestClockSample_.reset();
                } else if (configuration_.has_value()
                    && value.currentState == WasapiOutputState::invalidated) {
                    configuration_->outcome =
                        WasapiWorkerConfigurationOutcome::unavailable;
                    configuration_->hresult = value.hresult;
                    latestClockSample_.reset();
                }
                const auto existing = std::find_if(
                    terminalHistory_.begin(),
                    terminalHistory_.end(),
                    [&value](const WasapiOutputReceipt& receipt) {
                        return receipt.generation == value.generation;
                    }
                );
                if (existing != terminalHistory_.end()) {
                    *existing = value;
                } else {
                    constexpr std::size_t terminalHistoryCapacity = 16;
                    if (terminalHistory_.size() == terminalHistoryCapacity) {
                        terminalHistory_.pop_front();
                    }
                    terminalHistory_.push_back(value);
                }
            }
            condition_.notify_all();
        }
    }

    std::optional<WasapiOutputReceipt> terminalForGenerationLocked(
        std::uint64_t generation
    ) const {
        const auto terminal = std::find_if(
            terminalHistory_.begin(),
            terminalHistory_.end(),
            [generation](const WasapiOutputReceipt& receipt) {
                return receipt.generation == generation;
            }
        );
        return terminal == terminalHistory_.end()
            ? std::nullopt
            : std::optional<WasapiOutputReceipt>(*terminal);
    }

    WasapiOutputReceipt executeControl(
        const ControlCommand& command,
        WasapiOutputStateMachine& machine
    ) {
        if (command.kind != ControlKind::close
            && command.expectedGeneration != machine.generation()) {
            return generationRefusal(
                command.kind,
                machine.state(),
                machine.generation()
            );
        }
        switch (command.kind) {
        case ControlKind::start:
            return machine.start();
        case ControlKind::pause:
            return machine.pause();
        case ControlKind::discardGeneration:
            return machine.discardGeneration(command.expectedGeneration);
        case ControlKind::installGeneration:
            return machine.installGeneration(
                command.expectedGeneration,
                command.nextGeneration
            );
        case ControlKind::close:
            return machine.close();
        }
        return unavailableReceipt(command.kind, E_UNEXPECTED, machine.generation());
    }

    WasapiWorkerPcmReceipt validateAndAdmit(
        const PcmPayload& payload,
        const PcmFormat& format,
        std::uint32_t capacityFrames,
        WasapiOutputStateMachine& machine,
        WasapiPcmQueue& queue
    ) {
        WasapiWorkerPcmReceipt value;
        value.generation = machine.generation();
        if (machine.state() == WasapiOutputState::completed
            || machine.state() == WasapiOutputState::invalidated
            || machine.state() == WasapiOutputState::failed
            || machine.state() == WasapiOutputState::closed) {
            value.outcome = WasapiWorkerPcmOutcome::refused;
            value.stage = WasapiWorkerPcmStage::closed;
            value.hresult = E_ILLEGAL_METHOD_CALL;
            return value;
        }
        if (const auto* block = std::get_if<WasapiWorkerPcmBlock>(&payload)) {
            if (block->generation != machine.generation()) {
                value.outcome = WasapiWorkerPcmOutcome::refused;
                value.stage = WasapiWorkerPcmStage::generationInvariant;
                value.hresult = E_INVALIDARG;
                return value;
            }
            if (block->pcmFormat != format) {
                value.outcome = WasapiWorkerPcmOutcome::refused;
                value.stage = WasapiWorkerPcmStage::formatInvariant;
                value.hresult = E_INVALIDARG;
                return value;
            }
            if (queue.endOfStream()) {
                value.outcome = WasapiWorkerPcmOutcome::refused;
                value.stage = WasapiWorkerPcmStage::sampleInvariant;
                value.hresult = E_ILLEGAL_METHOD_CALL;
                value.nextOutputSample = nextOutputSample_.value_or(0);
                return value;
            }
            const std::uint64_t byteCount = static_cast<std::uint64_t>(block->frameCount)
                * format.blockAlign;
            if (block->frameCount == 0 || block->frameCount > capacityFrames
                || byteCount != block->bytes.size()) {
                value.outcome = WasapiWorkerPcmOutcome::refused;
                value.stage = WasapiWorkerPcmStage::capacityInvariant;
                value.hresult = E_INVALIDARG;
                return value;
            }
            if (nextOutputSample_.has_value()
                && block->startOutputSample != *nextOutputSample_) {
                value.outcome = WasapiWorkerPcmOutcome::refused;
                value.stage = WasapiWorkerPcmStage::sampleInvariant;
                value.hresult = E_INVALIDARG;
                value.nextOutputSample = *nextOutputSample_;
                return value;
            }
            if (block->startOutputSample
                > (std::numeric_limits<std::uint64_t>::max)() - block->frameCount) {
                value.outcome = WasapiWorkerPcmOutcome::refused;
                value.stage = WasapiWorkerPcmStage::sampleInvariant;
                value.hresult = E_INVALIDARG;
                return value;
            }
            if (block->frameCount > queue.freeFrames()) {
                value.outcome = WasapiWorkerPcmOutcome::failed;
                value.stage = WasapiWorkerPcmStage::capacityInvariant;
                value.hresult = HRESULT_FROM_WIN32(ERROR_RETRY);
                return value;
            }
            if (!queue.enqueue(block->bytes)) {
                value.outcome = WasapiWorkerPcmOutcome::failed;
                value.stage = WasapiWorkerPcmStage::capacityInvariant;
                value.hresult = E_UNEXPECTED;
                return value;
            }
            nextOutputSample_ = block->startOutputSample + block->frameCount;
            value.outcome = WasapiWorkerPcmOutcome::accepted;
            value.hresult = S_OK;
            value.nextOutputSample = *nextOutputSample_;
            value.acceptedFrames = block->frameCount;
            return value;
        }

        const auto& end = std::get<EndOfStream>(payload);
        if (end.generation != machine.generation()) {
            value.outcome = WasapiWorkerPcmOutcome::refused;
            value.stage = WasapiWorkerPcmStage::generationInvariant;
            value.hresult = E_INVALIDARG;
            return value;
        }
        if (queue.endOfStream()) {
            if (nextOutputSample_.has_value()
                && end.finalOutputSample == *nextOutputSample_) {
                value.outcome = WasapiWorkerPcmOutcome::noOp;
                value.hresult = S_OK;
                value.nextOutputSample = *nextOutputSample_;
                value.endOfStream = true;
                return value;
            }
            value.outcome = WasapiWorkerPcmOutcome::refused;
            value.stage = WasapiWorkerPcmStage::sampleInvariant;
            value.hresult = E_INVALIDARG;
            value.nextOutputSample = nextOutputSample_.value_or(0);
            return value;
        }
        if (nextOutputSample_.has_value()
            && end.finalOutputSample != *nextOutputSample_) {
            value.outcome = WasapiWorkerPcmOutcome::refused;
            value.stage = WasapiWorkerPcmStage::sampleInvariant;
            value.hresult = E_INVALIDARG;
            value.nextOutputSample = *nextOutputSample_;
            return value;
        }
        nextOutputSample_ = end.finalOutputSample;
        queue.markEndOfStream();
        value.outcome = WasapiWorkerPcmOutcome::accepted;
        value.hresult = S_OK;
        value.nextOutputSample = end.finalOutputSample;
        value.endOfStream = true;
        return value;
    }

    void rejectPending(HRESULT result) {
        std::deque<ControlCommand> controls;
        std::optional<PcmCommand> pcm;
        {
            std::lock_guard lock(mutex_);
            controls.swap(controls_);
            pcm.swap(pcm_);
        }
        for (const auto& command : controls) {
            complete(command.completion, unavailableReceipt(command.kind, result));
        }
        if (pcm.has_value()) {
            complete(
                pcm->completion,
                {
                    WasapiWorkerPcmOutcome::failed,
                    WasapiWorkerPcmStage::closed,
                    result,
                }
            );
        }
    }

    void run(std::stop_token stopToken) noexcept {
        try {
            auto stream = factory_();
            if (!stream) {
                throw std::bad_alloc();
            }
            const auto environment = runWasapiEnvironmentProbe(*stream);
            if (environment.status != WasapiProbeStatus::available) {
                setStartupFailure(
                    environment.status == WasapiProbeStatus::unavailable
                        ? WasapiWorkerConfigurationOutcome::unavailable
                        : WasapiWorkerConfigurationOutcome::failed,
                    environment.hresult
                );
                stream->close();
                runUnavailable(stopToken);
            } else {
                WasapiPcmQueue queue(environment.bufferFrames, environment.pcmFormat);
                WasapiOutputStateMachine machine(
                    {
                        environment.bufferFrames,
                        environment.pcmFormat,
                        environment.clockFrequency,
                        1,
                    },
                    *stream,
                    queue
                );
                {
                    std::lock_guard lock(mutex_);
                    currentGeneration_ = machine.generation();
                    configuration_ = WasapiWorkerConfiguration{
                        WasapiWorkerConfigurationOutcome::available,
                        S_OK,
                        machine.generation(),
                        environment.bufferFrames,
                        environment.clockFrequency,
                        environment.pcmFormat,
                    };
                }
                condition_.notify_all();
                runReady(stopToken, environment, machine, queue);
            }
        } catch (const std::bad_alloc&) {
            setStartupFailure(
                WasapiWorkerConfigurationOutcome::failed,
                E_OUTOFMEMORY
            );
            runUnavailable(stopToken);
        } catch (...) {
            setStartupFailure(
                WasapiWorkerConfigurationOutcome::failed,
                E_UNEXPECTED
            );
            runUnavailable(stopToken);
        }
        rejectPending(E_ILLEGAL_METHOD_CALL);
        {
            std::lock_guard lock(mutex_);
            joined_ = true;
        }
        condition_.notify_all();
    }

    void runUnavailable(std::stop_token stopToken) {
        while (!stopToken.stop_requested()) {
            std::optional<ControlCommand> command;
            std::optional<PcmCommand> pcmCommand;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(
                    lock,
                    stopToken,
                    [this] { return !controls_.empty() || pcm_.has_value(); }
                );
                if (stopToken.stop_requested()) {
                    return;
                }
                if (!controls_.empty()) {
                    command = std::move(controls_.front());
                    controls_.pop_front();
                } else {
                    pcmCommand = std::move(pcm_);
                    pcm_.reset();
                    condition_.notify_all();
                }
            }
            if (pcmCommand.has_value()) {
                complete(
                    pcmCommand->completion,
                    {
                        WasapiWorkerPcmOutcome::failed,
                        WasapiWorkerPcmStage::closed,
                        startupFailure_,
                    }
                );
                continue;
            }
            if (command->kind == ControlKind::close) {
                auto value = unavailableReceipt(ControlKind::close, S_OK);
                value.outcome = WasapiOutputOutcome::changed;
                value.currentState = WasapiOutputState::closed;
                complete(command->completion, value);
                recordTerminal(value);
                return;
            }
            complete(
                command->completion,
                unavailableReceipt(command->kind, startupFailure_)
            );
        }
    }

    void runReady(
        std::stop_token stopToken,
        const WasapiEnvironmentProbeResult& environment,
        WasapiOutputStateMachine& machine,
        WasapiPcmQueue& queue
    ) {
        bool closing = false;
        while (!stopToken.stop_requested() && !closing) {
            std::optional<ControlCommand> controlCommand;
            std::optional<PcmCommand> pcmCommand;
            bool render = false;
            {
                std::unique_lock lock(mutex_);
                if (!controls_.empty()) {
                    controlCommand = std::move(controls_.front());
                    controls_.pop_front();
                } else if (pcm_.has_value()) {
                    bool canAdmit = true;
                    if (const auto* block = std::get_if<WasapiWorkerPcmBlock>(
                            &pcm_->payload
                        )) {
                        canAdmit = block->frameCount > environment.bufferFrames
                            || block->frameCount <= queue.freeFrames();
                    }
                    if (canAdmit) {
                        pcmCommand = std::move(pcm_);
                        pcm_.reset();
                        condition_.notify_all();
                    } else if (machine.state() == WasapiOutputState::running) {
                        render = true;
                    } else {
                        pcmCommand = std::move(pcm_);
                        pcm_.reset();
                        condition_.notify_all();
                    }
                } else if (machine.state() == WasapiOutputState::running) {
                    render = true;
                }
                if (!controlCommand.has_value() && !pcmCommand.has_value() && !render) {
                    condition_.wait(
                        lock,
                        stopToken,
                        [this] { return !controls_.empty() || pcm_.has_value(); }
                    );
                    continue;
                }
                if (render) {
                    renderCancellation_ = std::stop_source{};
                    renderWaitActive_ = true;
                }
            }

            if (controlCommand.has_value()) {
                auto value = executeControl(*controlCommand, machine);
                std::optional<PcmCommand> discardedPcm;
                {
                    std::lock_guard lock(mutex_);
                    currentGeneration_ = value.generation;
                    if (configuration_.has_value()) {
                        configuration_->generation = value.generation;
                    }
                    if (value.hasClockSample) {
                        latestClockSample_ = value.clockSample;
                    }
                }
                if ((controlCommand->kind == ControlKind::discardGeneration
                        || controlCommand->kind == ControlKind::installGeneration)
                    && value.outcome == WasapiOutputOutcome::changed) {
                    nextOutputSample_.reset();
                    {
                        std::lock_guard lock(mutex_);
                        latestClockSample_.reset();
                        if (controlCommand->kind
                            == ControlKind::discardGeneration) {
                            terminalHistory_.erase(
                                std::remove_if(
                                    terminalHistory_.begin(),
                                    terminalHistory_.end(),
                                    [&value](const WasapiOutputReceipt& receipt) {
                                        return receipt.generation == value.generation;
                                    }
                                ),
                                terminalHistory_.end()
                            );
                        }
                        discardedPcm = std::move(pcm_);
                        pcm_.reset();
                    }
                    condition_.notify_all();
                }
                if (discardedPcm.has_value()) {
                    complete(
                        discardedPcm->completion,
                        {
                            WasapiWorkerPcmOutcome::refused,
                            WasapiWorkerPcmStage::generationInvariant,
                            E_INVALIDARG,
                            machine.generation(),
                        }
                    );
                }
                recordTerminal(value);
                complete(controlCommand->completion, value);
                closing = controlCommand->kind == ControlKind::close;
                continue;
            }

            if (pcmCommand.has_value()) {
                auto value = validateAndAdmit(
                    pcmCommand->payload,
                    environment.pcmFormat,
                    environment.bufferFrames,
                    machine,
                    queue
                );
                complete(pcmCommand->completion, value);
                continue;
            }

            const auto value = machine.renderOnce(
                INFINITE,
                renderCancellation_.get_token()
            );
            {
                std::lock_guard lock(mutex_);
                renderWaitActive_ = false;
                if (value.hasClockSample) {
                    latestClockSample_ = value.clockSample;
                }
            }
            if (value.outcome != WasapiOutputOutcome::cancelled) {
                recordTerminal(value);
            }
        }
    }

    WasapiOutputWorkerStreamFactory factory_;
    std::mutex mutex_;
    std::condition_variable_any condition_;
    std::deque<ControlCommand> controls_;
    std::optional<PcmCommand> pcm_;
    std::deque<WasapiOutputReceipt> terminalHistory_;
    std::optional<WasapiOutputReceipt> closeReceipt_;
    std::optional<WasapiWorkerConfiguration> configuration_;
    std::optional<AudioClockSample> latestClockSample_;
    std::optional<std::uint64_t> nextOutputSample_;
    std::uint64_t currentGeneration_{};
    std::stop_source renderCancellation_;
    HRESULT startupFailure_{E_UNEXPECTED};
    WasapiWorkerConfigurationOutcome startupOutcome_{
        WasapiWorkerConfigurationOutcome::failed
    };
    bool renderWaitActive_{};
    bool startupUnavailable_{};
    bool closeStarted_{};
    bool closeFinished_{};
    bool joined_{};
    std::jthread worker_;
};

WasapiOutputWorker::WasapiOutputWorker()
    : WasapiOutputWorker([] {
          return std::make_unique<NativeWorkerStream>();
      }) {}

WasapiOutputWorker::WasapiOutputWorker(WasapiOutputWorkerStreamFactory factory)
    : impl_(std::make_unique<Impl>(std::move(factory))) {}

WasapiOutputWorker::~WasapiOutputWorker() = default;

WasapiWorkerConfiguration WasapiOutputWorker::configuration(
    std::stop_token stopToken
) {
    return impl_->configuration(stopToken);
}

WasapiOutputReceipt WasapiOutputWorker::start(std::uint64_t expectedGeneration) {
    return impl_->control(ControlKind::start, expectedGeneration);
}

WasapiOutputReceipt WasapiOutputWorker::pause(std::uint64_t expectedGeneration) {
    return impl_->control(ControlKind::pause, expectedGeneration);
}

WasapiOutputReceipt WasapiOutputWorker::discardGeneration(
    std::uint64_t expectedGeneration
) {
    return impl_->control(ControlKind::discardGeneration, expectedGeneration);
}

WasapiOutputReceipt WasapiOutputWorker::installGeneration(
    std::uint64_t expectedGeneration,
    std::uint64_t nextGeneration
) {
    return impl_->control(
        ControlKind::installGeneration,
        expectedGeneration,
        nextGeneration
    );
}

WasapiWorkerPcmReceipt WasapiOutputWorker::submit(
    WasapiWorkerPcmBlock block,
    std::stop_token stopToken
) {
    return impl_->submit(std::move(block), stopToken);
}

WasapiWorkerPcmReceipt WasapiOutputWorker::markEndOfStream(
    std::uint64_t generation,
    std::uint64_t finalOutputSample,
    std::stop_token stopToken
) {
    return impl_->submit(
        EndOfStream{generation, finalOutputSample},
        stopToken
    );
}

WasapiOutputReceipt WasapiOutputWorker::waitForTerminal(
    std::uint64_t generation,
    std::stop_token stopToken
) {
    return impl_->waitForTerminal(generation, stopToken);
}

WasapiWorkerClockReceipt WasapiOutputWorker::clockPosition(
    std::uint64_t expectedGeneration
) const {
    return impl_->clockPosition(expectedGeneration);
}

WasapiOutputReceipt WasapiOutputWorker::close() {
    return impl_->close();
}

}
