#include "palmier/audio/wasapi_environment_probe.hpp"

#include <iostream>

int main() {
    const auto result = palmier::audio::probeDefaultWasapiRenderEndpoint();
    std::cout << palmier::audio::wasapiProbeJson(result) << '\n';
    return result.status == palmier::audio::WasapiProbeStatus::failed ? 1 : 0;
}
