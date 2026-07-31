#include "palmier/contracts/top_level_json.hpp"

#include <charconv>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <sstream>
#include <system_error>

namespace palmier::contracts {
namespace {

class Parser final {
public:
    explicit Parser(std::string_view source) : source_(source) {
        if (source_.starts_with("\xEF\xBB\xBF")) {
            position_ = 3;
        }
    }

    TopLevelJsonObject parseTopLevelObject() {
        skipWhitespace();
        expect('{');
        TopLevelJsonObject result;
        skipWhitespace();
        if (consume('}')) {
            finish();
            return result;
        }

        while (true) {
            skipWhitespace();
            const auto key = parseString();
            if (result.contains(key)) {
                fail("duplicate top-level key '" + key + "'");
            }
            skipWhitespace();
            expect(':');
            skipWhitespace();
            result.emplace(key, parseValue(1));
            skipWhitespace();
            if (consume('}')) {
                finish();
                return result;
            }
            expect(',');
        }
    }

private:
    static constexpr std::size_t maximumDepth = 256;

    JsonValueSummary parseValue(std::size_t depth) {
        if (depth > maximumDepth) {
            fail("maximum nesting depth exceeded");
        }
        if (position_ >= source_.size()) {
            fail("expected a JSON value");
        }
        switch (source_[position_]) {
        case 'n':
            expectLiteral("null");
            return {JsonValueKind::nullValue, std::nullopt, std::nullopt, std::nullopt};
        case 't':
            expectLiteral("true");
            return {JsonValueKind::boolean, std::nullopt, true, std::nullopt};
        case 'f':
            expectLiteral("false");
            return {JsonValueKind::boolean, std::nullopt, false, std::nullopt};
        case '"':
            return {JsonValueKind::string, std::nullopt, std::nullopt, parseString()};
        case '[':
            parseArray(depth + 1);
            return {JsonValueKind::array, std::nullopt, std::nullopt, std::nullopt};
        case '{':
            parseObject(depth + 1);
            return {JsonValueKind::object, std::nullopt, std::nullopt, std::nullopt};
        default:
            return parseNumber();
        }
    }

    void parseArray(std::size_t depth) {
        expect('[');
        skipWhitespace();
        if (consume(']')) {
            return;
        }
        while (true) {
            skipWhitespace();
            parseValue(depth);
            skipWhitespace();
            if (consume(']')) {
                return;
            }
            expect(',');
        }
    }

    void parseObject(std::size_t depth) {
        expect('{');
        std::set<std::string> keys;
        skipWhitespace();
        if (consume('}')) {
            return;
        }
        while (true) {
            skipWhitespace();
            const auto key = parseString();
            if (!keys.insert(key).second) {
                fail("duplicate object key '" + key + "'");
            }
            skipWhitespace();
            expect(':');
            skipWhitespace();
            parseValue(depth);
            skipWhitespace();
            if (consume('}')) {
                return;
            }
            expect(',');
        }
    }

    JsonValueSummary parseNumber() {
        const auto start = position_;
        consume('-');
        if (consume('0')) {
            if (position_ < source_.size() && isDigit(source_[position_])) {
                fail("leading zero in number");
            }
        } else {
            requireDigits();
        }

        bool integer = true;
        if (consume('.')) {
            integer = false;
            requireDigits();
        }
        if (consume('e') || consume('E')) {
            integer = false;
            if (!consume('+')) {
                consume('-');
            }
            requireDigits();
        }

        std::optional<std::int64_t> value;
        if (integer) {
            std::int64_t parsed = 0;
            const auto* begin = source_.data() + start;
            const auto* end = source_.data() + position_;
            const auto conversion = std::from_chars(begin, end, parsed);
            if (conversion.ec == std::errc::result_out_of_range) {
                fail("integer is outside the int64 range");
            }
            if (conversion.ec != std::errc{} || conversion.ptr != end) {
                fail("invalid integer");
            }
            value = parsed;
        }
        return {JsonValueKind::number, value, std::nullopt, std::nullopt};
    }

    std::string parseString() {
        expect('"');
        std::string result;
        while (position_ < source_.size()) {
            const auto character = static_cast<unsigned char>(source_[position_++]);
            if (character == '"') {
                return result;
            }
            if (character < 0x20) {
                fail("unescaped control character in string");
            }
            if (character >= 0x80) {
                appendRawUtf8(result, character);
                continue;
            }
            if (character != '\\') {
                result.push_back(static_cast<char>(character));
                continue;
            }
            if (position_ >= source_.size()) {
                fail("unterminated string escape");
            }
            switch (source_[position_++]) {
            case '"': result.push_back('"'); break;
            case '\\': result.push_back('\\'); break;
            case '/': result.push_back('/'); break;
            case 'b': result.push_back('\b'); break;
            case 'f': result.push_back('\f'); break;
            case 'n': result.push_back('\n'); break;
            case 'r': result.push_back('\r'); break;
            case 't': result.push_back('\t'); break;
            case 'u': appendEscapedCodePoint(result); break;
            default: fail("invalid string escape");
            }
        }
        fail("unterminated string");
    }

    void appendRawUtf8(std::string& output, unsigned char lead) {
        output.push_back(static_cast<char>(lead));
        if (lead >= 0xC2 && lead <= 0xDF) {
            appendContinuation(output, 0x80, 0xBF);
            return;
        }
        if (lead == 0xE0) {
            appendContinuation(output, 0xA0, 0xBF);
            appendContinuation(output, 0x80, 0xBF);
            return;
        }
        if ((lead >= 0xE1 && lead <= 0xEC) || (lead >= 0xEE && lead <= 0xEF)) {
            appendContinuation(output, 0x80, 0xBF);
            appendContinuation(output, 0x80, 0xBF);
            return;
        }
        if (lead == 0xED) {
            appendContinuation(output, 0x80, 0x9F);
            appendContinuation(output, 0x80, 0xBF);
            return;
        }
        if (lead == 0xF0) {
            appendContinuation(output, 0x90, 0xBF);
            appendContinuation(output, 0x80, 0xBF);
            appendContinuation(output, 0x80, 0xBF);
            return;
        }
        if (lead >= 0xF1 && lead <= 0xF3) {
            appendContinuation(output, 0x80, 0xBF);
            appendContinuation(output, 0x80, 0xBF);
            appendContinuation(output, 0x80, 0xBF);
            return;
        }
        if (lead == 0xF4) {
            appendContinuation(output, 0x80, 0x8F);
            appendContinuation(output, 0x80, 0xBF);
            appendContinuation(output, 0x80, 0xBF);
            return;
        }
        fail("invalid UTF-8 leading byte");
    }

    void appendContinuation(
        std::string& output,
        unsigned char minimum,
        unsigned char maximum
    ) {
        if (position_ >= source_.size()) {
            fail("incomplete UTF-8 sequence");
        }
        const auto byte = static_cast<unsigned char>(source_[position_++]);
        if (byte < minimum || byte > maximum) {
            fail("invalid UTF-8 continuation byte");
        }
        output.push_back(static_cast<char>(byte));
    }

    void appendEscapedCodePoint(std::string& output) {
        auto codePoint = parseHexQuad();
        if (codePoint >= 0xD800 && codePoint <= 0xDBFF) {
            if (position_ + 2 > source_.size() || source_.substr(position_, 2) != "\\u") {
                fail("high surrogate without low surrogate");
            }
            position_ += 2;
            const auto low = parseHexQuad();
            if (low < 0xDC00 || low > 0xDFFF) {
                fail("invalid low surrogate");
            }
            codePoint = 0x10000 + ((codePoint - 0xD800) << 10) + (low - 0xDC00);
        } else if (codePoint >= 0xDC00 && codePoint <= 0xDFFF) {
            fail("unexpected low surrogate");
        }
        appendUtf8(output, codePoint);
    }

    std::uint32_t parseHexQuad() {
        if (position_ + 4 > source_.size()) {
            fail("incomplete unicode escape");
        }
        std::uint32_t value = 0;
        for (int index = 0; index < 4; ++index) {
            value = (value << 4) | hexValue(source_[position_++]);
        }
        return value;
    }

    static void appendUtf8(std::string& output, std::uint32_t value) {
        if (value <= 0x7F) {
            output.push_back(static_cast<char>(value));
        } else if (value <= 0x7FF) {
            output.push_back(static_cast<char>(0xC0 | (value >> 6)));
            output.push_back(static_cast<char>(0x80 | (value & 0x3F)));
        } else if (value <= 0xFFFF) {
            output.push_back(static_cast<char>(0xE0 | (value >> 12)));
            output.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | (value & 0x3F)));
        } else {
            output.push_back(static_cast<char>(0xF0 | (value >> 18)));
            output.push_back(static_cast<char>(0x80 | ((value >> 12) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | (value & 0x3F)));
        }
    }

    static std::uint32_t hexValue(char character) {
        if (character >= '0' && character <= '9') {
            return static_cast<std::uint32_t>(character - '0');
        }
        if (character >= 'a' && character <= 'f') {
            return static_cast<std::uint32_t>(character - 'a' + 10);
        }
        if (character >= 'A' && character <= 'F') {
            return static_cast<std::uint32_t>(character - 'A' + 10);
        }
        throw JsonError("invalid hexadecimal digit in unicode escape");
    }

    void requireDigits() {
        const auto start = position_;
        while (position_ < source_.size() && isDigit(source_[position_])) {
            ++position_;
        }
        if (position_ == start) {
            fail("expected digit in number");
        }
    }

    static bool isDigit(char character) {
        return character >= '0' && character <= '9';
    }

    void expectLiteral(std::string_view literal) {
        if (source_.substr(position_, literal.size()) != literal) {
            fail("invalid JSON literal");
        }
        position_ += literal.size();
    }

    void finish() {
        skipWhitespace();
        if (position_ != source_.size()) {
            fail("trailing content after JSON document");
        }
    }

    void skipWhitespace() {
        while (position_ < source_.size()) {
            const auto character = source_[position_];
            if (character != ' ' && character != '\t' && character != '\r' && character != '\n') {
                return;
            }
            ++position_;
        }
    }

    bool consume(char expected) {
        if (position_ < source_.size() && source_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    void expect(char expected) {
        if (!consume(expected)) {
            fail(std::string("expected '") + expected + "'");
        }
    }

    [[noreturn]] void fail(const std::string& detail) const {
        throw JsonError(detail + " at byte " + std::to_string(position_));
    }

    std::string_view source_;
    std::size_t position_ = 0;
};

}

TopLevelJsonObject parseTopLevelJsonObject(std::string_view source) {
    return Parser(source).parseTopLevelObject();
}

std::string pathForDiagnostic(const std::filesystem::path& path) noexcept {
    try {
        const auto utf8 = path.generic_u8string();
        return {
            reinterpret_cast<const char*>(utf8.data()),
            utf8.size(),
        };
    } catch (...) {
        return "<unprintable-path>";
    }
}

TopLevelJsonObject readTopLevelJsonObject(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::ios_base::failure("cannot open " + pathForDiagnostic(path));
    }
    const std::string source{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()
    };
    if (!stream.eof() && stream.fail()) {
        throw std::ios_base::failure("cannot read " + pathForDiagnostic(path));
    }
    return parseTopLevelJsonObject(source);
}

}
