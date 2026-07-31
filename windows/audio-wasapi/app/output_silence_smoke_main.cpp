#include "palmier/audio/wasapi_output.hpp"

#include <iostream>

int main() {
    const auto result = palmier::audio::probeDefaultWasapiSilentOutput();
    std::cout << palmier::audio::wasapiSilentOutputJson(result) << '\n';
    return result.status == palmier::audio::WasapiSilentOutputStatus::failed ? 1 : 0;
}
