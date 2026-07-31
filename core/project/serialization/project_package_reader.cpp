#include "palmier/project/project_package_reader.hpp"

#include "project_package_reader_testing.hpp"
#include "json_document_testing.hpp"

#include <array>
#include <fstream>
#include <string_view>
#include <system_error>
#include <utility>

namespace palmier::project {
namespace {

[[noreturn]] void fail(std::string code, std::string detail) {
    throw ProjectPackageReadError(std::move(code), std::move(detail));
}

void checkCancellation(std::stop_token cancellation) {
    if (cancellation.stop_requested()) {
        fail("cancelled", "project package read was cancelled");
    }
}

void arrive(
    testing::ProjectPackageReadCheckpoints* checkpoints,
    testing::ProjectPackageReadCheckpoint checkpoint
) {
    if (checkpoints != nullptr) {
        checkpoints->arrive(checkpoint);
    }
}

bool hasPalmierExtension(const std::filesystem::path& path) {
    constexpr std::string_view expected = ".palmier";
    const auto extension = path.extension().native();
    if (extension.size() != expected.size()) {
        return false;
    }
    for (std::size_t index = 0; index < expected.size(); ++index) {
        auto character = extension[index];
        const auto upperA = static_cast<std::filesystem::path::value_type>('A');
        const auto upperZ = static_cast<std::filesystem::path::value_type>('Z');
        if (character >= upperA && character <= upperZ) {
            character = static_cast<std::filesystem::path::value_type>(
                character + static_cast<std::filesystem::path::value_type>('a' - 'A')
            );
        }
        if (character != static_cast<std::filesystem::path::value_type>(expected[index])) {
            return false;
        }
    }
    return true;
}

std::string readProjectJson(
    const std::filesystem::path& path,
    const ProjectPackageReadOptions& options,
    testing::ProjectPackageReadCheckpoints* checkpoints
) {
    std::error_code statusError;
    const bool isRegularFile = std::filesystem::is_regular_file(path, statusError);
    checkCancellation(options.cancellation);
    if (statusError || !isRegularFile) {
        fail("projectJsonOpenFailed", "project.json is not a readable regular file");
    }

    std::ifstream stream(path, std::ios::binary);
    checkCancellation(options.cancellation);
    if (!stream) {
        fail("projectJsonOpenFailed", "cannot open project.json");
    }
    arrive(checkpoints, testing::ProjectPackageReadCheckpoint::afterOpen);
    checkCancellation(options.cancellation);

    std::string source;
    std::array<char, testing::projectJsonReadChunkBytes> buffer{};
    for (;;) {
        checkCancellation(options.cancellation);
        stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        checkCancellation(options.cancellation);

        const auto count = stream.gcount();
        if (count < 0) {
            fail("projectJsonReadFailed", "cannot read project.json");
        }
        const auto byteCount = static_cast<std::size_t>(count);
        if (
            byteCount > options.maximumProjectJsonBytes
            || source.size() > options.maximumProjectJsonBytes - byteCount
        ) {
            fail("projectJsonTooLarge", "project.json exceeds the configured size limit");
        }
        if (byteCount > 0) {
            source.append(buffer.data(), byteCount);
            arrive(checkpoints, testing::ProjectPackageReadCheckpoint::afterChunk);
            checkCancellation(options.cancellation);
        }

        if (stream.bad() || (stream.fail() && !stream.eof())) {
            fail("projectJsonReadFailed", "cannot read project.json");
        }
        if (stream.eof()) {
            break;
        }
        if (byteCount == 0) {
            fail("projectJsonReadFailed", "project.json read made no progress");
        }
    }
    return source;
}

}

ProjectPackageReadError::ProjectPackageReadError(std::string codeValue, std::string detail)
    : std::runtime_error(std::move(detail)), code(std::move(codeValue)) {}

ProjectDocument readProjectPackage(
    const std::filesystem::path& packagePath,
    const IdGenerator& idGenerator,
    ProjectPackageReadOptions options
) {
    return testing::readProjectPackage(packagePath, idGenerator, options, nullptr);
}

namespace testing {

class JsonCheckpointAdapter final : public palmier::json::testing::ParseCheckpoints {
public:
    JsonCheckpointAdapter(
        ProjectPackageReadCheckpoints* checkpoints,
        const ProjectPackageReadOptions& options
    ) : checkpoints_(checkpoints), options_(options) {}

    void arrive(
        palmier::json::testing::ParseCheckpoint checkpoint,
        std::size_t amount
    ) override {
        if (checkpoint == palmier::json::testing::ParseCheckpoint::value) {
            if (
                amount > options_.maximumProjectJsonValues
                || valueCount_ > options_.maximumProjectJsonValues - amount
            ) {
                fail("projectJsonTooComplex", "project.json has too many JSON values");
            }
            valueCount_ += amount;
        } else if (checkpoint == palmier::json::testing::ParseCheckpoint::stringBytes) {
            if (
                amount > options_.maximumProjectJsonStringBytes
                || stringBytes_ > options_.maximumProjectJsonStringBytes - amount
            ) {
                fail("projectJsonTooComplex", "project.json has too much decoded string data");
            }
            stringBytes_ += amount;
        } else if (checkpoints_ != nullptr) {
            checkpoints_->arrive(ProjectPackageReadCheckpoint::duringParse);
        }
    }

private:
    ProjectPackageReadCheckpoints* checkpoints_;
    const ProjectPackageReadOptions& options_;
    std::size_t valueCount_{};
    std::size_t stringBytes_{};
};

ProjectDocument readProjectPackage(
    const std::filesystem::path& packagePath,
    const IdGenerator& idGenerator,
    ProjectPackageReadOptions options,
    ProjectPackageReadCheckpoints* checkpoints
) {
    checkCancellation(options.cancellation);
    if (
        options.maximumProjectJsonBytes == 0
        || options.maximumProjectJsonValues == 0
        || options.maximumProjectJsonStringBytes == 0
    ) {
        fail("invalidReadLimit", "project.json limits must be positive");
    }
    if (!hasPalmierExtension(packagePath)) {
        fail("invalidPackagePath", "project package path must end in .palmier");
    }

    std::error_code statusError;
    const bool isPackageDirectory = std::filesystem::is_directory(packagePath, statusError);
    checkCancellation(options.cancellation);
    if (statusError || !isPackageDirectory) {
        fail("invalidPackagePath", "project package path is not a readable directory");
    }

    const auto source = readProjectJson(packagePath / "project.json", options, checkpoints);
    arrive(checkpoints, ProjectPackageReadCheckpoint::beforeParse);
    checkCancellation(options.cancellation);
    ProjectDocument document = [&] {
        try {
            JsonCheckpointAdapter adapter(checkpoints, options);
            auto json = palmier::json::testing::parse(
                source,
                options.cancellation,
                &adapter
            );
            return readProject(std::move(json), idGenerator, options.cancellation);
        } catch (const ReadError& error) {
            if (error.code == "cancelled") {
                fail("cancelled", "project package read was cancelled");
            }
            throw;
        } catch (const palmier::json::Error&) {
            checkCancellation(options.cancellation);
            throw;
        }
    }();
    arrive(checkpoints, ProjectPackageReadCheckpoint::afterParse);
    checkCancellation(options.cancellation);
    return document;
}

}

}
