#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "palmier/mcp/mcp_http_server.hpp"
#include "mcp_http_server_testing.hpp"

#include "palmier/json/json_document.hpp"

#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace palmier::mcp::testing {

std::uint32_t socketTimeoutMilliseconds(
    std::chrono::steady_clock::time_point now,
    std::chrono::steady_clock::time_point deadline
) noexcept {
    if (now >= deadline) return 0;
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - now
    ).count();
    if (remaining <= 0) return 1;
    return static_cast<std::uint32_t>(std::min<std::int64_t>(remaining, 1500));
}

}

namespace palmier::mcp {
namespace {

using palmier::json::Array;
using palmier::json::Number;
using palmier::json::Object;
using palmier::json::Value;

constexpr std::string_view protocolVersion = "2025-06-18";
constexpr std::size_t maximumHeaderBytes = 64 * 1024;
constexpr std::size_t maximumBodyBytes = 16 * 1024 * 1024;
constexpr std::size_t maximumSessions = 32;
constexpr auto sessionIdleTimeout = std::chrono::minutes(5);
constexpr auto maximumRequestLifetime = std::chrono::seconds(5);

class SocketSystem final {
public:
    SocketSystem() {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            throw std::runtime_error("WSAStartup failed");
        }
    }

    ~SocketSystem() { WSACleanup(); }

    SocketSystem(const SocketSystem&) = delete;
    SocketSystem& operator=(const SocketSystem&) = delete;
};

class Socket final {
public:
    explicit Socket(SOCKET value = INVALID_SOCKET) : value_(value) {}
    ~Socket() {
        if (value_ != INVALID_SOCKET) {
            closesocket(value_);
        }
    }

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept : value_(std::exchange(other.value_, INVALID_SOCKET)) {}
    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) {
            if (value_ != INVALID_SOCKET) {
                closesocket(value_);
            }
            value_ = std::exchange(other.value_, INVALID_SOCKET);
        }
        return *this;
    }

    SOCKET get() const noexcept { return value_; }

private:
    SOCKET value_;
};

class UniqueHandle final {
public:
    explicit UniqueHandle(HANDLE value = nullptr) : value_(value) {}
    ~UniqueHandle() {
        if (value_ != nullptr) CloseHandle(value_);
    }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    HANDLE get() const noexcept { return value_; }

private:
    HANDLE value_;
};

class WinsockEvent final {
public:
    explicit WinsockEvent(WSAEVENT value = WSA_INVALID_EVENT) : value_(value) {}
    ~WinsockEvent() {
        if (value_ != WSA_INVALID_EVENT) WSACloseEvent(value_);
    }

    WinsockEvent(const WinsockEvent&) = delete;
    WinsockEvent& operator=(const WinsockEvent&) = delete;

    WSAEVENT get() const noexcept { return value_; }

private:
    WSAEVENT value_;
};

struct HttpRequest final {
    std::string method;
    std::string path;
    std::map<std::string, std::string, std::less<>> headers;
    std::string body;
};

struct HttpResponse final {
    int status = 200;
    std::string reason = "OK";
    std::map<std::string, std::string, std::less<>> headers;
    std::string body;
};

struct SessionState final {
    bool initialized;
    std::chrono::steady_clock::time_point lastActivity;
    std::uint64_t projectGeneration;
};

class HttpError final : public std::runtime_error {
public:
    HttpError(int statusValue, std::string reasonValue, std::string detail)
        : std::runtime_error(std::move(detail)),
          status(statusValue),
          reason(std::move(reasonValue)) {}

    const int status;
    const std::string reason;
};

class RequestCancelled final : public std::exception {};

void checkRequestBoundary(
    std::stop_token cancellation,
    std::chrono::steady_clock::time_point deadline
) {
    if (cancellation.stop_requested()) throw RequestCancelled();
    if (std::chrono::steady_clock::now() >= deadline) {
        throw HttpError(408, "Request Timeout", "HTTP request exceeded five seconds");
    }
}

void setSocketOperationTimeout(
    SOCKET socketValue,
    int option,
    std::stop_token cancellation,
    std::chrono::steady_clock::time_point deadline
) {
    checkRequestBoundary(cancellation, deadline);
    const DWORD timeoutMilliseconds = testing::socketTimeoutMilliseconds(
        std::chrono::steady_clock::now(),
        deadline
    );
    if (timeoutMilliseconds == 0) {
        throw HttpError(408, "Request Timeout", "HTTP request exceeded five seconds");
    }
    if (setsockopt(
        socketValue,
        SOL_SOCKET,
        option,
        reinterpret_cast<const char*>(&timeoutMilliseconds),
        static_cast<int>(sizeof(timeoutMilliseconds))
    ) == SOCKET_ERROR) {
        throw std::runtime_error("cannot bound MCP socket operation to its deadline");
    }
}

Number integerNumber(std::int64_t value) {
    return {std::to_string(value), value};
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::string trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t");
    return std::string(value.substr(first, last - first + 1));
}

std::size_t parseContentLength(std::string_view value) {
    std::size_t parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
        throw HttpError(400, "Bad Request", "invalid Content-Length");
    }
    if (parsed > maximumBodyBytes) {
        throw HttpError(413, "Content Too Large", "request body exceeds 16 MiB");
    }
    return parsed;
}

HttpRequest readRequest(
    SOCKET clientSocket,
    std::stop_token cancellation,
    std::chrono::steady_clock::time_point deadline,
    const std::shared_ptr<HttpServerObserver>& observer
) {
    std::string bytes;
    bytes.reserve(8192);
    std::array<char, 8192> buffer{};
    std::size_t headerEnd = std::string::npos;
    while (headerEnd == std::string::npos) {
        checkRequestBoundary(cancellation, deadline);
        if (observer) observer->clientReceiveWaiting(bytes.size());
        setSocketOperationTimeout(clientSocket, SO_RCVTIMEO, cancellation, deadline);
        const auto received = recv(clientSocket, buffer.data(), static_cast<int>(buffer.size()), 0);
        checkRequestBoundary(cancellation, deadline);
        if (received == 0) {
            throw HttpError(400, "Bad Request", "connection closed before HTTP headers");
        }
        if (received == SOCKET_ERROR) {
            throw HttpError(400, "Bad Request", "cannot receive HTTP headers");
        }
        bytes.append(buffer.data(), static_cast<std::size_t>(received));
        if (bytes.size() > maximumHeaderBytes) {
            throw HttpError(431, "Request Header Fields Too Large", "HTTP headers exceed 64 KiB");
        }
        headerEnd = bytes.find("\r\n\r\n");
    }

    HttpRequest request;
    const auto headerText = bytes.substr(0, headerEnd);
    std::istringstream lines(headerText);
    std::string line;
    if (!std::getline(lines, line)) {
        throw HttpError(400, "Bad Request", "missing HTTP request line");
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    std::istringstream requestLine(line);
    std::string version;
    std::string extra;
    if (
        !(requestLine >> request.method >> request.path >> version)
        || (requestLine >> extra)
        || version != "HTTP/1.1"
    ) {
        throw HttpError(400, "Bad Request", "invalid HTTP request line");
    }
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto separator = line.find(':');
        if (separator == std::string::npos) {
            throw HttpError(400, "Bad Request", "invalid HTTP header");
        }
        auto name = lower(trim(std::string_view(line).substr(0, separator)));
        auto value = trim(std::string_view(line).substr(separator + 1));
        if (name.empty() || request.headers.contains(name)) {
            throw HttpError(400, "Bad Request", "empty or duplicate HTTP header");
        }
        request.headers.emplace(std::move(name), std::move(value));
    }
    const auto contentLengthField = request.headers.find("content-length");
    const auto contentLength = contentLengthField == request.headers.end()
        ? std::size_t{0}
        : parseContentLength(contentLengthField->second);
    const auto bodyStart = headerEnd + 4;
    if (bytes.size() - bodyStart > contentLength) {
        throw HttpError(400, "Bad Request", "request contains bytes beyond Content-Length");
    }
    request.body.assign(bytes.data() + bodyStart, bytes.size() - bodyStart);
    while (request.body.size() < contentLength) {
        checkRequestBoundary(cancellation, deadline);
        if (observer) observer->clientReceiveWaiting(request.body.size());
        setSocketOperationTimeout(clientSocket, SO_RCVTIMEO, cancellation, deadline);
        const auto remaining = contentLength - request.body.size();
        const auto amount = static_cast<int>(std::min(remaining, buffer.size()));
        const auto received = recv(clientSocket, buffer.data(), amount, 0);
        checkRequestBoundary(cancellation, deadline);
        if (received <= 0) {
            throw HttpError(400, "Bad Request", "connection closed before request body");
        }
        request.body.append(buffer.data(), static_cast<std::size_t>(received));
    }
    checkRequestBoundary(cancellation, deadline);
    return request;
}

void sendAll(
    SOCKET clientSocket,
    std::string_view bytes,
    std::stop_token cancellation,
    std::chrono::steady_clock::time_point deadline
) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        checkRequestBoundary(cancellation, deadline);
        setSocketOperationTimeout(clientSocket, SO_SNDTIMEO, cancellation, deadline);
        const auto remaining = bytes.size() - sent;
        const auto chunk = static_cast<int>(std::min(
            remaining,
            static_cast<std::size_t>(std::numeric_limits<int>::max())
        ));
        const auto result = send(clientSocket, bytes.data() + sent, chunk, 0);
        checkRequestBoundary(cancellation, deadline);
        if (result == SOCKET_ERROR || result == 0) {
            throw std::runtime_error("cannot send HTTP response");
        }
        sent += static_cast<std::size_t>(result);
    }
}

void sendResponse(
    SOCKET clientSocket,
    HttpResponse response,
    std::stop_token cancellation,
    std::chrono::steady_clock::time_point deadline
) {
    response.headers.emplace("Connection", "close");
    response.headers.emplace("Content-Length", std::to_string(response.body.size()));
    if (!response.body.empty() && !response.headers.contains("Content-Type")) {
        response.headers.emplace("Content-Type", "application/json; charset=utf-8");
    }
    std::ostringstream output;
    output << "HTTP/1.1 " << response.status << ' ' << response.reason << "\r\n";
    for (const auto& [name, value] : response.headers) {
        output << name << ": " << value << "\r\n";
    }
    output << "\r\n" << response.body;
    sendAll(clientSocket, output.str(), cancellation, deadline);
}

const Value& requiredField(const Object& object, std::string_view key) {
    const auto field = object.find(std::string(key));
    if (field == object.end()) {
        throw palmier::project::CommandError("invalidArguments", "missing field: " + std::string(key));
    }
    return field->second;
}

const Object& requireObject(const Value& value, std::string_view label) {
    if (value.kind() != Value::Kind::object) {
        throw palmier::project::CommandError(
            "invalidArguments",
            std::string(label) + " must be an object"
        );
    }
    return value.object();
}

std::int64_t requireInteger(const Value& value, std::string_view label) {
    if (value.kind() != Value::Kind::number || !value.number().integer) {
        throw palmier::project::CommandError(
            "invalidArguments",
            std::string(label) + " must be an integer"
        );
    }
    return *value.number().integer;
}

std::string requireString(const Value& value, std::string_view label) {
    if (value.kind() != Value::Kind::string) {
        throw palmier::project::CommandError(
            "invalidArguments",
            std::string(label) + " must be a string"
        );
    }
    return value.string();
}

void rejectUnknownKeys(
    const Object& object,
    const std::set<std::string, std::less<>>& allowed,
    std::string_view label
) {
    for (const auto& [key, value] : object) {
        static_cast<void>(value);
        if (!allowed.contains(key)) {
            throw palmier::project::CommandError(
                "invalidArguments",
                std::string(label) + " contains unknown field: " + key
            );
        }
    }
}

Value toolsList() {
    static const Value value = palmier::json::parse(R"json({
        "tools": [
            {
                "name": "get_timeline",
                "description": "Reads the active timeline using stable IDs and integer project frames.",
                "inputSchema": {
                    "type": "object",
                    "properties": {
                        "startFrame": {"type": "integer"},
                        "endFrame": {"type": "integer"},
                        "captionDetail": {"type": "boolean"}
                    }
                }
            },
            {
                "name": "move_clips",
                "description": "Moves clips atomically by stable ID to a compatible track and/or non-negative project frame, propagating frame deltas to linked partners.",
                "inputSchema": {
                    "type": "object",
                    "properties": {
                        "moves": {
                            "type": "array",
                            "items": {
                                "type": "object",
                                "properties": {
                                    "clipId": {"type": "string"},
                                    "toTrack": {"type": "integer"},
                                    "toFrame": {"type": "integer"}
                                },
                                "required": ["clipId"]
                            }
                        }
                    },
                    "required": ["moves"]
                }
            },
            {
                "name": "remove_clips",
                "description": "Removes clips atomically by stable ID, expands linked groups, prunes empty tracks, and records one undo action.",
                "inputSchema": {
                    "type": "object",
                    "properties": {
                        "clipIds": {
                            "type": "array",
                            "items": {"type": "string"}
                        }
                    },
                    "required": ["clipIds"]
                }
            },
            {
                "name": "split_clips",
                "description": "Splits clips atomically by stable ID or track frame in one shared undo action.",
                "inputSchema": {
                    "type": "object",
                    "properties": {
                        "splits": {
                            "type": "array",
                            "items": {
                                "type": "object",
                                "properties": {
                                    "clipId": {"type": "string"},
                                    "atFrame": {"type": "integer"}
                                },
                                "required": ["clipId", "atFrame"]
                            }
                        },
                        "trackIndex": {"type": "integer"},
                        "frames": {"type": "array", "items": {"type": "integer"}}
                    }
                }
            },
            {
                "name": "undo",
                "description": "Reverts the latest action from the shared Windows project history.",
                "inputSchema": {"type": "object"}
            }
        ]
    })json");
    return value;
}

Value toolSuccess(Value payload) {
    return Value(Object{{
        "content",
        Value(Array{Value(Object{
            {"text", Value(palmier::json::canonical(payload))},
            {"type", Value("text")},
        })}),
    }});
}

Value toolFailure(std::string code, std::string message) {
    const auto text = palmier::json::canonical(Value(Object{
        {"code", Value(std::move(code))},
        {"message", Value(std::move(message))},
    }));
    return Value(Object{
        {"content", Value(Array{Value(Object{
            {"text", Value(text)},
            {"type", Value("text")},
        })})},
        {"isError", Value(true)},
    });
}

Value jsonRpcResult(const Value& id, Value result) {
    return Value(Object{
        {"id", id},
        {"jsonrpc", Value("2.0")},
        {"result", std::move(result)},
    });
}

Value jsonRpcError(const Value& id, std::int64_t code, std::string message) {
    return Value(Object{
        {"error", Value(Object{
            {"code", Value(integerNumber(code))},
            {"message", Value(std::move(message))},
        })},
        {"id", id},
        {"jsonrpc", Value("2.0")},
    });
}

HttpResponse jsonResponse(Value value, int status = 200, std::string reason = "OK") {
    return {status, std::move(reason), {}, palmier::json::canonical(value)};
}

Value invokeTool(
    palmier::project::ProjectRuntime& runtime,
    std::string_view name,
    const Value& arguments,
    std::uint64_t expectedProjectGeneration
) {
    try {
        const auto& object = requireObject(arguments, "arguments");
        if (name == "get_timeline") {
            rejectUnknownKeys(object, {"startFrame", "endFrame", "captionDetail"}, name);
            palmier::project::TimelineQuery query;
            if (const auto field = object.find("startFrame"); field != object.end()) {
                query.startFrame = requireInteger(field->second, "startFrame");
            }
            if (const auto field = object.find("endFrame"); field != object.end()) {
                query.endFrame = requireInteger(field->second, "endFrame");
            }
            if (const auto field = object.find("captionDetail"); field != object.end()) {
                if (field->second.kind() != Value::Kind::boolean) {
                    throw palmier::project::CommandError(
                        "invalidArguments",
                        "captionDetail must be a boolean"
                    );
                }
                query.captionDetail = field->second.boolean();
            }
            return toolSuccess(runtime.getTimeline(query, expectedProjectGeneration).timeline);
        }
        if (name == "move_clips") {
            rejectUnknownKeys(object, {"moves"}, name);
            const auto& movesValue = requiredField(object, "moves");
            if (movesValue.kind() != Value::Kind::array) {
                throw palmier::project::CommandError("invalidArguments", "moves must be an array");
            }
            palmier::project::MoveClipsCommand command;
            command.moves.reserve(movesValue.array().size());
            for (const auto& item : movesValue.array()) {
                const auto& move = requireObject(item, "move item");
                rejectUnknownKeys(move, {"clipId", "toTrack", "toFrame"}, "move item");
                palmier::project::ClipMove parsed{
                    requireString(requiredField(move, "clipId"), "clipId"),
                    std::nullopt,
                    std::nullopt,
                };
                if (const auto field = move.find("toTrack"); field != move.end()) {
                    const auto trackIndex = requireInteger(field->second, "toTrack");
                    if (trackIndex < 0) {
                        throw palmier::project::CommandError(
                            "invalidArguments",
                            "toTrack must not be negative"
                        );
                    }
                    parsed.toTrack = static_cast<std::size_t>(trackIndex);
                }
                if (const auto field = move.find("toFrame"); field != move.end()) {
                    parsed.toFrame = requireInteger(field->second, "toFrame");
                }
                command.moves.push_back(std::move(parsed));
            }
            auto result = runtime.moveClips(
                std::move(command),
                expectedProjectGeneration
            );
            return toolSuccess(std::move(*result.command.payload));
        }
        if (name == "remove_clips") {
            rejectUnknownKeys(object, {"clipIds"}, name);
            const auto& clipIdsValue = requiredField(object, "clipIds");
            if (clipIdsValue.kind() != Value::Kind::array) {
                throw palmier::project::CommandError(
                    "invalidArguments",
                    "clipIds must be an array"
                );
            }
            palmier::project::RemoveClipsCommand command;
            command.clipIds.reserve(clipIdsValue.array().size());
            for (const auto& clipId : clipIdsValue.array()) {
                command.clipIds.push_back(requireString(clipId, "clipIds item"));
            }
            auto result = runtime.removeClips(
                std::move(command),
                expectedProjectGeneration
            );
            return toolSuccess(std::move(*result.command.payload));
        }
        if (name == "split_clips") {
            rejectUnknownKeys(object, {"splits", "trackIndex", "frames"}, name);
            palmier::project::SplitClipsCommand command;
            if (const auto field = object.find("splits"); field != object.end()) {
                if (field->second.kind() != Value::Kind::array) {
                    throw palmier::project::CommandError("invalidArguments", "splits must be an array");
                }
                std::vector<palmier::project::SplitPoint> splits;
                for (const auto& item : field->second.array()) {
                    const auto& split = requireObject(item, "split item");
                    rejectUnknownKeys(split, {"clipId", "atFrame"}, "split item");
                    splits.push_back({
                        requireString(requiredField(split, "clipId"), "clipId"),
                        requireInteger(requiredField(split, "atFrame"), "atFrame"),
                    });
                }
                command.splits = std::move(splits);
            }
            if (const auto field = object.find("trackIndex"); field != object.end()) {
                const auto trackIndex = requireInteger(field->second, "trackIndex");
                if (trackIndex < 0) {
                    throw palmier::project::CommandError("invalidArguments", "trackIndex must not be negative");
                }
                command.trackIndex = static_cast<std::size_t>(trackIndex);
            }
            if (const auto field = object.find("frames"); field != object.end()) {
                if (field->second.kind() != Value::Kind::array) {
                    throw palmier::project::CommandError("invalidArguments", "frames must be an array");
                }
                std::vector<std::int64_t> frames;
                for (const auto& frame : field->second.array()) {
                    frames.push_back(requireInteger(frame, "frame"));
                }
                command.frames = std::move(frames);
            }
            auto result = runtime.splitClips(
                std::move(command),
                expectedProjectGeneration
            );
            return toolSuccess(std::move(*result.command.payload));
        }
        if (name == "undo") {
            rejectUnknownKeys(object, {}, name);
            auto result = runtime.undo(expectedProjectGeneration);
            return toolSuccess(std::move(*result.command.payload));
        }
        return toolFailure("toolNotImplemented", "tool is not implemented by the Windows technical MVP");
    } catch (const palmier::project::CommandError& error) {
        return toolFailure(error.code, error.what());
    } catch (const palmier::project::ProjectRuntimeError& error) {
        return toolFailure(error.code, error.what());
    } catch (const std::exception& error) {
        return toolFailure("internalError", error.what());
    }
}

std::optional<std::string> headerValue(
    const HttpRequest& request,
    std::string_view name
) {
    const auto field = request.headers.find(name);
    return field == request.headers.end()
        ? std::nullopt
        : std::optional<std::string>(field->second);
}

std::string mediaType(std::string_view value) {
    return lower(trim(value.substr(0, value.find(';'))));
}

bool acceptsMcpResponse(const HttpRequest& request) {
    const auto accept = headerValue(request, "accept");
    if (!accept) {
        return false;
    }
    std::size_t start = 0;
    while (start <= accept->size()) {
        const auto comma = accept->find(',', start);
        const auto token = mediaType(std::string_view(*accept).substr(
            start,
            comma == std::string::npos ? std::string::npos : comma - start
        ));
        if (token == "application/json" || token == "text/event-stream" || token == "*/*") {
            return true;
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return false;
}

void validateOrigin(const HttpRequest& request, std::uint16_t port) {
    const auto origin = request.headers.find("origin");
    if (origin == request.headers.end()) {
        return;
    }
    const auto normalized = lower(origin->second);
    const auto suffix = ":" + std::to_string(port);
    if (normalized != "http://127.0.0.1" + suffix && normalized != "http://localhost" + suffix) {
        throw HttpError(403, "Forbidden", "Origin is not loopback-local");
    }
}

std::optional<std::string> sessionHeader(const HttpRequest& request) {
    const auto field = request.headers.find("mcp-session-id");
    return field == request.headers.end()
        ? std::nullopt
        : std::optional<std::string>(field->second);
}

void pruneExpiredSessions(
    std::map<std::string, SessionState, std::less<>>& sessions,
    std::chrono::steady_clock::time_point now
) {
    for (auto session = sessions.begin(); session != sessions.end();) {
        if (now - session->second.lastActivity >= sessionIdleTimeout) {
            session = sessions.erase(session);
        } else {
            ++session;
        }
    }
}

HttpResponse handleRequest(
    const HttpRequest& request,
    palmier::project::ProjectRuntime& projectRuntime,
    const HttpServerOptions& options,
    const std::function<std::string()>& sessionIdGenerator,
    std::map<std::string, SessionState, std::less<>>& sessions,
    bool& shouldExit
) {
    const auto now = std::chrono::steady_clock::now();
    pruneExpiredSessions(sessions, now);
    if (request.path != "/mcp") {
        throw HttpError(404, "Not Found", "unknown HTTP path");
    }
    validateOrigin(request, options.port);
    if (request.method == "DELETE") {
        const auto session = sessionHeader(request);
        if (!session || sessions.erase(*session) == 0) {
            throw HttpError(404, "Not Found", "unknown MCP session");
        }
        shouldExit = options.exitAfterLastSession && sessions.empty();
        return {204, "No Content", {}, {}};
    }
    if (request.method != "POST") {
        throw HttpError(405, "Method Not Allowed", "MCP supports POST and DELETE only");
    }
    const auto contentType = headerValue(request, "content-type");
    if (!contentType || mediaType(*contentType) != "application/json") {
        throw HttpError(415, "Unsupported Media Type", "Content-Type must be application/json");
    }
    if (!acceptsMcpResponse(request)) {
        throw HttpError(406, "Not Acceptable", "Accept must allow MCP response media types");
    }

    Value message;
    try {
        message = palmier::json::parse(request.body);
    } catch (const std::exception&) {
        return jsonResponse(jsonRpcError(Value(), -32700, "Parse error"), 400, "Bad Request");
    }
    if (message.kind() != Value::Kind::object) {
        return jsonResponse(jsonRpcError(Value(), -32600, "Invalid Request"), 400, "Bad Request");
    }
    const auto& object = message.object();
    const auto versionField = object.find("jsonrpc");
    if (
        versionField == object.end()
        || versionField->second.kind() != Value::Kind::string
        || versionField->second.string() != "2.0"
    ) {
        return jsonResponse(jsonRpcError(Value(), -32600, "Invalid Request"), 400, "Bad Request");
    }
    const auto methodField = object.find("method");
    if (methodField == object.end() || methodField->second.kind() != Value::Kind::string) {
        return jsonResponse(jsonRpcError(Value(), -32600, "Invalid Request"), 400, "Bad Request");
    }
    const auto& method = methodField->second.string();
    const auto idField = object.find("id");
    const Value id = idField == object.end() ? Value() : idField->second;
    const auto paramsField = object.find("params");
    const Value emptyParams(Object{});
    const auto& params = paramsField == object.end() ? emptyParams : paramsField->second;

    if (method == "initialize") {
        try {
            const auto& paramsObject = requireObject(params, "initialize params");
            const auto requested = requireString(
                requiredField(paramsObject, "protocolVersion"),
                "protocolVersion"
            );
            if (requested != protocolVersion) {
                return jsonResponse(jsonRpcError(id, -32602, "Unsupported protocol version"));
            }
        } catch (const palmier::project::CommandError& error) {
            return jsonResponse(jsonRpcError(id, -32602, error.what()));
        }
        const auto sessionId = sessionIdGenerator();
        if (sessionId.empty() || sessions.contains(sessionId)) {
            throw HttpError(500, "Internal Server Error", "session ID generation failed");
        }
        if (sessions.size() >= maximumSessions) {
            const auto oldest = std::min_element(
                sessions.begin(),
                sessions.end(),
                [](const auto& left, const auto& right) {
                    return left.second.lastActivity < right.second.lastActivity;
                }
            );
            sessions.erase(oldest);
        }
        sessions.emplace(sessionId, SessionState{
            false,
            now,
            projectRuntime.projectGeneration(),
        });
        auto response = jsonResponse(jsonRpcResult(id, Value(Object{
            {"capabilities", Value(Object{
                {"tools", Value(Object{{"listChanged", Value(false)}})},
            })},
            {"protocolVersion", Value(std::string(protocolVersion))},
            {"serverInfo", Value(Object{
                {"name", Value("palmier-pro-windows")},
                {"version", Value("0.1.0-m1")},
            })},
        })));
        response.headers.emplace("Mcp-Session-Id", sessionId);
        return response;
    }

    const auto session = sessionHeader(request);
    const auto sessionState = session ? sessions.find(*session) : sessions.end();
    if (!session || sessionState == sessions.end()) {
        throw HttpError(404, "Not Found", "unknown MCP session");
    }
    sessionState->second.lastActivity = now;
    const auto protocol = request.headers.find("mcp-protocol-version");
    if (protocol == request.headers.end() || protocol->second != protocolVersion) {
        throw HttpError(400, "Bad Request", "missing or unsupported MCP protocol version");
    }
    if (sessionState->second.projectGeneration != projectRuntime.projectGeneration()) {
        sessions.erase(sessionState);
        throw HttpError(409, "Conflict", "MCP session belongs to a replaced project");
    }
    if (method == "notifications/initialized") {
        sessionState->second.initialized = true;
        return {202, "Accepted", {}, {}};
    }
    if (!sessionState->second.initialized) {
        throw HttpError(409, "Conflict", "MCP session is not initialized");
    }
    if (idField == object.end()) {
        return jsonResponse(jsonRpcError(Value(), -32600, "Request id is required"), 400, "Bad Request");
    }
    if (method == "tools/list") {
        return jsonResponse(jsonRpcResult(id, toolsList()));
    }
    if (method == "tools/call") {
        try {
            const auto& paramsObject = requireObject(params, "tools/call params");
            rejectUnknownKeys(paramsObject, {"name", "arguments"}, "tools/call params");
            const auto name = requireString(requiredField(paramsObject, "name"), "tool name");
            const auto arguments = paramsObject.find("arguments");
            const auto& toolArguments = arguments == paramsObject.end()
                ? emptyParams
                : arguments->second;
            return jsonResponse(jsonRpcResult(id, invokeTool(
                projectRuntime,
                name,
                toolArguments,
                sessionState->second.projectGeneration
            )));
        } catch (const palmier::project::CommandError& error) {
            return jsonResponse(jsonRpcError(id, -32602, error.what()));
        }
    }
    return jsonResponse(jsonRpcError(id, -32601, "Method not found"));
}

HttpResponse errorResponse(const HttpError& error) {
    return jsonResponse(
        Value(Object{
            {"error", Value(error.what())},
            {"status", Value(integerNumber(error.status))},
        }),
        error.status,
        error.reason
    );
}

void serveHttpServer(
    palmier::project::ProjectRuntime& projectRuntime,
    const HttpServerOptions& options,
    const std::function<std::string()>& sessionIdGenerator,
    std::stop_token cancellation,
    HANDLE stopEvent,
    const std::shared_ptr<HttpServerObserver>& observer,
    const std::function<void(std::uint16_t)>& ready
) {
    SocketSystem socketSystem;
    Socket listener(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (listener.get() == INVALID_SOCKET) {
        throw std::runtime_error("cannot create MCP listener socket");
    }
    const BOOL exclusive = TRUE;
    if (setsockopt(
        listener.get(),
        SOL_SOCKET,
        SO_EXCLUSIVEADDRUSE,
        reinterpret_cast<const char*>(&exclusive),
        static_cast<int>(sizeof(exclusive))
    ) == SOCKET_ERROR) {
        throw std::runtime_error("cannot make MCP listener exclusive");
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(options.port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(
        listener.get(),
        reinterpret_cast<const sockaddr*>(&address),
        static_cast<int>(sizeof(address))
    ) == SOCKET_ERROR) {
        throw std::runtime_error("cannot bind MCP listener to 127.0.0.1");
    }
    if (listen(listener.get(), SOMAXCONN) == SOCKET_ERROR) {
        throw std::runtime_error("cannot listen for MCP requests");
    }
    WinsockEvent listenerEvent(WSACreateEvent());
    if (listenerEvent.get() == WSA_INVALID_EVENT) {
        throw std::runtime_error("cannot create MCP listener event");
    }
    if (WSAEventSelect(listener.get(), listenerEvent.get(), FD_ACCEPT) == SOCKET_ERROR) {
        throw std::runtime_error("cannot make MCP listener wait cancellable");
    }
    sockaddr_in boundAddress{};
    int boundAddressSize = static_cast<int>(sizeof(boundAddress));
    if (getsockname(
        listener.get(),
        reinterpret_cast<sockaddr*>(&boundAddress),
        &boundAddressSize
    ) == SOCKET_ERROR) {
        throw std::runtime_error("cannot inspect MCP listener address");
    }
    const auto boundPort = ntohs(boundAddress.sin_port);
    if (cancellation.stop_requested()) return;
    ready(boundPort);
    std::map<std::string, SessionState, std::less<>> sessions;
    bool shouldExit = false;
    while (!shouldExit && !cancellation.stop_requested()) {
        const std::array<HANDLE, 2> events{stopEvent, listenerEvent.get()};
        const auto waitResult = WaitForMultipleObjects(
            static_cast<DWORD>(events.size()),
            events.data(),
            FALSE,
            INFINITE
        );
        if (waitResult == WAIT_OBJECT_0) break;
        if (waitResult != WAIT_OBJECT_0 + 1) {
            throw std::runtime_error("cannot wait for MCP listener events");
        }
        WSANETWORKEVENTS networkEvents{};
        if (WSAEnumNetworkEvents(
            listener.get(),
            listenerEvent.get(),
            &networkEvents
        ) == SOCKET_ERROR) {
            throw std::runtime_error("cannot inspect MCP listener events");
        }
        if ((networkEvents.lNetworkEvents & FD_ACCEPT) == 0) continue;
        if (networkEvents.iErrorCode[FD_ACCEPT_BIT] != 0) {
            throw std::runtime_error("MCP listener reported an accept failure");
        }
        sockaddr_in peer{};
        int peerSize = static_cast<int>(sizeof(peer));
        Socket client(accept(
            listener.get(),
            reinterpret_cast<sockaddr*>(&peer),
            &peerSize
        ));
        if (client.get() == INVALID_SOCKET) {
            if (WSAGetLastError() == WSAEWOULDBLOCK) continue;
            throw std::runtime_error("cannot accept MCP connection");
        }
        if (cancellation.stop_requested()) break;
        u_long blockingMode = 0;
        if (
            WSAEventSelect(client.get(), nullptr, 0) == SOCKET_ERROR
            || ioctlsocket(client.get(), FIONBIO, &blockingMode) == SOCKET_ERROR
        ) {
            throw std::runtime_error("cannot make MCP client socket blocking");
        }
        if (observer) observer->clientAdmitted();
        const auto requestDeadline = std::chrono::steady_clock::now()
            + maximumRequestLifetime;
        if (peer.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) {
            try {
                sendResponse(
                    client.get(),
                    {403, "Forbidden", {}, {}},
                    cancellation,
                    requestDeadline
                );
            } catch (const RequestCancelled&) {
                break;
            } catch (const std::exception&) {
            }
            continue;
        }
        HttpResponse response;
        bool requestParsed = false;
        try {
            const auto request = readRequest(
                client.get(),
                cancellation,
                requestDeadline,
                observer
            );
            requestParsed = true;
            response = handleRequest(
                request,
                projectRuntime,
                options,
                sessionIdGenerator,
                sessions,
                shouldExit
            );
        } catch (const RequestCancelled&) {
            break;
        } catch (const HttpError& error) {
            response = errorResponse(error);
        } catch (const std::exception& error) {
            response = jsonResponse(
                Value(Object{{"error", Value(error.what())}}),
                500,
                "Internal Server Error"
            );
        }
        try {
            const auto responseCancellation = requestParsed
                ? std::stop_token{}
                : cancellation;
            sendResponse(
                client.get(),
                std::move(response),
                responseCancellation,
                std::chrono::steady_clock::now() + maximumRequestLifetime
            );
        } catch (const RequestCancelled&) {
            break;
        } catch (const std::exception&) {
        }
    }
}

}

struct HttpServerService::Implementation final {
    Implementation(
        palmier::project::ProjectRuntime& projectRuntimeValue,
        HttpServerOptions optionsValue,
        std::function<std::string()> sessionIdGeneratorValue,
        std::shared_ptr<HttpServerObserver> observerValue
    )
        : projectRuntime(projectRuntimeValue),
          options(optionsValue),
          sessionIdGenerator(std::move(sessionIdGeneratorValue)),
          observer(std::move(observerValue)),
          stopEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
        if (!sessionIdGenerator) {
            throw std::runtime_error("MCP server requires a session ID generator");
        }
        if (stopEvent.get() == nullptr) {
            throw std::runtime_error("cannot create MCP server stop event");
        }
    }

    ~Implementation() {
        requestStop();
        join();
    }

    Implementation(const Implementation&) = delete;
    Implementation& operator=(const Implementation&) = delete;

    void start() {
        std::scoped_lock threadLock(threadMutex);
        {
            std::scoped_lock statusLock(statusMutex);
            if (statusValue.state != HttpServerState::idle) {
                throw std::runtime_error("MCP server service can only be started once");
            }
            statusValue = {HttpServerState::starting, 0, {}};
        }
        statusCondition.notify_all();
        try {
            worker = std::thread([this] { run(); });
        } catch (const std::exception& error) {
            publish({HttpServerState::failed, 0, error.what()});
            throw;
        }
    }

    void requestStop() noexcept {
        {
            std::scoped_lock lock(statusMutex);
            stopSource.request_stop();
            if (statusValue.state == HttpServerState::idle) {
                statusValue = {HttpServerState::stopped, 0, {}};
            }
        }
        statusCondition.notify_all();
        static_cast<void>(SetEvent(stopEvent.get()));
    }

    void join() noexcept {
        std::scoped_lock threadLock(threadMutex);
        if (!worker.joinable() || worker.get_id() == std::this_thread::get_id()) return;
        worker.join();
    }

    HttpServerStatus status() const {
        std::scoped_lock lock(statusMutex);
        return statusValue;
    }

    HttpServerStatus waitForReadyOrTerminal() {
        std::unique_lock lock(statusMutex);
        statusCondition.wait(lock, [this] {
            return statusValue.state == HttpServerState::ready
                || statusValue.state == HttpServerState::failed
                || statusValue.state == HttpServerState::stopped;
        });
        return statusValue;
    }

    void publish(HttpServerStatus value) noexcept {
        {
            std::scoped_lock lock(statusMutex);
            statusValue = std::move(value);
        }
        statusCondition.notify_all();
    }

    void publishReady(std::uint16_t port) noexcept {
        {
            std::scoped_lock lock(statusMutex);
            if (stopSource.stop_requested()) return;
            statusValue = {HttpServerState::ready, port, {}};
        }
        statusCondition.notify_all();
    }

    void run() noexcept {
        try {
            serveHttpServer(
                projectRuntime,
                options,
                sessionIdGenerator,
                stopSource.get_token(),
                stopEvent.get(),
                observer,
                [this](std::uint16_t port) { publishReady(port); }
            );
            publish({HttpServerState::stopped, status().port, {}});
        } catch (const std::exception& error) {
            if (stopSource.stop_requested()) {
                publish({HttpServerState::stopped, status().port, {}});
            } else {
                publish({HttpServerState::failed, 0, error.what()});
            }
        } catch (...) {
            if (stopSource.stop_requested()) {
                publish({HttpServerState::stopped, status().port, {}});
            } else {
                publish({HttpServerState::failed, 0, "unknown MCP server failure"});
            }
        }
    }

    palmier::project::ProjectRuntime& projectRuntime;
    const HttpServerOptions options;
    const std::function<std::string()> sessionIdGenerator;
    const std::shared_ptr<HttpServerObserver> observer;
    std::stop_source stopSource;
    UniqueHandle stopEvent;
    mutable std::mutex statusMutex;
    std::condition_variable statusCondition;
    HttpServerStatus statusValue;
    std::mutex threadMutex;
    std::thread worker;
};

HttpServerService::HttpServerService(
    palmier::project::ProjectRuntime& projectRuntime,
    HttpServerOptions options,
    std::function<std::string()> sessionIdGenerator,
    std::shared_ptr<HttpServerObserver> observer
)
    : implementation_(std::make_unique<Implementation>(
        projectRuntime,
        options,
        std::move(sessionIdGenerator),
        std::move(observer)
    )) {}

HttpServerService::~HttpServerService() = default;

void HttpServerService::start() { implementation_->start(); }

void HttpServerService::requestStop() noexcept { implementation_->requestStop(); }

void HttpServerService::join() noexcept { implementation_->join(); }

HttpServerStatus HttpServerService::status() const { return implementation_->status(); }

HttpServerStatus HttpServerService::waitForReadyOrTerminal() {
    return implementation_->waitForReadyOrTerminal();
}

int runHttpServer(
    palmier::project::ProjectRuntime& projectRuntime,
    const HttpServerOptions& options,
    const std::function<std::string()>& sessionIdGenerator
) {
    HttpServerService service(projectRuntime, options, sessionIdGenerator);
    service.start();
    const auto ready = service.waitForReadyOrTerminal();
    if (ready.state == HttpServerState::failed) {
        throw std::runtime_error(ready.error);
    }
    if (ready.state != HttpServerState::ready) return 0;
    std::cout << "PALMIER_WINDOWS_MCP_READY 127.0.0.1:" << ready.port << '\n';
    std::cout.flush();
    service.join();
    const auto terminal = service.status();
    if (terminal.state == HttpServerState::failed) {
        throw std::runtime_error(terminal.error);
    }
    return 0;
}

}
