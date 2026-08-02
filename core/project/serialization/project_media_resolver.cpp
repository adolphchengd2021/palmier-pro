#include "palmier/project/project_media_resolver.hpp"

#include <algorithm>
#include <system_error>
#include <utility>

namespace palmier::project {
namespace {

[[noreturn]] void fail(std::string code, std::string detail) {
    throw ProjectMediaResolveError(std::move(code), std::move(detail));
}

void checkCancellation(std::stop_token cancellation) {
    if (cancellation.stop_requested()) {
        fail("cancelled", "media reference resolution was cancelled");
    }
}

bool containsParentTraversal(const std::filesystem::path& path) {
    return std::any_of(path.begin(), path.end(), [](const auto& component) {
        return component == "..";
    });
}

bool isContainedBy(
    const std::filesystem::path& candidate,
    const std::filesystem::path& packageRoot
) {
    const auto relative = candidate.lexically_relative(packageRoot);
    return !relative.empty() && !relative.is_absolute() && !containsParentTraversal(relative);
}

std::filesystem::path pathFromUtf8(std::string_view value) {
    return std::filesystem::path(std::u8string(value.begin(), value.end()));
}

}

ProjectMediaResolveError::ProjectMediaResolveError(
    std::string codeValue,
    std::string detail
) : std::runtime_error(std::move(detail)), code(std::move(codeValue)) {}

ResolvedProjectMediaReference resolveProjectMediaReference(
    const MediaManifest& manifest,
    std::string_view mediaId,
    std::string_view requiredType,
    const std::filesystem::path& packagePath,
    std::stop_token cancellation
) {
    checkCancellation(cancellation);
    const MediaManifestEntry* match = nullptr;
    for (const auto& entry : manifest.entries) {
        checkCancellation(cancellation);
        if (entry.id != mediaId) continue;
        if (match != nullptr) {
            fail("ambiguousMediaRef", "media reference matches multiple manifest entries");
        }
        match = &entry;
    }
    if (match == nullptr) {
        fail("mediaEntryMissing", "media reference is absent from the manifest");
    }
    if (match->type != requiredType) {
        fail("mediaTypeMismatch", "media reference type does not match the selected clip");
    }

    checkCancellation(cancellation);
    std::filesystem::path candidate;
    if (match->source.kind == MediaSourceKind::external) {
        candidate = pathFromUtf8(match->source.path);
        if (!candidate.is_absolute()) {
            fail("invalidMediaSourcePath", "external media source must be absolute");
        }
    } else {
        const auto relative = pathFromUtf8(match->source.path);
        if (
            relative.empty()
            || relative.is_absolute()
            || relative.has_root_name()
            || relative.has_root_directory()
            || containsParentTraversal(relative)
        ) {
            fail("invalidMediaSourcePath", "project media source must remain package-relative");
        }
        candidate = packagePath / relative;
    }

    std::error_code error;
    const auto canonicalCandidate = std::filesystem::weakly_canonical(candidate, error);
    checkCancellation(cancellation);
    if (error) {
        fail("mediaFileUnavailable", "media source path could not be resolved");
    }
    if (match->source.kind == MediaSourceKind::project) {
        const auto canonicalPackage = std::filesystem::weakly_canonical(packagePath, error);
        checkCancellation(cancellation);
        if (error) {
            fail("mediaFileUnavailable", "project package path could not be resolved");
        }
        if (!isContainedBy(canonicalCandidate, canonicalPackage)) {
            fail("invalidMediaSourcePath", "project media source escapes the package");
        }
    }
    const bool isRegularFile = std::filesystem::is_regular_file(canonicalCandidate, error);
    checkCancellation(cancellation);
    if (error || !isRegularFile) {
        fail("mediaFileUnavailable", "media source is not an available regular file");
    }
    return {canonicalCandidate, match->hasAudio};
}

}
