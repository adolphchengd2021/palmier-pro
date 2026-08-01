#pragma once

#include "wasapi_environment_session.hpp"
#include "wasapi_output_backend.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stop_token>
#include <vector>

namespace palmier::audio {

class WasapiOutputWorkerStream :
    public WasapiEnvironmentSession,
    public WasapiOutputBackend {
};

using WasapiOutputWorkerStreamFactory =
    std::function<std::unique_ptr<WasapiOutputWorkerStream>()>;

struct WasapiWorkerPcmBlock final {
    std::uint64_t generation{};
    std::uint64_t startOutputSample{};
    std::uint32_t frameCount{};
    PcmFormat pcmFormat;
    std::vector<std::byte> bytes;
};

enum class WasapiWorkerPcmOutcome {
    accepted,
    noOp,
    cancelled,
    refused,
    failed,
};

enum class WasapiWorkerPcmStage {
    none,
    generationInvariant,
    formatInvariant,
    sampleInvariant,
    capacityInvariant,
    closed,
};

struct WasapiWorkerPcmReceipt final {
    WasapiWorkerPcmOutcome outcome{WasapiWorkerPcmOutcome::failed};
    WasapiWorkerPcmStage stage{WasapiWorkerPcmStage::none};
    HRESULT hresult{S_OK};
    std::uint64_t generation{};
    std::uint64_t nextOutputSample{};
    std::uint32_t acceptedFrames{};
    bool endOfStream{};
};

class WasapiOutputWorker final {
public:
    WasapiOutputWorker();
    explicit WasapiOutputWorker(WasapiOutputWorkerStreamFactory factory);
    ~WasapiOutputWorker();

    WasapiOutputWorker(const WasapiOutputWorker&) = delete;
    WasapiOutputWorker& operator=(const WasapiOutputWorker&) = delete;

    WasapiOutputReceipt start(std::uint64_t expectedGeneration);
    WasapiOutputReceipt pause(std::uint64_t expectedGeneration);
    WasapiOutputReceipt discardGeneration(std::uint64_t expectedGeneration);
    WasapiOutputReceipt installGeneration(
        std::uint64_t expectedGeneration,
        std::uint64_t nextGeneration
    );
    WasapiWorkerPcmReceipt submit(
        WasapiWorkerPcmBlock block,
        std::stop_token stopToken = {}
    );
    WasapiWorkerPcmReceipt markEndOfStream(
        std::uint64_t generation,
        std::uint64_t finalOutputSample,
        std::stop_token stopToken = {}
    );
    WasapiOutputReceipt waitForTerminal(
        std::uint64_t generation,
        std::stop_token stopToken = {}
    );
    WasapiOutputReceipt close();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}
