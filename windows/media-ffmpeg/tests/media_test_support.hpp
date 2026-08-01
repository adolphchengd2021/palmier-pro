#pragma once

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace palmier::media::test_support {

inline void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

inline std::vector<std::uint8_t> decodeBase64(std::string_view input) {
    constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::array<std::int16_t, 256> table{};
    table.fill(-1);
    for (std::size_t index = 0; index < alphabet.size(); ++index) {
        table[static_cast<std::uint8_t>(alphabet[index])] =
            static_cast<std::int16_t>(index);
    }

    std::vector<std::uint8_t> output;
    output.reserve(input.size() * 3 / 4);
    std::uint32_t accumulator = 0;
    int bitCount = 0;
    for (const char character : input) {
        if (character == '=') {
            break;
        }
        const auto value = table[static_cast<std::uint8_t>(character)];
        require(value >= 0, "fixture contains invalid base64");
        accumulator = (accumulator << 6) | static_cast<std::uint32_t>(value);
        bitCount += 6;
        if (bitCount >= 8) {
            bitCount -= 8;
            output.push_back(static_cast<std::uint8_t>(
                (accumulator >> bitCount) & 0xFFU
            ));
        }
    }
    return output;
}

class TemporaryDirectory final {
public:
    TemporaryDirectory()
        : path_(std::filesystem::temp_directory_path()
            / ("palmier-media-" + std::to_string(GetCurrentProcessId()))) {
        std::filesystem::create_directory(path_);
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    std::filesystem::path write(
        std::string_view name,
        std::string_view base64
    ) const {
        return write(name, decodeBase64(base64));
    }

    std::filesystem::path write(
        std::string_view name,
        const std::vector<std::uint8_t>& bytes
    ) const {
        const auto destination = path_ / name;
        std::ofstream output(destination, std::ios::binary | std::ios::trunc);
        require(output.is_open(), "fixture file could not be opened");
        output.write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size())
        );
        require(output.good(), "fixture file could not be written");
        return destination;
    }

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

inline std::vector<std::uint8_t> replaceBytes(
    std::string_view base64,
    const std::vector<std::uint8_t>& pattern,
    const std::vector<std::uint8_t>& replacement
) {
    require(pattern.size() == replacement.size(), "replacement size differs");
    auto bytes = decodeBase64(base64);
    const auto position = std::search(
        bytes.begin(),
        bytes.end(),
        pattern.begin(),
        pattern.end()
    );
    require(position != bytes.end(), "fixture mutation pattern is missing");
    std::copy(replacement.begin(), replacement.end(), position);
    return bytes;
}

}
