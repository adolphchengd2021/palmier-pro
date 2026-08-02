#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <WinSock2.h>
#include <WS2tcpip.h>

#include "palmier/mcp/mcp_http_server.hpp"
#include "palmier/project/project_reader.hpp"
#include "mcp_http_server_testing.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

class SocketSystem final {
public:
    SocketSystem() {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            throw std::runtime_error("test WSAStartup failed");
        }
    }

    ~SocketSystem() { WSACleanup(); }

    SocketSystem(const SocketSystem&) = delete;
    SocketSystem& operator=(const SocketSystem&) = delete;
};

class Socket final {
public:
    explicit Socket(SOCKET value) : value_(value) {}
    ~Socket() {
        if (value_ != INVALID_SOCKET) closesocket(value_);
    }

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    SOCKET get() const noexcept { return value_; }

private:
    SOCKET value_;
};

class AdmissionObserver final : public palmier::mcp::HttpServerObserver {
public:
    void clientAdmitted() noexcept override {}

    void clientReceiveWaiting(std::size_t) noexcept override {
        std::scoped_lock lock(mutex_);
        ++receiveWaitCount_;
        condition_.notify_all();
    }

    void waitForReceiveWaits(std::size_t count) {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [this, count] { return receiveWaitCount_ >= count; });
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    std::size_t receiveWaitCount_{};
};

void sendAll(SOCKET socketValue, std::string_view bytes) {
    std::size_t sent{};
    while (sent < bytes.size()) {
        const auto result = send(
            socketValue,
            bytes.data() + sent,
            static_cast<int>(bytes.size() - sent),
            0
        );
        require(result > 0, "partial client send");
        sent += static_cast<std::size_t>(result);
    }
}

void installProject(palmier::project::ProjectRuntime& runtime) {
    auto document = palmier::project::readProject(R"({
        "timelines":[{
            "id":"timeline","name":"Project","fps":30,"width":1920,"height":1080,
            "tracks":[]
        }],
        "activeTimelineId":"timeline","openTimelineIds":["timeline"]
    })", [] { return std::string("reader-id"); });
    static_cast<void>(runtime.install(
        std::move(document),
        1,
        [] { return std::string("runtime-id"); }
    ));
}

void socketTimeoutTracksRemainingDeadline() {
    using namespace std::chrono_literals;
    const auto now = std::chrono::steady_clock::time_point(10s);
    require(
        palmier::mcp::testing::socketTimeoutMilliseconds(now, now + 2s) == 1500,
        "socket timeout must retain the per-call ceiling"
    );
    require(
        palmier::mcp::testing::socketTimeoutMilliseconds(now, now + 750ms) == 750,
        "socket timeout must shrink to the remaining deadline"
    );
    require(
        palmier::mcp::testing::socketTimeoutMilliseconds(now, now + 500us) == 1,
        "positive sub-millisecond deadline must retain one millisecond"
    );
    require(
        palmier::mcp::testing::socketTimeoutMilliseconds(now, now) == 0,
        "expired deadline must not enter a socket operation"
    );
}

void stopUnblocksAcceptAndReleasesPort() {
    SocketSystem socketSystem;
    palmier::project::ProjectRuntime runtime;
    installProject(runtime);
    auto observer = std::make_shared<AdmissionObserver>();
    palmier::mcp::HttpServerOptions ephemeralOptions;
    ephemeralOptions.port = 0;
    palmier::mcp::HttpServerService first(
        runtime,
        ephemeralOptions,
        [] { return std::string("session-one"); },
        observer
    );
    first.start();
    const auto firstReady = first.waitForReadyOrTerminal();
    require(firstReady.state == palmier::mcp::HttpServerState::ready, firstReady.error);
    require(firstReady.port != 0, "ephemeral listener must publish its bound port");

    palmier::mcp::HttpServerOptions occupiedOptions;
    occupiedOptions.port = firstReady.port;
    palmier::mcp::HttpServerService contender(
        runtime,
        occupiedOptions,
        [] { return std::string("session-contender"); }
    );
    contender.start();
    const auto refused = contender.waitForReadyOrTerminal();
    require(
        refused.state == palmier::mcp::HttpServerState::failed && !refused.error.empty(),
        "exclusive bind failure must publish a terminal error"
    );
    contender.join();

    {
        Socket partial(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        require(partial.get() != INVALID_SOCKET, "partial client socket");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(firstReady.port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(
            connect(
                partial.get(),
                reinterpret_cast<const sockaddr*>(&address),
                static_cast<int>(sizeof(address))
            ) != SOCKET_ERROR,
            "partial client connect"
        );
        constexpr std::string_view partialHeader =
            "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\n";
        sendAll(partial.get(), partialHeader);
        observer->waitForReceiveWaits(2);
        first.requestStop();
        first.join();
    }
    require(
        first.status().state == palmier::mcp::HttpServerState::stopped,
        "stop must bound an admitted partial request"
    );

    palmier::mcp::HttpServerOptions reboundOptions;
    reboundOptions.port = firstReady.port;
    palmier::mcp::HttpServerService second(
        runtime,
        reboundOptions,
        [] { return std::string("session-two"); }
    );
    second.start();
    const auto secondReady = second.waitForReadyOrTerminal();
    require(secondReady.state == palmier::mcp::HttpServerState::ready, secondReady.error);
    require(secondReady.port == firstReady.port, "released listener port must bind again");
    second.requestStop();
    second.join();
    require(
        second.status().state == palmier::mcp::HttpServerState::stopped,
        "rebound listener must stop cleanly"
    );
}

}

int main() {
    try {
        socketTimeoutTracksRemainingDeadline();
        stopUnblocksAcceptAndReleasesPort();
        std::cout << "PALMIER_MCP_HTTP_SERVICE_TESTS_OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "PALMIER_MCP_HTTP_SERVICE_TESTS_FAILED " << error.what() << '\n';
        return 1;
    }
}
