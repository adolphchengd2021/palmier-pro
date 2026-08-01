#include "palmier/audio/wasapi_output.hpp"

#include "wasapi_output_backend.hpp"

#include <audioclient.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace palmier::audio {
namespace {

constexpr HRESULT cancelledResult = HRESULT_FROM_WIN32(ERROR_CANCELLED);

bool multiplyFitsSize(
    std::uint32_t frameCount,
    std::uint16_t blockAlign,
    std::size_t& byteCount
) {
    const auto product = static_cast<std::uint64_t>(frameCount) * blockAlign;
    if (product > std::numeric_limits<std::size_t>::max()) {
        return false;
    }
    byteCount = static_cast<std::size_t>(product);
    return true;
}

}

WasapiPcmQueue::WasapiPcmQueue(
    std::uint32_t capacityFrames,
    PcmFormat format
) : capacityFrames_(capacityFrames), format_(format) {
    std::size_t byteCount{};
    if (capacityFrames == 0 || !isValidPcmFormat(format_)
        || !multiplyFitsSize(capacityFrames, format_.blockAlign, byteCount)) {
        throw std::invalid_argument("invalid WASAPI PCM queue capacity");
    }
    storage_.resize(byteCount);
}

bool WasapiPcmQueue::enqueue(std::span<const std::byte> bytes) {
    if (bytes.empty() || bytes.size() % format_.blockAlign != 0) {
        return false;
    }
    const auto frameCount64 = bytes.size() / format_.blockAlign;
    if (frameCount64 > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    const auto frameCount = static_cast<std::uint32_t>(frameCount64);
    if (frameCount > freeFrames() || endOfStream_) {
        return false;
    }

    const std::uint32_t tailFrame = (headFrame_ + sizeFrames_) % capacityFrames_;
    const std::uint32_t firstFrames = std::min(frameCount, capacityFrames_ - tailFrame);
    const std::size_t firstBytes = static_cast<std::size_t>(firstFrames)
        * format_.blockAlign;
    std::memcpy(
        storage_.data() + static_cast<std::size_t>(tailFrame) * format_.blockAlign,
        bytes.data(),
        firstBytes
    );
    const std::uint32_t secondFrames = frameCount - firstFrames;
    if (secondFrames > 0) {
        std::memcpy(
            storage_.data(),
            bytes.data() + firstBytes,
            static_cast<std::size_t>(secondFrames) * format_.blockAlign
        );
    }
    sizeFrames_ += frameCount;
    return true;
}

void WasapiPcmQueue::markEndOfStream() {
    endOfStream_ = true;
}

void WasapiPcmQueue::clear() {
    headFrame_ = 0;
    sizeFrames_ = 0;
    endOfStream_ = false;
}

std::uint32_t WasapiPcmQueue::availableFrames() const {
    return sizeFrames_;
}

std::uint32_t WasapiPcmQueue::freeFrames() const {
    return capacityFrames_ - sizeFrames_;
}

bool WasapiPcmQueue::endOfStream() const {
    return endOfStream_;
}

void WasapiPcmQueue::copyFrames(
    std::byte* destination,
    std::uint32_t frameCount
) const {
    const std::uint32_t firstFrames = std::min(frameCount, capacityFrames_ - headFrame_);
    const std::size_t firstBytes = static_cast<std::size_t>(firstFrames)
        * format_.blockAlign;
    std::memcpy(
        destination,
        storage_.data() + static_cast<std::size_t>(headFrame_) * format_.blockAlign,
        firstBytes
    );
    const std::uint32_t secondFrames = frameCount - firstFrames;
    if (secondFrames > 0) {
        std::memcpy(
            destination + firstBytes,
            storage_.data(),
            static_cast<std::size_t>(secondFrames) * format_.blockAlign
        );
    }
}

void WasapiPcmQueue::commitFrames(std::uint32_t frameCount) {
    headFrame_ = (headFrame_ + frameCount) % capacityFrames_;
    sizeFrames_ -= frameCount;
}

WasapiOutputStateMachine::WasapiOutputStateMachine(
    WasapiOutputConfig config,
    WasapiOutputBackend& backend,
    WasapiPcmQueue& queue,
    WasapiOutputCheckpoints* checkpoints
) : config_(config), backend_(backend), queue_(queue), checkpoints_(checkpoints) {
    if (config.bufferFrames == 0 || !isValidPcmFormat(config.pcmFormat)
        || config.clockFrequency == 0 || config.generation == 0
        || queue.format_ != config.pcmFormat) {
        throw std::invalid_argument("invalid WASAPI output configuration");
    }
}

WasapiOutputStateMachine::~WasapiOutputStateMachine() noexcept {
    if (state_ != WasapiOutputState::closed) {
        close();
    }
}

WasapiOutputReceipt WasapiOutputStateMachine::receipt(
    WasapiOutputOperation operation
) const {
    WasapiOutputReceipt value;
    value.operation = operation;
    value.previousState = state_;
    value.currentState = state_;
    value.generation = config_.generation;
    value.bufferFrames = config_.bufferFrames;
    value.underrunEventsTotal = underrunEvents_;
    return value;
}

WasapiOutputReceipt WasapiOutputStateMachine::fail(
    WasapiOutputReceipt value,
    WasapiOutputStage stage,
    HRESULT result
) {
    value.stage = stage;
    value.hresult = result;
    if (isWasapiOutputInvalidation(result)) {
        if (config_.generation != std::numeric_limits<std::uint64_t>::max()) {
            ++config_.generation;
        }
        queue_.clear();
        clientRunning_ = false;
        state_ = WasapiOutputState::invalidated;
        value.outcome = WasapiOutputOutcome::invalidated;
    } else {
        state_ = WasapiOutputState::failed;
        value.outcome = WasapiOutputOutcome::failed;
    }
    if (clientRunning_ && stage != WasapiOutputStage::stopClient) {
        backend_.stop();
    }
    clientRunning_ = false;
    if (!backendClosed_) {
        backend_.close();
        backendClosed_ = true;
    }
    value.currentState = state_;
    value.generation = config_.generation;
    value.underrunEventsTotal = underrunEvents_;
    return value;
}

void WasapiOutputStateMachine::checkpoint(WasapiOutputCheckpoint value) const noexcept {
    if (checkpoints_ != nullptr) {
        checkpoints_->arrive(value);
    }
}

WasapiOutputReceipt WasapiOutputStateMachine::fillAvailable(
    WasapiOutputOperation operation,
    bool startup,
    std::stop_token stopToken
) {
    auto value = receipt(operation);
    if (stopToken.stop_requested()) {
        value.outcome = WasapiOutputOutcome::cancelled;
        value.hresult = cancelledResult;
        return value;
    }

    HRESULT result = backend_.loadCurrentPadding(value.paddingFrames);
    if (FAILED(result)) {
        return fail(value, WasapiOutputStage::currentPadding, result);
    }
    checkpoint(WasapiOutputCheckpoint::afterPadding);
    if (stopToken.stop_requested()) {
        value.outcome = WasapiOutputOutcome::cancelled;
        value.stage = WasapiOutputStage::currentPadding;
        value.hresult = cancelledResult;
        return value;
    }
    if (value.paddingFrames > config_.bufferFrames) {
        return fail(value, WasapiOutputStage::paddingInvariant, E_UNEXPECTED);
    }
    value.availableFrames = config_.bufferFrames - value.paddingFrames;
    const bool sourceEnded = queue_.endOfStream();
    if (sourceEnded && queue_.availableFrames() == 0) {
        if (value.paddingFrames != 0) {
            value.outcome = WasapiOutputOutcome::noOp;
            return value;
        }
        if (clientRunning_) {
            result = backend_.stop();
            checkpoint(WasapiOutputCheckpoint::afterStop);
            if (FAILED(result)) {
                return fail(value, WasapiOutputStage::stopClient, result);
            }
            clientRunning_ = false;
            value.stage = WasapiOutputStage::stopClient;
        }
        state_ = WasapiOutputState::completed;
        value.currentState = state_;
        value.outcome = WasapiOutputOutcome::changed;
        value.hresult = S_OK;
        return value;
    }
    value.requestedFrames = sourceEnded
        ? std::min(value.availableFrames, queue_.availableFrames())
        : value.availableFrames;
    if (value.requestedFrames == 0) {
        if (startup) {
            state_ = WasapiOutputState::primed;
            value.currentState = state_;
        }
        value.outcome = WasapiOutputOutcome::noOp;
        return value;
    }

    std::byte* output = nullptr;
    result = backend_.acquireBuffer(value.requestedFrames, output);
    if (FAILED(result)) {
        return fail(value, WasapiOutputStage::acquireBuffer, result);
    }
    if (output == nullptr) {
        const HRESULT abandonResult = backend_.releaseBuffer(0, 0);
        return fail(
            value,
            WasapiOutputStage::acquireBuffer,
            FAILED(abandonResult) ? abandonResult : E_POINTER
        );
    }
    checkpoint(WasapiOutputCheckpoint::afterAcquire);
    if (stopToken.stop_requested()) {
        result = backend_.releaseBuffer(0, 0);
        if (FAILED(result)) {
            return fail(value, WasapiOutputStage::releaseBuffer, result);
        }
        value.outcome = WasapiOutputOutcome::cancelled;
        value.stage = WasapiOutputStage::acquireBuffer;
        value.hresult = cancelledResult;
        return value;
    }

    value.mediaFrames = std::min(queue_.availableFrames(), value.requestedFrames);
    value.silenceFrames = value.requestedFrames - value.mediaFrames;
    if (value.mediaFrames > 0) {
        queue_.copyFrames(output, value.mediaFrames);
    }
    if (value.mediaFrames > 0 && value.silenceFrames > 0) {
        const std::size_t mediaBytes = static_cast<std::size_t>(value.mediaFrames)
            * config_.pcmFormat.blockAlign;
        const std::size_t silenceBytes = static_cast<std::size_t>(value.silenceFrames)
            * config_.pcmFormat.blockAlign;
        std::memset(output + mediaBytes, 0, silenceBytes);
    }
    checkpoint(WasapiOutputCheckpoint::afterCopy);
    if (stopToken.stop_requested()) {
        result = backend_.releaseBuffer(0, 0);
        if (FAILED(result)) {
            return fail(value, WasapiOutputStage::releaseBuffer, result);
        }
        value.outcome = WasapiOutputOutcome::cancelled;
        value.stage = WasapiOutputStage::copyBuffer;
        value.hresult = cancelledResult;
        return value;
    }

    const DWORD flags = value.mediaFrames == 0
        ? static_cast<DWORD>(AUDCLNT_BUFFERFLAGS_SILENT)
        : DWORD{0};
    result = backend_.releaseBuffer(value.requestedFrames, flags);
    checkpoint(WasapiOutputCheckpoint::afterRelease);
    if (FAILED(result)) {
        return fail(value, WasapiOutputStage::releaseBuffer, result);
    }
    queue_.commitFrames(value.mediaFrames);
    value.releasedFrames = value.requestedFrames;
    if (!startup && !queue_.endOfStream()
        && value.mediaFrames < value.requestedFrames) {
        ++underrunEvents_;
        value.underrunEventsDelta = 1;
    }
    if (startup) {
        state_ = WasapiOutputState::primed;
    }
    value.outcome = WasapiOutputOutcome::changed;
    value.currentState = state_;
    value.stage = WasapiOutputStage::releaseBuffer;
    value.hresult = S_OK;
    value.underrunEventsTotal = underrunEvents_;
    value.lateCancellation = stopToken.stop_requested();
    return value;
}

WasapiOutputReceipt WasapiOutputStateMachine::start(std::stop_token stopToken) {
    auto value = receipt(WasapiOutputOperation::start);
    if (stopToken.stop_requested()) {
        value.outcome = WasapiOutputOutcome::cancelled;
        value.hresult = cancelledResult;
        return value;
    }
    if (state_ == WasapiOutputState::running) {
        value.outcome = WasapiOutputOutcome::noOp;
        return value;
    }
    if (state_ == WasapiOutputState::ready) {
        value = fillAvailable(WasapiOutputOperation::start, true, stopToken);
        if (value.outcome != WasapiOutputOutcome::changed
            && value.outcome != WasapiOutputOutcome::noOp) {
            return value;
        }
        if (value.lateCancellation) {
            return value;
        }
        if (state_ == WasapiOutputState::completed) {
            return value;
        }
    } else if (state_ != WasapiOutputState::primed
        && state_ != WasapiOutputState::stopped) {
        value.outcome = WasapiOutputOutcome::refused;
        value.hresult = E_ILLEGAL_METHOD_CALL;
        return value;
    }

    HRESULT result = backend_.start();
    checkpoint(WasapiOutputCheckpoint::afterStart);
    if (FAILED(result)) {
        return fail(value, WasapiOutputStage::startClient, result);
    }
    state_ = WasapiOutputState::running;
    clientRunning_ = true;
    value.outcome = WasapiOutputOutcome::changed;
    value.currentState = state_;
    value.stage = WasapiOutputStage::startClient;
    value.hresult = S_OK;
    value.lateCancellation = stopToken.stop_requested();
    if (value.lateCancellation) {
        return value;
    }
    WasapiClockReading reading;
    result = backend_.loadClockPosition(reading);
    if (FAILED(result)) {
        return fail(value, WasapiOutputStage::clockPosition, result);
    }
    value.stage = WasapiOutputStage::clockPosition;
    value.hresult = result;
    value.hasClockSample = true;
    value.clockSample = {
        config_.generation,
        reading.devicePosition,
        reading.qpc100Nanoseconds,
        reading.precisionDegraded || result == S_FALSE,
    };
    return value;
}

WasapiOutputReceipt WasapiOutputStateMachine::renderOnce(
    std::uint32_t timeoutMilliseconds,
    std::stop_token stopToken
) {
    auto value = receipt(WasapiOutputOperation::render);
    if (state_ != WasapiOutputState::running) {
        value.outcome = WasapiOutputOutcome::refused;
        value.hresult = E_ILLEGAL_METHOD_CALL;
        return value;
    }
    if (stopToken.stop_requested()) {
        value.outcome = WasapiOutputOutcome::cancelled;
        value.hresult = cancelledResult;
        return value;
    }
    const HRESULT waitResult = backend_.waitForRenderEvent(stopToken, timeoutMilliseconds);
    if (waitResult == cancelledResult || stopToken.stop_requested()) {
        value.outcome = WasapiOutputOutcome::cancelled;
        value.stage = WasapiOutputStage::waitForEvent;
        value.hresult = cancelledResult;
        return value;
    }
    if (FAILED(waitResult)) {
        return fail(value, WasapiOutputStage::waitForEvent, waitResult);
    }
    value = fillAvailable(WasapiOutputOperation::render, false, stopToken);
    if (value.outcome != WasapiOutputOutcome::changed
        && value.outcome != WasapiOutputOutcome::noOp) {
        return value;
    }
    if (value.lateCancellation) {
        return value;
    }
    if (state_ == WasapiOutputState::completed) {
        return value;
    }

    WasapiClockReading reading;
    const HRESULT clockResult = backend_.loadClockPosition(reading);
    if (FAILED(clockResult)) {
        return fail(value, WasapiOutputStage::clockPosition, clockResult);
    }
    value.stage = WasapiOutputStage::clockPosition;
    value.hresult = clockResult;
    value.hasClockSample = true;
    value.clockSample = {
        config_.generation,
        reading.devicePosition,
        reading.qpc100Nanoseconds,
        reading.precisionDegraded || clockResult == S_FALSE,
    };
    return value;
}

WasapiOutputReceipt WasapiOutputStateMachine::pause(std::stop_token stopToken) {
    auto value = receipt(WasapiOutputOperation::pause);
    if (stopToken.stop_requested()) {
        value.outcome = WasapiOutputOutcome::cancelled;
        value.hresult = cancelledResult;
        return value;
    }
    if (state_ == WasapiOutputState::stopped
        || state_ == WasapiOutputState::completed) {
        value.outcome = WasapiOutputOutcome::noOp;
        return value;
    }
    if (state_ != WasapiOutputState::running) {
        value.outcome = WasapiOutputOutcome::refused;
        value.hresult = E_ILLEGAL_METHOD_CALL;
        return value;
    }
    const HRESULT result = backend_.stop();
    checkpoint(WasapiOutputCheckpoint::afterStop);
    if (FAILED(result)) {
        return fail(value, WasapiOutputStage::stopClient, result);
    }
    state_ = WasapiOutputState::stopped;
    clientRunning_ = false;
    value.outcome = WasapiOutputOutcome::changed;
    value.currentState = state_;
    value.stage = WasapiOutputStage::stopClient;
    value.lateCancellation = stopToken.stop_requested();
    return value;
}

WasapiOutputReceipt WasapiOutputStateMachine::reset(std::stop_token stopToken) {
    auto value = receipt(WasapiOutputOperation::reset);
    if (stopToken.stop_requested()) {
        value.outcome = WasapiOutputOutcome::cancelled;
        value.hresult = cancelledResult;
        return value;
    }
    if (state_ == WasapiOutputState::ready) {
        value.outcome = WasapiOutputOutcome::noOp;
        return value;
    }
    if (state_ != WasapiOutputState::stopped
        && state_ != WasapiOutputState::primed
        && state_ != WasapiOutputState::completed) {
        value.outcome = WasapiOutputOutcome::refused;
        value.hresult = E_ILLEGAL_METHOD_CALL;
        return value;
    }
    if (config_.generation == std::numeric_limits<std::uint64_t>::max()) {
        return fail(value, WasapiOutputStage::resetClient, E_UNEXPECTED);
    }
    const HRESULT result = backend_.reset();
    checkpoint(WasapiOutputCheckpoint::afterReset);
    if (FAILED(result)) {
        return fail(value, WasapiOutputStage::resetClient, result);
    }
    ++config_.generation;
    queue_.clear();
    state_ = WasapiOutputState::ready;
    value.outcome = WasapiOutputOutcome::changed;
    value.currentState = state_;
    value.stage = WasapiOutputStage::resetClient;
    value.generation = config_.generation;
    value.lateCancellation = stopToken.stop_requested();
    return value;
}

WasapiOutputReceipt WasapiOutputStateMachine::close() noexcept {
    auto value = receipt(WasapiOutputOperation::close);
    if (state_ == WasapiOutputState::closed) {
        value.outcome = WasapiOutputOutcome::noOp;
        return value;
    }

    HRESULT firstFailure = S_OK;
    WasapiOutputStage failureStage = WasapiOutputStage::none;
    if (clientRunning_) {
        const HRESULT stopResult = backend_.stop();
        if (FAILED(stopResult)) {
            firstFailure = stopResult;
            failureStage = WasapiOutputStage::stopClient;
        } else {
            clientRunning_ = false;
        }
    }
    const HRESULT closeResult = backendClosed_ ? S_OK : backend_.close();
    backendClosed_ = true;
    clientRunning_ = false;
    if (SUCCEEDED(firstFailure) && FAILED(closeResult)) {
        firstFailure = closeResult;
        failureStage = WasapiOutputStage::closeBackend;
    }
    queue_.clear();
    state_ = WasapiOutputState::closed;
    value.currentState = state_;
    value.stage = failureStage;
    value.hresult = firstFailure;
    value.outcome = FAILED(firstFailure)
        ? (isWasapiOutputInvalidation(firstFailure)
            ? WasapiOutputOutcome::invalidated
            : WasapiOutputOutcome::failed)
        : WasapiOutputOutcome::changed;
    return value;
}

WasapiOutputState WasapiOutputStateMachine::state() const {
    return state_;
}

std::uint64_t WasapiOutputStateMachine::generation() const {
    return config_.generation;
}

bool isWasapiOutputInvalidation(HRESULT result) {
    return result == AUDCLNT_E_DEVICE_INVALIDATED
        || result == AUDCLNT_E_RESOURCES_INVALIDATED
        || result == AUDCLNT_E_SERVICE_NOT_RUNNING;
}

}
