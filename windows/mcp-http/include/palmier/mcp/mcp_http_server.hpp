#pragma once

#include "palmier/project/project_runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace palmier::mcp {

struct HttpServerOptions final {
    std::uint16_t port = 19789;
    bool exitAfterLastSession = false;
};

enum class HttpServerState {
    idle,
    starting,
    ready,
    failed,
    stopped,
};

struct HttpServerStatus final {
    HttpServerState state = HttpServerState::idle;
    std::uint16_t port = 0;
    std::string error;
};

class HttpServerObserver {
public:
    virtual ~HttpServerObserver() = default;
    virtual void clientAdmitted() noexcept = 0;
    virtual void clientReceiveWaiting(std::size_t) noexcept {}
};

class HttpServerService final {
public:
    HttpServerService(
        palmier::project::ProjectRuntime& projectRuntime,
        HttpServerOptions options,
        std::function<std::string()> sessionIdGenerator,
        std::shared_ptr<HttpServerObserver> observer = {}
    );
    ~HttpServerService();

    HttpServerService(const HttpServerService&) = delete;
    HttpServerService& operator=(const HttpServerService&) = delete;

    void start();
    void requestStop() noexcept;
    void join() noexcept;
    HttpServerStatus status() const;
    HttpServerStatus waitForReadyOrTerminal();

private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
};

int runHttpServer(
    palmier::project::ProjectRuntime& projectRuntime,
    const HttpServerOptions& options,
    const std::function<std::string()>& sessionIdGenerator
);

}
