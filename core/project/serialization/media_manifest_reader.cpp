#include "palmier/project/media_manifest_reader.hpp"

#include "palmier/json/json_document.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <fstream>
#include <set>
#include <utility>

namespace palmier::project {
namespace {

using JsonKind = palmier::json::Value::Kind;

[[noreturn]] void fail(std::string code, std::string pointer, std::string detail) {
    throw MediaManifestReadError(
        std::move(code),
        std::move(pointer),
        std::move(detail)
    );
}

void checkCancellation(std::stop_token cancellation) {
    if (cancellation.stop_requested()) {
        fail("cancelled", "", "media manifest read was cancelled");
    }
}

const palmier::json::Object& requireObject(
    const palmier::json::Value& value,
    const std::string& pointer
) {
    if (value.kind() != JsonKind::object) {
        fail("wrongRequiredType", pointer, "expected object");
    }
    return value.object();
}

const palmier::json::Value& requireField(
    const palmier::json::Object& object,
    const std::string& key,
    const std::string& pointer
) {
    const auto field = object.find(key);
    if (field == object.end()) {
        fail("missingRequiredField", pointer + "/" + key, "missing required field");
    }
    return field->second;
}

std::string requireString(
    const palmier::json::Object& object,
    const std::string& key,
    const std::string& pointer
) {
    const auto& value = requireField(object, key, pointer);
    if (value.kind() != JsonKind::string) {
        fail("wrongRequiredType", pointer + "/" + key, "expected string");
    }
    return value.string();
}

std::string sourcePath(
    const palmier::json::Object& source,
    const std::string& key,
    const std::string& pathKey,
    const std::string& pointer
) {
    const auto& value = requireField(source, key, pointer);
    const auto& object = requireObject(value, pointer + "/" + key);
    return requireString(object, pathKey, pointer + "/" + key);
}

MediaManifestSource decodeSource(
    const palmier::json::Value& value,
    const std::string& pointer
) {
    const auto& source = requireObject(value, pointer);
    const bool hasExternal = source.contains("external");
    const bool hasProject = source.contains("project");
    if (hasExternal == hasProject) {
        fail(
            "invalidMediaSource",
            pointer,
            "media source must contain exactly one known source case"
        );
    }
    if (hasExternal) {
        return {
            MediaSourceKind::external,
            sourcePath(source, "external", "absolutePath", pointer),
        };
    }
    return {
        MediaSourceKind::project,
        sourcePath(source, "project", "relativePath", pointer),
    };
}

std::optional<bool> optionalBoolean(
    const palmier::json::Object& object,
    const std::string& key,
    const std::string& pointer
) {
    const auto field = object.find(key);
    if (field == object.end() || field->second.kind() == JsonKind::nullValue) {
        return std::nullopt;
    }
    if (field->second.kind() != JsonKind::boolean) {
        fail("wrongOptionalType", pointer + "/" + key, "expected boolean or null");
    }
    return field->second.boolean();
}

void validateEntryFields(
    const palmier::json::Object& object,
    const std::string& pointer
) {
    static const std::set<std::string> mediaTypes{
        "video", "audio", "image", "text", "lottie", "sequence",
    };
    static_cast<void>(requireString(object, "name", pointer));
    const auto type = requireString(object, "type", pointer);
    if (!mediaTypes.contains(type)) {
        fail("unsupportedRequiredEnum", pointer + "/type", "unsupported media type");
    }
    const auto& duration = requireField(object, "duration", pointer);
    if (duration.kind() != JsonKind::number) {
        fail("wrongRequiredType", pointer + "/duration", "expected number");
    }
    double parsed{};
    const auto& lexeme = duration.number().lexeme;
    const auto conversion = std::from_chars(
        lexeme.data(),
        lexeme.data() + lexeme.size(),
        parsed
    );
    if (
        conversion.ec != std::errc{}
        || conversion.ptr != lexeme.data() + lexeme.size()
        || !std::isfinite(parsed)
        || parsed < 0
    ) {
        fail("invalidMediaDuration", pointer + "/duration", "duration must be finite and >= 0");
    }
}

MediaManifest decodeManifest(
    const palmier::json::Value& value,
    std::stop_token cancellation
) {
    checkCancellation(cancellation);
    const auto& root = requireObject(value, "");
    const auto version = root.find("version");
    if (version != root.end()) {
        if (
            version->second.kind() != JsonKind::number
            || !version->second.number().integer
            || *version->second.number().integer < 1
        ) {
            fail("invalidManifestVersion", "/version", "manifest version must be an integer >= 1");
        }
    }

    const auto entriesField = root.find("entries");
    if (entriesField == root.end()) return {};
    if (entriesField->second.kind() != JsonKind::array) {
        fail("wrongRequiredType", "/entries", "expected array");
    }

    MediaManifest manifest;
    const auto& entries = entriesField->second.array();
    manifest.entries.reserve(entries.size());
    for (std::size_t index = 0; index < entries.size(); ++index) {
        checkCancellation(cancellation);
        const auto pointer = "/entries/" + std::to_string(index);
        const auto& object = requireObject(entries[index], pointer);
        validateEntryFields(object, pointer);
        manifest.entries.push_back({
            requireString(object, "id", pointer),
            requireString(object, "type", pointer),
            decodeSource(requireField(object, "source", pointer), pointer + "/source"),
            optionalBoolean(object, "hasAudio", pointer),
        });
    }
    checkCancellation(cancellation);
    return manifest;
}

std::string readManifestJson(
    const std::filesystem::path& path,
    const MediaManifestReadOptions& options
) {
    std::ifstream stream(path, std::ios::binary);
    checkCancellation(options.cancellation);
    if (!stream) fail("mediaJsonOpenFailed", "", "cannot open media.json");

    std::string source;
    std::array<char, 64U * 1024U> buffer{};
    for (;;) {
        checkCancellation(options.cancellation);
        stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        checkCancellation(options.cancellation);
        const auto count = stream.gcount();
        if (count < 0) fail("mediaJsonReadFailed", "", "cannot read media.json");
        const auto byteCount = static_cast<std::size_t>(count);
        if (
            byteCount > options.maximumMediaJsonBytes
            || source.size() > options.maximumMediaJsonBytes - byteCount
        ) {
            fail("mediaJsonTooLarge", "", "media.json exceeds the configured size limit");
        }
        if (byteCount > 0) source.append(buffer.data(), byteCount);
        if (stream.bad() || (stream.fail() && !stream.eof())) {
            fail("mediaJsonReadFailed", "", "cannot read media.json");
        }
        if (stream.eof()) break;
        if (byteCount == 0) {
            fail("mediaJsonReadFailed", "", "media.json read made no progress");
        }
    }
    return source;
}

}

MediaManifestReadError::MediaManifestReadError(
    std::string codeValue,
    std::string jsonPointerValue,
    std::string detail
) : std::runtime_error(std::move(detail)),
    code(std::move(codeValue)),
    jsonPointer(std::move(jsonPointerValue)) {}

std::optional<MediaManifest> readMediaManifest(
    const std::filesystem::path& packagePath,
    MediaManifestReadOptions options
) {
    checkCancellation(options.cancellation);
    if (options.maximumMediaJsonBytes == 0) {
        fail("invalidReadLimit", "", "media.json size limit must be positive");
    }
    const auto path = packagePath / "media.json";
    std::error_code statusError;
    const bool exists = std::filesystem::exists(path, statusError);
    checkCancellation(options.cancellation);
    if (statusError) fail("mediaJsonOpenFailed", "", "cannot inspect media.json");
    if (!exists) return std::nullopt;
    const bool isRegularFile = std::filesystem::is_regular_file(path, statusError);
    checkCancellation(options.cancellation);
    if (statusError || !isRegularFile) {
        fail("mediaJsonOpenFailed", "", "media.json is not a readable regular file");
    }
    const auto source = readManifestJson(path, options);
    try {
        return decodeManifest(
            palmier::json::parse(source, options.cancellation),
            options.cancellation
        );
    } catch (const palmier::json::Error& error) {
        checkCancellation(options.cancellation);
        fail("invalidMediaJson", "", error.what());
    }
}

}
