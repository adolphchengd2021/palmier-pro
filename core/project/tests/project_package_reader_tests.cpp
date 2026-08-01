#include "palmier/project/project_package_reader.hpp"
#include "palmier/project/media_manifest_reader.hpp"

#include "project_package_reader_testing.hpp"

#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace {

using palmier::project::ProjectPackageReadError;
using palmier::project::testing::ProjectPackageReadCheckpoint;
using palmier::project::testing::ProjectPackageReadCheckpoints;
using palmier::json::Value;

constexpr std::string_view validProject =
    R"({"timelines":[{"id":"timeline","fps":30,"width":1920,"height":1080,"tracks":[]}]})";

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

const Value& at(const Value& value, const std::string& key) {
    const auto* child = value.find(key);
    if (child == nullptr) {
        throw std::runtime_error("missing JSON field " + key);
    }
    return *child;
}

class TemporaryDirectory final {
public:
    explicit TemporaryDirectory(std::string_view extension = ".palmier") {
        std::random_device random;
        for (int attempt = 0; attempt < 32; ++attempt) {
            const auto candidate = std::filesystem::temp_directory_path()
                / ("palmier-project-package-tests-" + std::to_string(random())
                    + "-" + std::to_string(random()) + std::string(extension));
            std::error_code error;
            if (std::filesystem::create_directory(candidate, error)) {
                path_ = candidate;
                return;
            }
        }
        throw std::runtime_error("cannot create unique temporary package");
    }

    ~TemporaryDirectory() {
        if (!path_.empty()) {
            std::error_code error;
            std::filesystem::remove_all(path_, error);
        }
    }

    const std::filesystem::path& path() const noexcept { return path_; }

    void write(std::string_view content) const {
        write("project.json", content);
    }

    void write(std::string_view name, std::string_view content) const {
        std::ofstream stream(path_ / name, std::ios::binary);
        if (!stream) {
            throw std::runtime_error("cannot create package fixture");
        }
        stream.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!stream) {
            throw std::runtime_error("cannot write package fixture");
        }
    }

private:
    std::filesystem::path path_;
};

template<typename Operation>
void requirePackageError(Operation operation, const std::string& code) {
    try {
        operation();
    } catch (const ProjectPackageReadError& error) {
        require(error.code == code, "unexpected package error code");
        return;
    }
    throw std::runtime_error("expected project package failure");
}

template<typename Operation>
void requireManifestError(
    Operation operation,
    const std::string& code,
    const std::string& pointer = {}
) {
    try {
        operation();
    } catch (const palmier::project::MediaManifestReadError& error) {
        require(error.code == code, "unexpected manifest error code");
        require(error.jsonPointer == pointer, "unexpected manifest error pointer");
        return;
    }
    throw std::runtime_error("expected media manifest failure");
}

class CancellingCheckpoint final : public ProjectPackageReadCheckpoints {
public:
    CancellingCheckpoint(ProjectPackageReadCheckpoint target, std::stop_source& source)
        : target_(target), source_(source) {}

    void arrive(ProjectPackageReadCheckpoint checkpoint) override {
        if (checkpoint == target_) {
            source_.request_stop();
        }
    }

private:
    ProjectPackageReadCheckpoint target_;
    std::stop_source& source_;
};

class GrowingFileCheckpoint final : public ProjectPackageReadCheckpoints {
public:
    explicit GrowingFileCheckpoint(std::filesystem::path path) : path_(std::move(path)) {}

    void arrive(ProjectPackageReadCheckpoint checkpoint) override {
        if (checkpoint != ProjectPackageReadCheckpoint::afterChunk || appended_) {
            return;
        }
        appended_ = true;
        std::ofstream stream(path_, std::ios::binary | std::ios::app);
        if (!stream) {
            throw std::runtime_error("cannot append growing fixture");
        }
        const std::string growth(32, ' ');
        stream.write(growth.data(), static_cast<std::streamsize>(growth.size()));
        if (!stream) {
            throw std::runtime_error("cannot grow project.json fixture");
        }
    }

private:
    std::filesystem::path path_;
    bool appended_{};
};

auto generatedId() {
    return [] { return std::string("generated-id"); };
}

std::string projectWithCanonicalClips(std::size_t clipCount) {
    std::string source = R"({"timelines":[{"id":"timeline","name":"Timeline","fps":30,"width":1920,"height":1080,"settingsConfigured":true,"tracks":[{"id":"track","type":"video","muted":false,"hidden":false,"syncLocked":true,"displayHeight":50,"clips":[)";
    for (std::size_t index = 0; index < clipCount; ++index) {
        if (index > 0) source += ',';
        source += R"({"id":"clip-)" + std::to_string(index)
            + R"(","mediaRef":"media","mediaType":"video","sourceClipType":"video","startFrame":)"
            + std::to_string(index)
            + R"(,"durationFrames":1,"trimStartFrame":0,"trimEndFrame":0,"speed":1,"volume":1,"opacity":1,"blendMode":"normal","linkGroupId":null,"captionGroupId":null,"multicamGroupId":null})";
    }
    source += R"(]}]}],"activeTimelineId":"timeline","openTimelineIds":["timeline"]})";
    return source;
}

void readsValidPackage() {
    TemporaryDirectory package;
    package.write(validProject);
    const auto document = palmier::project::readProjectPackage(package.path(), generatedId());
    require(document.project().activeTimelineId == "timeline", "wrong loaded timeline");
    require(
        palmier::project::defaultMaximumProjectJsonBytes == 64U * 1024U * 1024U,
        "wrong default project.json limit"
    );
}

void readsRepositoryPackages(const std::filesystem::path& root) {
    const auto fixtures = root / "fixtures/contracts/projects";
    const auto current = palmier::project::readProjectPackage(
        fixtures / "current-multitimeline.palmier",
        generatedId()
    );
    require(current.rootKind() == palmier::project::RootKind::current, "wrong current root");
    require(current.project().timelines.size() == 2, "wrong current timeline count");

    const auto legacy = palmier::project::readProjectPackage(
        fixtures / "legacy-bare-timeline.palmier",
        generatedId()
    );
    require(legacy.rootKind() == palmier::project::RootKind::legacy, "wrong legacy root");
    require(legacy.project().activeTimelineId == "timeline-legacy", "wrong legacy timeline");

    const auto unknown = palmier::project::readProjectPackage(
        fixtures / "unknown-fields.palmier",
        generatedId()
    );
    require(
        at(unknown.source(), "x-contract-null").kind() == Value::Kind::nullValue,
        "unknown package field was lost"
    );
}

void readsMediaManifestContract(const std::filesystem::path& root) {
    const auto package = root / "fixtures/contracts/projects/current-multitimeline.palmier";
    const auto manifest = palmier::project::readMediaManifest(package);
    require(manifest.has_value(), "repository media manifest was not found");
    require(manifest->entries.size() == 1, "wrong media entry count");
    const auto& entry = manifest->entries.front();
    require(entry.id == "media-main-1", "media ID changed");
    require(entry.type == "video", "media type changed");
    require(
        entry.source.kind == palmier::project::MediaSourceKind::project,
        "media source kind changed"
    );
    require(entry.source.path == "media/main.mp4", "media source path changed");
    require(entry.hasAudio == true, "media audio hint changed");

    TemporaryDirectory missing;
    require(
        !palmier::project::readMediaManifest(missing.path()),
        "missing manifest did not return empty"
    );
}

void validatesMediaManifestContract() {
    TemporaryDirectory package;
    package.write(
        "media.json",
        R"({"entries":[{"id":"first","name":"First","type":"video","source":{"external":{"absolutePath":"C:\\first.mov"}},"duration":1},{"id":"first","name":"Duplicate","type":"video","source":{"project":{"relativePath":"media/duplicate.mov"}},"duration":1}]})"
    );
    const auto manifest = palmier::project::readMediaManifest(package.path());
    require(manifest && manifest->entries.size() == 2, "duplicate media IDs were not preserved");
    require(manifest->entries.front().source.path == "C:\\first.mov", "first media entry changed");

    package.write("media.json", R"({"version":0,"entries":[]})");
    requireManifestError(
        [&] { static_cast<void>(palmier::project::readMediaManifest(package.path())); },
        "invalidManifestVersion",
        "/version"
    );
    package.write(
        "media.json",
        R"({"entries":[{"id":"both","name":"Both","type":"video","source":{"external":{"absolutePath":"C:\\a.mov"},"project":{"relativePath":"media/a.mov"}},"duration":1}]})"
    );
    requireManifestError(
        [&] { static_cast<void>(palmier::project::readMediaManifest(package.path())); },
        "invalidMediaSource",
        "/entries/0/source"
    );
    package.write("media.json", R"({"entries":[]})");
    auto options = palmier::project::MediaManifestReadOptions{};
    options.maximumMediaJsonBytes = 1;
    requireManifestError(
        [&] { static_cast<void>(palmier::project::readMediaManifest(package.path(), options)); },
        "mediaJsonTooLarge"
    );
    std::stop_source cancellation;
    cancellation.request_stop();
    requireManifestError(
        [&] {
            static_cast<void>(palmier::project::readMediaManifest(
                package.path(),
                {.cancellation = cancellation.get_token()}
            ));
        },
        "cancelled"
    );
}

void validatesPackageAndLimit() {
    TemporaryDirectory package;
    package.write(validProject);
    auto options = palmier::project::ProjectPackageReadOptions{};
    options.maximumProjectJsonBytes = validProject.size();
    const auto exact = palmier::project::readProjectPackage(
        package.path(),
        generatedId(),
        options
    );
    require(exact.project().activeTimelineId == "timeline", "exact limit read failed");

    options.maximumProjectJsonBytes = validProject.size() - 1;
    requirePackageError(
        [&] { palmier::project::readProjectPackage(package.path(), generatedId(), options); },
        "projectJsonTooLarge"
    );
    options.maximumProjectJsonBytes = 0;
    requirePackageError(
        [&] { palmier::project::readProjectPackage(package.path(), generatedId(), options); },
        "invalidReadLimit"
    );
    requirePackageError(
        [&] {
            palmier::project::readProjectPackage(
                package.path() / "missing.palmier",
                generatedId()
            );
        },
        "invalidPackagePath"
    );

    TemporaryDirectory nonPackage("");
    requirePackageError(
        [&] { palmier::project::readProjectPackage(nonPackage.path(), generatedId()); },
        "invalidPackagePath"
    );

    TemporaryDirectory uppercasePackage(".PALMIER");
    uppercasePackage.write(validProject);
    const auto uppercase = palmier::project::readProjectPackage(
        uppercasePackage.path(),
        generatedId()
    );
    require(uppercase.project().activeTimelineId == "timeline", "uppercase extension rejected");
}

void validatesJsonComplexityBudgets() {
    TemporaryDirectory package;
    package.write(validProject);

    auto options = palmier::project::ProjectPackageReadOptions{};
    options.maximumProjectJsonValues = 8;
    options.maximumProjectJsonStringBytes = 39;
    const auto exact = palmier::project::readProjectPackage(
        package.path(),
        generatedId(),
        options
    );
    require(exact.project().activeTimelineId == "timeline", "exact complexity limit failed");

    options.maximumProjectJsonValues = 7;
    requirePackageError(
        [&] { palmier::project::readProjectPackage(package.path(), generatedId(), options); },
        "projectJsonTooComplex"
    );

    options.maximumProjectJsonValues = 8;
    options.maximumProjectJsonStringBytes = 38;
    requirePackageError(
        [&] { palmier::project::readProjectPackage(package.path(), generatedId(), options); },
        "projectJsonTooComplex"
    );
}

void defaultBudgetAcceptsMaximumTimelineProjection() {
    TemporaryDirectory package;
    package.write(projectWithCanonicalClips(10'000));
    const auto document = palmier::project::readProjectPackage(
        package.path(),
        generatedId()
    );
    require(
        document.project().timelines.front().tracks.front().clips.size() == 10'000,
        "default complexity budget rejected the maximum timeline projection"
    );
}

void validatesProjectJsonFile() {
    TemporaryDirectory missing;
    requirePackageError(
        [&] { palmier::project::readProjectPackage(missing.path(), generatedId()); },
        "projectJsonOpenFailed"
    );

    TemporaryDirectory directory;
    std::filesystem::create_directory(directory.path() / "project.json");
    requirePackageError(
        [&] { palmier::project::readProjectPackage(directory.path(), generatedId()); },
        "projectJsonOpenFailed"
    );

    for (const std::string_view content : {std::string_view{}, std::string_view{"{"}}) {
        TemporaryDirectory invalid;
        invalid.write(content);
        try {
            palmier::project::readProjectPackage(invalid.path(), generatedId());
        } catch (const palmier::json::Error&) {
            continue;
        }
        throw std::runtime_error("expected empty or malformed JSON failure");
    }

    TemporaryDirectory invalidModel;
    invalidModel.write(R"({"timelines":[]})");
    try {
        palmier::project::readProjectPackage(invalidModel.path(), generatedId());
    } catch (const palmier::project::ReadError& error) {
        require(error.code == "emptyTimelines", "unexpected project model error");
        return;
    }
    throw std::runtime_error("expected project model failure");
}

void cancellationBoundaries() {
    TemporaryDirectory package;
    package.write(validProject);

    std::stop_source preCancelled;
    preCancelled.request_stop();
    requirePackageError(
        [&] {
            palmier::project::readProjectPackage(
                package.path() / "missing.palmier",
                generatedId(),
                {.cancellation = preCancelled.get_token()}
            );
        },
        "cancelled"
    );

    for (const auto checkpoint : {
        ProjectPackageReadCheckpoint::afterOpen,
        ProjectPackageReadCheckpoint::afterChunk,
        ProjectPackageReadCheckpoint::beforeParse,
        ProjectPackageReadCheckpoint::afterParse,
    }) {
        std::stop_source source;
        CancellingCheckpoint checkpoints(checkpoint, source);
        requirePackageError(
            [&] {
                palmier::project::testing::readProjectPackage(
                    package.path(),
                    generatedId(),
                    {.cancellation = source.get_token()},
                    &checkpoints
                );
            },
            "cancelled"
        );
    }
}

void cancellationDuringJsonParse() {
    TemporaryDirectory package;
    package.write(validProject);
    std::stop_source source;
    CancellingCheckpoint checkpoints(ProjectPackageReadCheckpoint::duringParse, source);
    requirePackageError(
        [&] {
            palmier::project::testing::readProjectPackage(
                package.path(),
                generatedId(),
                {.cancellation = source.get_token()},
                &checkpoints
            );
        },
        "cancelled"
    );
}

void cancellationDuringModelNormalization() {
    TemporaryDirectory package;
    package.write(R"({"timelines":[{"name":"Timeline","fps":30,"width":1920,"height":1080,"tracks":[]}],"activeTimelineId":null,"openTimelineIds":[]})");
    std::stop_source source;
    requirePackageError(
        [&] {
            palmier::project::readProjectPackage(
                package.path(),
                [&source] {
                    source.request_stop();
                    return std::string("generated");
                },
                {.cancellation = source.get_token()}
            );
        },
        "cancelled"
    );
}

void growingFileCannotCrossLimit() {
    TemporaryDirectory package;
    const auto chunkBytes = palmier::project::testing::projectJsonReadChunkBytes;
    std::string initial(chunkBytes, ' ');
    initial.front() = '{';
    package.write(initial);
    GrowingFileCheckpoint checkpoints(package.path() / "project.json");
    auto options = palmier::project::ProjectPackageReadOptions{};
    options.maximumProjectJsonBytes = chunkBytes + 16;
    requirePackageError(
        [&] {
            palmier::project::testing::readProjectPackage(
                package.path(),
                generatedId(),
                options,
                &checkpoints
            );
        },
        "projectJsonTooLarge"
    );
}

void runTests(const std::filesystem::path& root) {
    readsValidPackage();
    readsRepositoryPackages(root);
    readsMediaManifestContract(root);
    validatesMediaManifestContract();
    validatesPackageAndLimit();
    validatesJsonComplexityBudgets();
    defaultBudgetAcceptsMaximumTimelineProjection();
    validatesProjectJsonFile();
    cancellationBoundaries();
    cancellationDuringJsonParse();
    cancellationDuringModelNormalization();
    growingFileCannotCrossLimit();
}

}

int wmain(int argumentCount, wchar_t* arguments[]) {
    if (argumentCount != 2) {
        std::cerr << "PALMIER_PROJECT_PACKAGE_READER_TESTS_FAILED expected repository root\n";
        return 1;
    }
    const std::filesystem::path root(arguments[1]);
    std::exception_ptr failure;
    std::jthread worker([&root, &failure] {
        try {
            runTests(root);
        } catch (...) {
            failure = std::current_exception();
        }
    });
    worker.join();

    if (failure) {
        try {
            std::rethrow_exception(failure);
        } catch (const std::exception& error) {
            std::cerr << "PALMIER_PROJECT_PACKAGE_READER_TESTS_FAILED " << error.what() << '\n';
            return 1;
        }
    }
    std::cout << "PALMIER_PROJECT_PACKAGE_READER_TESTS_OK\n";
    return 0;
}
