#include "palmier/audio/wasapi_output.hpp"

#include "palmier/audio/wasapi_environment_probe.hpp"
#include "wasapi_native_stream.hpp"
#include "wasapi_output_backend.hpp"

#include <exception>
#include <iomanip>
#include <new>
#include <optional>
#include <sstream>
#include <system_error>
#include <thread>
#include <utility>

namespace palmier::audio {
namespace {

std::string stageName(WasapiOutputStage stage) {
    switch (stage) {
    case WasapiOutputStage::none: return "none";
    case WasapiOutputStage::waitForEvent: return "wait-for-event";
    case WasapiOutputStage::currentPadding: return "current-padding";
    case WasapiOutputStage::paddingInvariant: return "padding-invariant";
    case WasapiOutputStage::acquireBuffer: return "acquire-buffer";
    case WasapiOutputStage::copyBuffer: return "copy-buffer";
    case WasapiOutputStage::releaseBuffer: return "release-buffer";
    case WasapiOutputStage::startClient: return "start-client";
    case WasapiOutputStage::clockPosition: return "clock-position";
    case WasapiOutputStage::stopClient: return "stop-client";
    case WasapiOutputStage::resetClient: return "reset-client";
    case WasapiOutputStage::generationInvariant: return "generation-invariant";
    case WasapiOutputStage::closeBackend: return "close-backend";
    }
    return "none";
}

std::string statusName(WasapiSilentOutputStatus status) {
    switch (status) {
    case WasapiSilentOutputStatus::available: return "available";
    case WasapiSilentOutputStatus::unavailable: return "unavailable";
    case WasapiSilentOutputStatus::failed: return "failed";
    }
    return "failed";
}

WasapiSilentOutputResult terminalResult(
    const WasapiOutputReceipt& receipt,
    std::uint32_t primedFrames,
    std::uint32_t eventFrames
) {
    return {
        receipt.outcome == WasapiOutputOutcome::invalidated
            ? WasapiSilentOutputStatus::unavailable
            : WasapiSilentOutputStatus::failed,
        stageName(receipt.stage),
        receipt.hresult,
        receipt.generation,
        primedFrames,
        eventFrames,
        receipt.hasClockSample,
    };
}

WasapiSilentOutputResult runSilentCycle() {
    WasapiNativeStream stream;
    const auto environment = runWasapiEnvironmentProbe(stream);
    if (environment.status != WasapiProbeStatus::available) {
        return {
            environment.status == WasapiProbeStatus::unavailable
                ? WasapiSilentOutputStatus::unavailable
                : WasapiSilentOutputStatus::failed,
            "setup-" + environment.stage,
            environment.hresult,
        };
    }

    WasapiPcmQueue queue(environment.bufferFrames, environment.pcmFormat);
    WasapiOutputStateMachine machine(
        {
            environment.bufferFrames,
            environment.pcmFormat,
            environment.clockFrequency,
            1,
        },
        stream,
        queue
    );

    const auto started = machine.start();
    const std::uint32_t primedFrames = started.releasedFrames;
    if (started.outcome != WasapiOutputOutcome::changed) {
        machine.close();
        return terminalResult(started, primedFrames, 0);
    }
    const auto rendered = machine.renderOnce(2'000);
    const std::uint32_t eventFrames = rendered.releasedFrames;
    if (rendered.outcome != WasapiOutputOutcome::changed
        && rendered.outcome != WasapiOutputOutcome::noOp) {
        machine.close();
        return terminalResult(rendered, primedFrames, eventFrames);
    }
    const auto paused = machine.pause();
    if (paused.outcome != WasapiOutputOutcome::changed) {
        machine.close();
        return terminalResult(paused, primedFrames, eventFrames);
    }
    const auto reset = machine.installGeneration(1, 2);
    if (reset.outcome != WasapiOutputOutcome::changed) {
        machine.close();
        return terminalResult(reset, primedFrames, eventFrames);
    }
    const auto closed = machine.close();
    if (closed.outcome != WasapiOutputOutcome::changed) {
        return terminalResult(closed, primedFrames, eventFrames);
    }
    return {
        WasapiSilentOutputStatus::available,
        "completed-silent-cycle",
        S_OK,
        reset.generation,
        primedFrames,
        eventFrames,
        started.hasClockSample && rendered.hasClockSample,
    };
}

}

WasapiSilentOutputResult probeDefaultWasapiSilentOutput() {
    std::optional<WasapiSilentOutputResult> result;
    std::exception_ptr exception;
    try {
        {
            std::jthread worker([&result, &exception] {
                try {
                    result = runSilentCycle();
                } catch (...) {
                    exception = std::current_exception();
                }
            });
        }
        if (exception) {
            std::rethrow_exception(exception);
        }
    } catch (const std::bad_alloc&) {
        return {WasapiSilentOutputStatus::failed, "output-thread", E_OUTOFMEMORY};
    } catch (const std::system_error&) {
        return {WasapiSilentOutputStatus::failed, "output-thread", E_FAIL};
    } catch (...) {
        return {WasapiSilentOutputStatus::failed, "output-thread", E_UNEXPECTED};
    }
    if (!result.has_value()) {
        return {WasapiSilentOutputStatus::failed, "output-thread", E_UNEXPECTED};
    }
    return std::move(*result);
}

std::string wasapiSilentOutputJson(const WasapiSilentOutputResult& result) {
    std::ostringstream output;
    output << "{\"status\":\"" << statusName(result.status)
           << "\",\"stage\":\"" << result.stage
           << "\",\"hresult\":\"0x"
           << std::hex << std::uppercase << std::setw(8) << std::setfill('0')
           << static_cast<std::uint32_t>(result.hresult)
           << std::dec << "\",\"generation\":" << result.generation
           << ",\"primedFrames\":" << result.primedFrames
           << ",\"eventFrames\":" << result.eventFrames
           << ",\"clockSampled\":"
           << (result.clockSampled ? "true" : "false") << '}';
    return output.str();
}

}
