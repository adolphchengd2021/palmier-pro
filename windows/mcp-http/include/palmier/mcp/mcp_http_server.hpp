#pragma once

#include "palmier/project/project_runtime.hpp"

#include <cstdint>
#include <functional>
#include <string>

namespace palmier::mcp {

struct HttpServerOptions final {
    std::uint16_t port = 19789;
    bool exitAfterLastSession = false;
};

int runHttpServer(
    palmier::project::ProjectRuntime& projectRuntime,
    const HttpServerOptions& options,
    const std::function<std::string()>& sessionIdGenerator
);

}
