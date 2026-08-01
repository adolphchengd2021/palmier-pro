#pragma once

#include "palmier/audio/wasapi_output.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stop_token>
#include <vector>

namespace palmier::audio {

class WasapiOutputWorkerStream;

using WasapiOutputWorkerStreamFactory =
    std::function<std::unique_ptr<WasapiOutputWorkerStream>()>;

enum class WasapiWorkerConfigurationOutcome {
    available,
    cancelled,
    unavailable,
    failed,
    closed,
};

struct WasapiWorkerConfiguration final {
    WasapiWorkerConfigurationOutcome outcome{
        WasapiWorkerConfigurationOutcome::failed
    };
    HRESULT hresult{S_OK};
    std::uint64_t generation{};
    std::uint32_t bufferFrames{};
    std::uint64_t clockFrequency{};
    PcmFormat pcmFormat;
};

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

enum class WasapiWorkerClockOutcome {
    available,
    noSample,
    refused,
    unavailable,
    failed,
    closed,
};

struct WasapiWorkerClockReceipt final {
    WasapiWorkerClockOutcome outcome{WasapiWorkerClockOutcome::failed};
    HRESULT hresult{S_OK};
    std::uint64_t generation{};
    bool hasSample{};
    AudioClockSample sample;
};

class WasapiOutputWorker final {
public:
    WasapiOutputWorker();
    explicit WasapiOutputWorker(WasapiOutputWorkerStreamFactory factory);
    ~WasapiOutputWorker();

    WasapiOutputWorker(const WasapiOutputWorker&) = delete;
    WasapiOutputWorker& operator=(const WasapiOutputWorker&) = delete;

    WasapiWorkerConfiguration configuration(std::stop_token stopToken = {});
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
    WasapiWorkerClockReceipt clockPosition(
        std::uint64_t expectedGeneration
    ) const;
    WasapiOutputReceipt close();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}
