#include "palmier/contracts/top_level_json.hpp"

namespace palmier::contracts {
namespace {

JsonValueSummary summarize(const palmier::json::Value& value) {
    using Kind = palmier::json::Value::Kind;
    switch (value.kind()) {
    case Kind::nullValue:
        return {JsonValueKind::nullValue, std::nullopt, std::nullopt, std::nullopt};
    case Kind::boolean:
        return {JsonValueKind::boolean, std::nullopt, value.boolean(), std::nullopt};
    case Kind::number:
        return {JsonValueKind::number, value.number().integer, std::nullopt, std::nullopt};
    case Kind::string:
        return {JsonValueKind::string, std::nullopt, std::nullopt, value.string()};
    case Kind::array:
        return {JsonValueKind::array, std::nullopt, std::nullopt, std::nullopt};
    case Kind::object:
        return {JsonValueKind::object, std::nullopt, std::nullopt, std::nullopt};
    }
    throw JsonError("unsupported JSON value kind");
}

TopLevelJsonObject summarizeObject(const palmier::json::Value& document) {
    if (document.kind() != palmier::json::Value::Kind::object) {
        throw JsonError("top-level JSON value must be an object");
    }
    TopLevelJsonObject result;
    for (const auto& [key, value] : document.object()) {
        result.emplace(key, summarize(value));
    }
    return result;
}

}

TopLevelJsonObject parseTopLevelJsonObject(std::string_view source) {
    return summarizeObject(palmier::json::parse(source));
}

TopLevelJsonObject readTopLevelJsonObject(const std::filesystem::path& path) {
    return summarizeObject(palmier::json::read(path));
}

std::string pathForDiagnostic(const std::filesystem::path& path) noexcept {
    return palmier::json::pathForDiagnostic(path);
}

}
