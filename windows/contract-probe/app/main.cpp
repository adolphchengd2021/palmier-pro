#include "palmier/contracts/repository_contract_probe.hpp"

#include <filesystem>
#include <iostream>
#include <string_view>

namespace {

int printResult(const palmier::contracts::ProbeResult& result) {
    auto& stream = result.code == palmier::contracts::ProbeExitCode::success
        ? std::cout
        : std::cerr;
    stream << result.marker << ' ' << result.detail << '\n';
    return static_cast<int>(result.code);
}

int printJsonResult(const palmier::contracts::ProbeResult& result) {
    if (result.code == palmier::contracts::ProbeExitCode::success) {
        std::cout << result.detail << '\n';
        return 0;
    }
    return printResult(result);
}

}

int wmain(int argumentCount, wchar_t* arguments[]) {
    if (argumentCount != 3) {
        std::cerr << "PALMIER_CONTRACT_E_USAGE expected a supported command and path\n";
        return static_cast<int>(palmier::contracts::ProbeExitCode::usage);
    }

    const std::wstring_view command(arguments[1]);
    const std::filesystem::path path(arguments[2]);
    if (command == L"--repo-root") {
        return printResult(palmier::contracts::probeRepository(path));
    }
    if (command == L"--contract-version-file") {
        return printResult(palmier::contracts::probeContractVersionFile(path));
    }
    if (command == L"--normalize-project-file") {
        return printJsonResult(palmier::contracts::normalizeProjectFile(path));
    }
    if (command == L"--canonical-project-source") {
        return printJsonResult(palmier::contracts::canonicalizeProjectSource(path));
    }

    std::cerr << "PALMIER_CONTRACT_E_USAGE unknown command\n";
    return static_cast<int>(palmier::contracts::ProbeExitCode::usage);
}
