#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace palmier::json {

struct Number final {
    std::string lexeme;
    std::optional<std::int64_t> integer;
};

struct Value;
using Array = std::vector<Value>;
using Object = std::map<std::string, Value>;

struct Value final {
    using Storage = std::variant<std::nullptr_t, bool, Number, std::string, Array, Object>;

    enum class Kind {
        nullValue,
        boolean,
        number,
        string,
        array,
        object,
    };

    Value();
    explicit Value(bool value);
    explicit Value(Number value);
    explicit Value(const char* value);
    explicit Value(std::string value);
    explicit Value(Array value);
    explicit Value(Object value);

    Kind kind() const noexcept;
    bool boolean() const;
    const Number& number() const;
    const std::string& string() const;
    const Array& array() const;
    const Object& object() const;
    Array& array();
    Object& object();
    const Value* find(std::string_view key) const;

    Storage storage;
};

class Error final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

Value parse(std::string_view source);
Value parse(std::string_view source, std::stop_token cancellation);
Value read(const std::filesystem::path& path);
std::string canonical(const Value& value);
std::string canonical(const Value& value, std::stop_token cancellation);
std::string pathForDiagnostic(const std::filesystem::path& path) noexcept;

}
