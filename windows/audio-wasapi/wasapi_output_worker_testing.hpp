#pragma once

#include "palmier/audio/wasapi_output_worker.hpp"
#include "wasapi_environment_session.hpp"
#include "wasapi_output_backend.hpp"

namespace palmier::audio {

class WasapiOutputWorkerStream :
    public WasapiEnvironmentSession,
    public WasapiOutputBackend {
};

}
