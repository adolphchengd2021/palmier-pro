#include "palmier/contracts/repository_contract_probe.hpp"

#include "palmier/contracts/top_level_json.hpp"

#include <array>
#include <ios>
#include <utility>

namespace palmier::contracts {
namespace {

class ProbeFailure final : public std::runtime_error {
public:
    ProbeFailure(ProbeExitCode code, std::string marker, std::string detail)
        : std::runtime_error(detail), code(code), marker(std::move(marker)) {}

    ProbeExitCode code;
    std::string marker;
};

[[noreturn]] void fail(
    ProbeExitCode code,
    std::string marker,
    const std::filesystem::path& path,
    const std::string& detail
) {
    throw ProbeFailure(code, std::move(marker), pathForDiagnostic(path) + ": " + detail);
}

const JsonValueSummary& requireField(
    const TopLevelJsonObject& document,
    const std::filesystem::path& path,
    const std::string& key
) {
    const auto field = document.find(key);
    if (field == document.end()) {
        fail(ProbeExitCode::invariant, "PALMIER_CONTRACT_E_INVARIANT", path, "missing '" + key + "'");
    }
    return field->second;
}

TopLevelJsonObject readDocument(const std::filesystem::path& path) {
    try {
        return readTopLevelJsonObject(path);
    } catch (const JsonError& error) {
        fail(ProbeExitCode::json, "PALMIER_CONTRACT_E_JSON", path, error.what());
    } catch (const std::ios_base::failure& error) {
        fail(ProbeExitCode::io, "PALMIER_CONTRACT_E_IO", path, error.what());
    }
}

void requireContractVersion(const std::filesystem::path& path) {
    const auto document = readDocument(path);
    const auto field = document.find("contractVersion");
    if (field == document.end()) {
        fail(ProbeExitCode::version, "PALMIER_CONTRACT_E_VERSION", path, "missing contractVersion");
    }
    if (field->second.kind != JsonValueKind::number || !field->second.integer.has_value()) {
        fail(ProbeExitCode::version, "PALMIER_CONTRACT_E_VERSION", path, "contractVersion must be an integer");
    }
    if (*field->second.integer != 1) {
        fail(ProbeExitCode::version, "PALMIER_CONTRACT_E_VERSION", path, "contractVersion must equal 1");
    }
}

void requireKind(
    const JsonValueSummary& value,
    JsonValueKind expected,
    const std::filesystem::path& path,
    const std::string& field
) {
    if (value.kind != expected) {
        fail(ProbeExitCode::invariant, "PALMIER_CONTRACT_E_INVARIANT", path, "'" + field + "' has the wrong JSON type");
    }
}

ProbeResult run(const auto& operation) {
    try {
        operation();
        return {ProbeExitCode::success, "PALMIER_CONTRACT_OK", "Windows contract probe passed"};
    } catch (const ProbeFailure& error) {
        return {error.code, error.marker, error.what()};
    } catch (const std::exception& error) {
        return {ProbeExitCode::invariant, "PALMIER_CONTRACT_E_INVARIANT", error.what()};
    }
}

}

ProbeResult probeContractVersionFile(const std::filesystem::path& path) {
    return run([&] { requireContractVersion(path); });
}

ProbeResult probeRepository(const std::filesystem::path& root) {
    return run([&] {
        constexpr std::array versionedFiles{
            "contracts/effects/v1/effects.json",
            "contracts/effects/v1/blend-modes.json",
            "contracts/mcp/v1/tools.json",
            "contracts/project/v1/canaries.json",
            "contracts/project/v1/media-model.json",
            "fixtures/contracts/media/v1/manifest.json",
        };
        for (const auto* relativePath : versionedFiles) {
            requireContractVersion(root / relativePath);
        }

        const auto canaryPath = root / "contracts/project/v1/canaries.json";
        const auto canaries = readDocument(canaryPath);
        const auto& roundTrip = requireField(canaries, canaryPath, "runtimeRoundTripStatus");
        requireKind(roundTrip, JsonValueKind::string, canaryPath, "runtimeRoundTripStatus");
        if (!roundTrip.string || *roundTrip.string != "enforced") {
            fail(ProbeExitCode::invariant, "PALMIER_CONTRACT_E_INVARIANT", canaryPath, "runtime round trip is not enforced");
        }

        const auto blendPath = root / "contracts/effects/v1/blend-modes.json";
        const auto blend = readDocument(blendPath);
        const auto& count = requireField(blend, blendPath, "count");
        if (!count.integer || *count.integer != 16) {
            fail(ProbeExitCode::invariant, "PALMIER_CONTRACT_E_INVARIANT", blendPath, "blend count must equal 16");
        }
        const auto& includesNormal = requireField(blend, blendPath, "includesNormal");
        if (!includesNormal.boolean || !*includesNormal.boolean) {
            fail(ProbeExitCode::invariant, "PALMIER_CONTRACT_E_INVARIANT", blendPath, "normal blend mode must be included");
        }

        const auto manifestPath = root / "fixtures/contracts/projects/manifest.json";
        const auto manifest = readDocument(manifestPath);
        requireKind(requireField(manifest, manifestPath, "fixtures"), JsonValueKind::array, manifestPath, "fixtures");

        const auto currentPath = root / "fixtures/contracts/projects/current-multitimeline.palmier/project.json";
        const auto current = readDocument(currentPath);
        requireKind(requireField(current, currentPath, "timelines"), JsonValueKind::array, currentPath, "timelines");
        if (current.contains("tracks")) {
            fail(ProbeExitCode::invariant, "PALMIER_CONTRACT_E_INVARIANT", currentPath, "current project must not use the legacy root shape");
        }

        const auto legacyPath = root / "fixtures/contracts/projects/legacy-bare-timeline.palmier/project.json";
        const auto legacy = readDocument(legacyPath);
        requireKind(requireField(legacy, legacyPath, "tracks"), JsonValueKind::array, legacyPath, "tracks");
        if (legacy.contains("timelines")) {
            fail(ProbeExitCode::invariant, "PALMIER_CONTRACT_E_INVARIANT", legacyPath, "legacy project must remain a bare timeline");
        }
    });
}

}
