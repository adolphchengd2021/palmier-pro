#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace palmier::contracts {

enum class JsonValueKind {
    nullValue,
    boolean,
    number,
    string,
    array,
    object,
};

struct JsonValueSummary {
    JsonValueKind kind;
    std::optional<std::int64_t> integer;
    std::optional<bool> boolean;
    std::optional<std::string> string;
};

using TopLevelJsonObject = std::map<std::string, JsonValueSummary>;

class JsonError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

TopLevelJsonObject parseTopLevelJsonObject(std::string_view source);
TopLevelJsonObject readTopLevelJsonObject(const std::filesystem::path& path);
std::string pathForDiagnostic(const std::filesystem::path& path) noexcept;

}
