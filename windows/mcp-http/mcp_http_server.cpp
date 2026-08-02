#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "palmier/mcp/mcp_http_server.hpp"

#include "palmier/json/json_document.hpp"

#include <WinSock2.h>
#include <WS2tcpip.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

HttpRequest readRequest(SOCKET clientSocket) {
    std::string bytes;
    bytes.reserve(8192);
    std::array<char, 8192> buffer{};
    std::size_t headerEnd = std::string::npos;
    while (headerEnd == std::string::npos) {
        const auto received = recv(clientSocket, buffer.data(), static_cast<int>(buffer.size()), 0);
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
        const auto remaining = contentLength - request.body.size();
        const auto amount = static_cast<int>(std::min(remaining, buffer.size()));
        const auto received = recv(clientSocket, buffer.data(), amount, 0);
        if (received <= 0) {
            throw HttpError(400, "Bad Request", "connection closed before request body");
        }
        request.body.append(buffer.data(), static_cast<std::size_t>(received));
    }
    return request;
}

void sendAll(SOCKET clientSocket, std::string_view bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const auto remaining = bytes.size() - sent;
        const auto chunk = static_cast<int>(std::min(
            remaining,
            static_cast<std::size_t>(std::numeric_limits<int>::max())
        ));
        const auto result = send(clientSocket, bytes.data() + sent, chunk, 0);
        if (result == SOCKET_ERROR || result == 0) {
            throw std::runtime_error("cannot send HTTP response");
        }
        sent += static_cast<std::size_t>(result);
    }
}

void sendResponse(SOCKET clientSocket, HttpResponse response) {
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
    sendAll(clientSocket, output.str());
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
    palmier::project::ProjectSession& session,
    std::string_view name,
    const Value& arguments
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
            return toolSuccess(session.getTimeline(query));
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
            auto result = session.splitClips(command);
            return toolSuccess(std::move(*result.payload));
        }
        if (name == "undo") {
            rejectUnknownKeys(object, {}, name);
            auto result = session.undo();
            return toolSuccess(std::move(*result.payload));
        }
        return toolFailure("toolNotImplemented", "tool is not implemented by the Windows technical MVP");
    } catch (const palmier::project::CommandError& error) {
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
    palmier::project::ProjectSession& projectSession,
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
        sessions.emplace(sessionId, SessionState{false, now});
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
            return jsonResponse(jsonRpcResult(id, invokeTool(projectSession, name, toolArguments)));
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

}

int runHttpServer(
    palmier::project::ProjectSession& projectSession,
    const HttpServerOptions& options,
    const std::function<std::string()>& sessionIdGenerator
) {
    if (!sessionIdGenerator) {
        throw std::runtime_error("MCP server requires a session ID generator");
    }
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

    std::cout << "PALMIER_WINDOWS_MCP_READY 127.0.0.1:" << options.port << '\n';
    std::cout.flush();
    std::map<std::string, SessionState, std::less<>> sessions;
    bool shouldExit = false;
    while (!shouldExit) {
        sockaddr_in peer{};
        int peerSize = static_cast<int>(sizeof(peer));
        Socket client(accept(
            listener.get(),
            reinterpret_cast<sockaddr*>(&peer),
            &peerSize
        ));
        if (client.get() == INVALID_SOCKET) {
            throw std::runtime_error("cannot accept MCP connection");
        }
        const DWORD timeoutMilliseconds = 1500;
        if (
            setsockopt(
                client.get(),
                SOL_SOCKET,
                SO_RCVTIMEO,
                reinterpret_cast<const char*>(&timeoutMilliseconds),
                static_cast<int>(sizeof(timeoutMilliseconds))
            ) == SOCKET_ERROR
            || setsockopt(
                client.get(),
                SOL_SOCKET,
                SO_SNDTIMEO,
                reinterpret_cast<const char*>(&timeoutMilliseconds),
                static_cast<int>(sizeof(timeoutMilliseconds))
            ) == SOCKET_ERROR
        ) {
            throw std::runtime_error("cannot bound MCP connection timeouts");
        }
        if (peer.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) {
            try {
                sendResponse(client.get(), {403, "Forbidden", {}, {}});
            } catch (const std::exception&) {
            }
            continue;
        }
        HttpResponse response;
        try {
            const auto request = readRequest(client.get());
            response = handleRequest(
                request,
                projectSession,
                options,
                sessionIdGenerator,
                sessions,
                shouldExit
            );
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
            sendResponse(client.get(), std::move(response));
        } catch (const std::exception&) {
        }
    }
    return 0;
}

}
