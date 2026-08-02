#pragma once

#include "palmier/project/project_session.hpp"

#include <cstdint>
#include <functional>
#include <string>

namespace palmier::mcp {

struct HttpServerOptions final {
    std::uint16_t port = 19789;
    bool exitAfterLastSession = false;
};

int runHttpServer(
    palmier::project::ProjectSession& projectSession,
    const HttpServerOptions& options,
    const std::function<std::string()>& sessionIdGenerator
);

}
