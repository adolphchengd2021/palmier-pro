#include "palmier/mcp/mcp_http_server.hpp"
#include "palmier/project/project_package_reader.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <objbase.h>

#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

std::string newGuid() {
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) {
        throw std::runtime_error("cannot generate a stable session ID");
    }
    std::ostringstream stream;
    stream << std::hex << std::setfill('0')
           << std::setw(8) << guid.Data1 << '-'
           << std::setw(4) << guid.Data2 << '-'
           << std::setw(4) << guid.Data3 << '-'
           << std::setw(2) << static_cast<unsigned int>(guid.Data4[0])
           << std::setw(2) << static_cast<unsigned int>(guid.Data4[1]) << '-';
    for (std::size_t index = 2; index < 8; ++index) {
        stream << std::setw(2) << static_cast<unsigned int>(guid.Data4[index]);
    }
    return stream.str();
}

std::uint16_t parsePort(std::wstring_view value) {
    std::size_t consumed = 0;
    const auto owned = std::wstring(value);
    const auto parsed = std::stoul(owned, &consumed, 10);
    if (
        consumed != owned.size()
        || parsed == 0
        || parsed > 65535
    ) {
        throw std::runtime_error("port must be an integer from 1 through 65535");
    }
    return static_cast<std::uint16_t>(parsed);
}

}

int wmain(int argumentCount, wchar_t* arguments[]) {
    try {
        std::filesystem::path projectPath;
        palmier::mcp::HttpServerOptions options;
        for (int index = 1; index < argumentCount; ++index) {
            const std::wstring_view argument(arguments[index]);
            if (argument == L"--project" && index + 1 < argumentCount) {
                projectPath = arguments[++index];
            } else if (argument == L"--port" && index + 1 < argumentCount) {
                options.port = parsePort(arguments[++index]);
            } else if (argument == L"--exit-after-last-session") {
                options.exitAfterLastSession = true;
            } else {
                throw std::runtime_error("unknown or incomplete command-line argument");
            }
        }
        if (projectPath.empty()) {
            throw std::runtime_error("--project is required");
        }
        auto document = palmier::project::readProjectPackage(projectPath, newGuid);
        palmier::project::ProjectRuntime runtime;
        static_cast<void>(runtime.install(std::move(document), 1, newGuid));
        return palmier::mcp::runHttpServer(runtime, options, newGuid);
    } catch (const std::exception& error) {
        std::cerr << "PALMIER_WINDOWS_MCP_FAILED " << error.what() << '\n';
        return 1;
    }
}
