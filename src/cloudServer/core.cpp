#include <cloudServer/core.hpp>

#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <variant>

namespace CloudServer {

namespace {

using JsonData = std::variant<std::nullptr_t, bool, double, std::string, JsonValue::Array, JsonValue::Object>;

const std::string emptyString;
const JsonValue::Array emptyArray;
const JsonValue::Object emptyObject;

bool startsWith(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool endsWith(std::string_view value, std::string_view suffix) {
    return value.size() >= suffix.size() && value.substr(value.size() - suffix.size()) == suffix;
}

std::vector<std::string> splitString(const std::string& value, char delimiter) {
    std::vector<std::string> parts;
    std::string current;
    for (char ch : value) {
        if (ch == delimiter) {
            parts.push_back(current);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    parts.push_back(current);
    return parts;
}

bool isDigits(const std::string& value) {
    return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isdigit(ch);
    });
}

bool hasLeadingZero(const std::string& value) {
    return value.size() > 1 && value.front() == '0';
}

std::string parseJsonStringLiteral(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        char ch = value[index];
        if (ch != '\\' || index + 1 >= value.size()) {
            result.push_back(ch);
            continue;
        }

        char escaped = value[++index];
        switch (escaped) {
            case '"': result.push_back('"'); break;
            case '\\': result.push_back('\\'); break;
            case '/': result.push_back('/'); break;
            case 'b': result.push_back('\b'); break;
            case 'f': result.push_back('\f'); break;
            case 'n': result.push_back('\n'); break;
            case 'r': result.push_back('\r'); break;
            case 't': result.push_back('\t'); break;
            case 'u':
                if (index + 4 < value.size()) {
                    result.push_back('?');
                    index += 4;
                }
                break;
            default:
                result.push_back(escaped);
                break;
        }
    }
    return result;
}

class JsonParser {
public:
    explicit JsonParser(const std::string& source) : source(source) {}

    JsonValue parse() {
        skipWhitespace();
        JsonValue value = parseValue();
        skipWhitespace();
        return value;
    }

private:
    const std::string& source;
    std::size_t position = 0;

    char peek() const {
        if (position >= source.size()) {
            return '\0';
        }
        return source[position];
    }

    char get() {
        if (position >= source.size()) {
            return '\0';
        }
        return source[position++];
    }

    void skipWhitespace() {
        while (position < source.size() && std::isspace(static_cast<unsigned char>(source[position]))) {
            ++position;
        }
    }

    bool consume(std::string_view expected) {
        if (source.compare(position, expected.size(), expected) != 0) {
            return false;
        }
        position += expected.size();
        return true;
    }

    JsonValue parseValue() {
        skipWhitespace();
        char ch = peek();
        if (ch == '{') return parseObject();
        if (ch == '[') return parseArray();
        if (ch == '"') return parseString();
        if (ch == '-' || std::isdigit(static_cast<unsigned char>(ch))) return parseNumber();
        if (consume("true")) return JsonValue(true);
        if (consume("false")) return JsonValue(false);
        consume("null");
        return JsonValue();
    }

    JsonValue parseObject() {
        get();
        JsonValue::Object object;
        skipWhitespace();
        if (peek() == '}') {
            get();
            return object;
        }

        while (position < source.size()) {
            skipWhitespace();
            JsonValue key = parseString();
            skipWhitespace();
            if (get() != ':') {
                return object;
            }

            object[key.asString()] = parseValue();
            skipWhitespace();
            char next = get();
            if (next == '}') {
                return object;
            }
            if (next != ',') {
                return object;
            }
        }
        return object;
    }

    JsonValue parseArray() {
        get();
        JsonValue::Array array;
        skipWhitespace();
        if (peek() == ']') {
            get();
            return array;
        }

        while (position < source.size()) {
            array.push_back(parseValue());
            skipWhitespace();
            char next = get();
            if (next == ']') {
                return array;
            }
            if (next != ',') {
                return array;
            }
        }
        return array;
    }

    JsonValue parseString() {
        if (get() != '"') {
            return std::string();
        }

        std::string raw;
        while (position < source.size()) {
            char ch = get();
            if (ch == '"') {
                return parseJsonStringLiteral(raw);
            }
            raw.push_back(ch);
            if (ch == '\\' && position < source.size()) {
                raw.push_back(get());
            }
        }
        return parseJsonStringLiteral(raw);
    }

    JsonValue parseNumber() {
        std::size_t start = position;
        if (peek() == '-') {
            get();
        }
        while (std::isdigit(static_cast<unsigned char>(peek()))) {
            get();
        }
        if (peek() == '.') {
            get();
            while (std::isdigit(static_cast<unsigned char>(peek()))) {
                get();
            }
        }
        if (peek() == 'e' || peek() == 'E') {
            get();
            if (peek() == '+' || peek() == '-') {
                get();
            }
            while (std::isdigit(static_cast<unsigned char>(peek()))) {
                get();
            }
        }

        try {
            return std::stod(source.substr(start, position - start));
        } catch (...) {
            return 0;
        }
    }
};

std::string escapeJsonString(const std::string& value) {
    std::ostringstream output;
    output << '"';
    for (unsigned char ch : value) {
        switch (ch) {
            case '"': output << "\\\""; break;
            case '\\': output << "\\\\"; break;
            case '\b': output << "\\b"; break;
            case '\f': output << "\\f"; break;
            case '\n': output << "\\n"; break;
            case '\r': output << "\\r"; break;
            case '\t': output << "\\t"; break;
            default:
                if (ch < 0x20) {
                    output << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(ch);
                } else {
                    output << static_cast<char>(ch);
                }
                break;
        }
    }
    output << '"';
    return output.str();
}

bool isLowerKebabName(const std::string& value) {
    if (value.empty() || value.size() > 64) {
        return false;
    }
    if (!std::islower(static_cast<unsigned char>(value.front())) && !std::isdigit(static_cast<unsigned char>(value.front()))) {
        return false;
    }
    if (value.front() == '-' || value.back() == '-') {
        return false;
    }
    bool lastWasDash = false;
    for (unsigned char ch : value) {
        bool valid = std::islower(ch) || std::isdigit(ch) || ch == '-';
        if (!valid) {
            return false;
        }
        if (ch == '-' && lastWasDash) {
            return false;
        }
        lastWasDash = ch == '-';
    }
    return true;
}

int comparePrereleaseToken(const std::string& left, const std::string& right) {
    bool leftNumeric = isDigits(left);
    bool rightNumeric = isDigits(right);
    if (leftNumeric && rightNumeric) {
        long long leftValue = std::stoll(left);
        long long rightValue = std::stoll(right);
        if (leftValue < rightValue) return -1;
        if (leftValue > rightValue) return 1;
        return 0;
    }
    if (leftNumeric) return -1;
    if (rightNumeric) return 1;
    if (left < right) return -1;
    if (left > right) return 1;
    return 0;
}

bool parseCoreVersion(const std::string& value, SemVer& version) {
    std::vector<std::string> parts = splitString(value, '.');
    if (parts.size() != 3) {
        return false;
    }
    for (const auto& part : parts) {
        if (!isDigits(part) || hasLeadingZero(part)) {
            return false;
        }
    }
    version.majorVersion = std::stoi(parts[0]);
    version.minorVersion = std::stoi(parts[1]);
    version.patchVersion = std::stoi(parts[2]);
    return true;
}

bool validIdentifierList(const std::string& value, bool allowLeadingZeroNumber) {
    if (value.empty()) {
        return true;
    }
    for (const std::string& part : splitString(value, '.')) {
        if (part.empty()) {
            return false;
        }
        for (unsigned char ch : part) {
            if (!std::isalnum(ch) && ch != '-') {
                return false;
            }
        }
        if (!allowLeadingZeroNumber && isDigits(part) && hasLeadingZero(part)) {
            return false;
        }
    }
    return true;
}

bool safeArchivePath(const std::string& path) {
    if (path.empty() || path.front() == '/' || path.find('\\') != std::string::npos) {
        return false;
    }
    if (path.find("//") != std::string::npos) {
        return false;
    }
    for (const auto& part : splitString(path, '/')) {
        if (part.empty() || part == "." || part == "..") {
            return false;
        }
    }
    return true;
}

bool deniedArchivePath(const std::string& path) {
    return path == ".git" || startsWith(path, ".git/") ||
           path == "build" || startsWith(path, "build/") ||
           path == ".cloud/objects" || startsWith(path, ".cloud/objects/");
}

bool allowedArchivePath(std::string path) {
    while (!path.empty() && path.back() == '/') {
        path.pop_back();
    }
    if (path == "config.toml") {
        return true;
    }
    if (path == "src" || startsWith(path, "src/")) {
        return true;
    }
    if (path == "include" || startsWith(path, "include/")) {
        return true;
    }
    if (path == "docs" || startsWith(path, "docs/")) {
        return true;
    }
    if (path.find('/') == std::string::npos) {
        return startsWith(path, "README") || startsWith(path, "LICENSE");
    }
    return false;
}

std::uint64_t parseTarOctal(const unsigned char* bytes, std::size_t size) {
    std::uint64_t value = 0;
    std::size_t index = 0;
    while (index < size && (bytes[index] == ' ' || bytes[index] == '\0')) {
        ++index;
    }
    for (; index < size; ++index) {
        unsigned char ch = bytes[index];
        if (ch == '\0' || ch == ' ') {
            break;
        }
        if (ch < '0' || ch > '7') {
            return 0;
        }
        value = (value << 3) + static_cast<std::uint64_t>(ch - '0');
    }
    return value;
}

std::string readTarString(const unsigned char* bytes, std::size_t size) {
    std::size_t length = 0;
    while (length < size && bytes[length] != '\0') {
        ++length;
    }
    return std::string(reinterpret_cast<const char*>(bytes), length);
}

bool zeroBlock(const unsigned char* bytes) {
    for (std::size_t index = 0; index < 512; ++index) {
        if (bytes[index] != 0) {
            return false;
        }
    }
    return true;
}

std::optional<std::string> decompressGzip(const std::string& bytes, std::size_t maxOutputBytes, std::string& errorMessage) {
    z_stream stream{};
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(bytes.data()));
    stream.avail_in = static_cast<uInt>(bytes.size());

    if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK) {
        errorMessage = "archive is not valid gzip data";
        return std::nullopt;
    }

    std::string output;
    std::array<char, 32768> buffer{};
    int result = Z_OK;
    while (result == Z_OK) {
        stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
        stream.avail_out = static_cast<uInt>(buffer.size());
        result = inflate(&stream, Z_NO_FLUSH);
        std::size_t produced = buffer.size() - stream.avail_out;
        if (output.size() + produced > maxOutputBytes) {
            inflateEnd(&stream);
            errorMessage = "archive expands beyond the allowed size";
            return std::nullopt;
        }
        output.append(buffer.data(), produced);
    }

    inflateEnd(&stream);
    if (result != Z_STREAM_END) {
        errorMessage = "archive could not be decompressed";
        return std::nullopt;
    }
    return output;
}

std::string contentDispositionParameter(const std::string& header, const std::string& parameter) {
    std::string needle = parameter + "=\"";
    std::size_t start = header.find(needle);
    if (start == std::string::npos) {
        return "";
    }
    start += needle.size();
    std::size_t end = header.find('"', start);
    if (end == std::string::npos) {
        return "";
    }
    return header.substr(start, end - start);
}

std::string headerValue(const std::map<std::string, std::string>& headers, const std::string& key) {
    auto found = headers.find(lowerCopy(key));
    return found == headers.end() ? "" : found->second;
}

std::string bytesToHex(const unsigned char* bytes, std::size_t size) {
    static constexpr char alphabet[] = "0123456789abcdef";
    std::string output;
    output.resize(size * 2);
    for (std::size_t index = 0; index < size; ++index) {
        output[index * 2] = alphabet[(bytes[index] >> 4) & 0x0f];
        output[index * 2 + 1] = alphabet[bytes[index] & 0x0f];
    }
    return output;
}

std::string utcFormat(const char* format) {
    auto now = std::chrono::system_clock::now();
    std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&time, &tm);
    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), format, &tm);
    return buffer;
}

}

struct JsonValue::State {
    JsonData data;
};

JsonValue::JsonValue() : state(new State{nullptr}) {}
JsonValue::JsonValue(std::nullptr_t) : state(new State{nullptr}) {}
JsonValue::JsonValue(bool value) : state(new State{value}) {}
JsonValue::JsonValue(double value) : state(new State{value}) {}
JsonValue::JsonValue(int value) : state(new State{static_cast<double>(value)}) {}
JsonValue::JsonValue(std::int64_t value) : state(new State{static_cast<double>(value)}) {}
JsonValue::JsonValue(const char* value) : state(new State{std::string(value)}) {}
JsonValue::JsonValue(std::string value) : state(new State{std::move(value)}) {}
JsonValue::JsonValue(Array value) : state(new State{std::move(value)}) {}
JsonValue::JsonValue(Object value) : state(new State{std::move(value)}) {}
JsonValue::~JsonValue() { delete state; }

JsonValue::JsonValue(const JsonValue& other) : state(new State{other.state->data}) {}
JsonValue::JsonValue(JsonValue&& other) noexcept : state(other.state) {
    other.state = new State{nullptr};
}

JsonValue& JsonValue::operator=(const JsonValue& other) {
    if (this != &other) {
        state->data = other.state->data;
    }
    return *this;
}

JsonValue& JsonValue::operator=(JsonValue&& other) noexcept {
    if (this != &other) {
        delete state;
        state = other.state;
        other.state = new State{nullptr};
    }
    return *this;
}

JsonType JsonValue::type() const {
    if (std::holds_alternative<std::nullptr_t>(state->data)) return JsonType::Null;
    if (std::holds_alternative<bool>(state->data)) return JsonType::Bool;
    if (std::holds_alternative<double>(state->data)) return JsonType::Number;
    if (std::holds_alternative<std::string>(state->data)) return JsonType::String;
    if (std::holds_alternative<Array>(state->data)) return JsonType::Array;
    return JsonType::Object;
}

bool JsonValue::isNull() const { return type() == JsonType::Null; }
bool JsonValue::isBool() const { return type() == JsonType::Bool; }
bool JsonValue::isNumber() const { return type() == JsonType::Number; }
bool JsonValue::isString() const { return type() == JsonType::String; }
bool JsonValue::isArray() const { return type() == JsonType::Array; }
bool JsonValue::isObject() const { return type() == JsonType::Object; }

bool JsonValue::asBool(bool defaultValue) const {
    return isBool() ? std::get<bool>(state->data) : defaultValue;
}

double JsonValue::asNumber(double defaultValue) const {
    return isNumber() ? std::get<double>(state->data) : defaultValue;
}

std::int64_t JsonValue::asInt(std::int64_t defaultValue) const {
    return isNumber() ? static_cast<std::int64_t>(std::get<double>(state->data)) : defaultValue;
}

const std::string& JsonValue::asString() const {
    return isString() ? std::get<std::string>(state->data) : emptyString;
}

const JsonValue::Array& JsonValue::asArray() const {
    return isArray() ? std::get<Array>(state->data) : emptyArray;
}

const JsonValue::Object& JsonValue::asObject() const {
    return isObject() ? std::get<Object>(state->data) : emptyObject;
}

std::optional<std::string> JsonValue::getString(const std::string& key) const {
    auto found = asObject().find(key);
    if (found == asObject().end() || !found->second.isString()) {
        return std::nullopt;
    }
    return found->second.asString();
}

std::optional<JsonValue> JsonValue::getValue(const std::string& key) const {
    auto found = asObject().find(key);
    if (found == asObject().end()) {
        return std::nullopt;
    }
    return found->second;
}

std::string JsonValue::serialize() const {
    switch (type()) {
        case JsonType::Null:
            return "null";
        case JsonType::Bool:
            return asBool() ? "true" : "false";
        case JsonType::Number: {
            double value = asNumber();
            if (value == static_cast<std::int64_t>(value)) {
                return std::to_string(static_cast<std::int64_t>(value));
            }
            std::ostringstream output;
            output << std::setprecision(15) << value;
            return output.str();
        }
        case JsonType::String:
            return escapeJsonString(asString());
        case JsonType::Array: {
            std::string output = "[";
            bool first = true;
            for (const auto& item : asArray()) {
                if (!first) output += ",";
                output += item.serialize();
                first = false;
            }
            output += "]";
            return output;
        }
        case JsonType::Object: {
            std::string output = "{";
            bool first = true;
            for (const auto& [key, value] : asObject()) {
                if (!first) output += ",";
                output += escapeJsonString(key);
                output += ":";
                output += value.serialize();
                first = false;
            }
            output += "}";
            return output;
        }
    }
    return "null";
}

JsonValue JsonValue::parse(const std::string& source) {
    return JsonParser(source).parse();
}

std::string SemVer::text() const {
    std::string value = std::to_string(majorVersion) + "." + std::to_string(minorVersion) + "." + std::to_string(patchVersion);
    if (!prerelease.empty()) {
        value += "-" + prerelease;
    }
    if (!build.empty()) {
        value += "+" + build;
    }
    return value;
}

int SemVer::compare(const SemVer& other) const {
    if (majorVersion != other.majorVersion) return majorVersion < other.majorVersion ? -1 : 1;
    if (minorVersion != other.minorVersion) return minorVersion < other.minorVersion ? -1 : 1;
    if (patchVersion != other.patchVersion) return patchVersion < other.patchVersion ? -1 : 1;
    if (prerelease.empty() && other.prerelease.empty()) return 0;
    if (prerelease.empty()) return 1;
    if (other.prerelease.empty()) return -1;

    std::vector<std::string> leftParts = splitString(prerelease, '.');
    std::vector<std::string> rightParts = splitString(other.prerelease, '.');
    std::size_t limit = std::min(leftParts.size(), rightParts.size());
    for (std::size_t index = 0; index < limit; ++index) {
        int result = comparePrereleaseToken(leftParts[index], rightParts[index]);
        if (result != 0) {
            return result;
        }
    }
    if (leftParts.size() == rightParts.size()) return 0;
    return leftParts.size() < rightParts.size() ? -1 : 1;
}

bool SemVer::matchesRange(const std::string& rawRange) const {
    std::string range = trimCopy(rawRange);
    if (range.empty() || range == "*") {
        return true;
    }

    auto compareToBase = [&](const std::string& text) -> std::optional<int> {
        auto base = SemVer::parse(text);
        if (!base) {
            return std::nullopt;
        }
        return compare(*base);
    };

    if (startsWith(range, ">=")) {
        auto result = compareToBase(range.substr(2));
        return result && *result >= 0;
    }
    if (startsWith(range, "<=")) {
        auto result = compareToBase(range.substr(2));
        return result && *result <= 0;
    }
    if (startsWith(range, ">")) {
        auto result = compareToBase(range.substr(1));
        return result && *result > 0;
    }
    if (startsWith(range, "<")) {
        auto result = compareToBase(range.substr(1));
        return result && *result < 0;
    }
    if (startsWith(range, "^")) {
        auto base = SemVer::parse(trimCopy(range.substr(1)));
        if (!base || compare(*base) < 0) {
            return false;
        }
        if (base->majorVersion > 0) {
            return majorVersion == base->majorVersion;
        }
        if (base->minorVersion > 0) {
            return majorVersion == 0 && minorVersion == base->minorVersion;
        }
        return majorVersion == 0 && minorVersion == 0 && patchVersion == base->patchVersion;
    }
    if (startsWith(range, "~")) {
        auto base = SemVer::parse(trimCopy(range.substr(1)));
        return base && compare(*base) >= 0 &&
               majorVersion == base->majorVersion &&
               minorVersion == base->minorVersion;
    }

    auto exact = SemVer::parse(range);
    return exact && compare(*exact) == 0;
}

std::optional<SemVer> SemVer::parse(const std::string& rawValue) {
    std::string value = trimCopy(rawValue);
    if (value.empty()) {
        return std::nullopt;
    }

    SemVer version;
    std::size_t buildStart = value.find('+');
    if (buildStart != std::string::npos) {
        version.build = value.substr(buildStart + 1);
        value = value.substr(0, buildStart);
        if (!validIdentifierList(version.build, true)) {
            return std::nullopt;
        }
    }

    std::size_t prereleaseStart = value.find('-');
    if (prereleaseStart != std::string::npos) {
        version.prerelease = value.substr(prereleaseStart + 1);
        value = value.substr(0, prereleaseStart);
        if (!validIdentifierList(version.prerelease, false)) {
            return std::nullopt;
        }
    }

    if (!parseCoreVersion(value, version)) {
        return std::nullopt;
    }
    return version;
}

bool operator<(const SemVer& left, const SemVer& right) {
    return left.compare(right) < 0;
}

bool operator==(const SemVer& left, const SemVer& right) {
    return left.compare(right) == 0;
}

bool isValidOwnerName(const std::string& value) {
    return isLowerKebabName(value);
}

bool isValidPackageName(const std::string& value) {
    return isLowerKebabName(value);
}

bool isValidScopedPackageName(const std::string& value) {
    if (!startsWith(value, "@")) {
        return false;
    }
    std::size_t slash = value.find('/');
    if (slash == std::string::npos || value.find('/', slash + 1) != std::string::npos) {
        return false;
    }
    return isValidOwnerName(value.substr(1, slash - 1)) && isValidPackageName(value.substr(slash + 1));
}

std::optional<PackageManifest> parsePackageManifest(
    const std::string& manifestJson,
    const std::string& expectedOwner,
    const std::string& expectedPackage,
    const std::string& expectedVersion,
    std::string& errorMessage
) {
    JsonValue manifest = JsonValue::parse(manifestJson);
    if (!manifest.isObject()) {
        errorMessage = "manifest must be a JSON object";
        return std::nullopt;
    }

    auto name = manifest.getString("name");
    auto version = manifest.getString("version");
    auto mainFile = manifest.getString("main");
    auto dependencies = manifest.getValue("dependencies");
    if (!name || !version || !mainFile || !dependencies) {
        errorMessage = "manifest requires name, version, main, and dependencies";
        return std::nullopt;
    }
    if (!isValidScopedPackageName(*name)) {
        errorMessage = "manifest name must be scoped like @owner/package";
        return std::nullopt;
    }
    std::string expectedName = "@" + expectedOwner + "/" + expectedPackage;
    if (*name != expectedName) {
        errorMessage = "manifest name does not match the package route";
        return std::nullopt;
    }
    if (*version != expectedVersion || !SemVer::parse(*version)) {
        errorMessage = "manifest version must be immutable SemVer and match the route";
        return std::nullopt;
    }
    if (mainFile->empty() || mainFile->front() == '/' || mainFile->find("..") != std::string::npos) {
        errorMessage = "manifest main must be a relative project file";
        return std::nullopt;
    }
    if (!dependencies->isObject()) {
        errorMessage = "manifest dependencies must be an object";
        return std::nullopt;
    }

    return PackageManifest{
        *name,
        expectedOwner,
        expectedPackage,
        *version,
        *mainFile,
        *dependencies,
        manifest
    };
}

ArchiveValidation validateSourceArchive(const std::string& archiveBytes, std::size_t maxPackageBytes) {
    ArchiveValidation validation;
    if (archiveBytes.empty()) {
        validation.errorMessage = "archive is empty";
        return validation;
    }
    if (archiveBytes.size() > maxPackageBytes) {
        validation.errorMessage = "archive exceeds the configured package size";
        return validation;
    }

    std::string errorMessage;
    std::size_t maxExpandedBytes = std::max<std::size_t>(maxPackageBytes * 20, maxPackageBytes + 1024 * 1024);
    auto tarBytes = decompressGzip(archiveBytes, maxExpandedBytes, errorMessage);
    if (!tarBytes) {
        validation.errorMessage = errorMessage;
        return validation;
    }

    std::size_t offset = 0;
    bool foundConfig = false;
    while (offset + 512 <= tarBytes->size()) {
        const auto* header = reinterpret_cast<const unsigned char*>(tarBytes->data() + offset);
        if (zeroBlock(header)) {
            if (!foundConfig) {
                validation.errorMessage = "archive must include config.toml";
                return validation;
            }
            validation.ok = true;
            return validation;
        }

        std::string name = readTarString(header, 100);
        std::string prefix = readTarString(header + 345, 155);
        std::string path = prefix.empty() ? name : prefix + "/" + name;
        std::uint64_t size = parseTarOctal(header + 124, 12);
        char type = static_cast<char>(header[156]);

        if (!safeArchivePath(path) || deniedArchivePath(path) || !allowedArchivePath(path)) {
            validation.errorMessage = "archive contains disallowed path: " + path;
            return validation;
        }
        if (type != '\0' && type != '0' && type != '5') {
            validation.errorMessage = "archive contains unsupported entry type for path: " + path;
            return validation;
        }
        if (type == '5') {
            size = 0;
        }
        if (size > maxPackageBytes || validation.unpackedBytes + size > maxExpandedBytes) {
            validation.errorMessage = "archive contains an oversized file: " + path;
            return validation;
        }

        if (path == "config.toml") {
            foundConfig = true;
        }
        validation.entries.push_back(path);
        validation.unpackedBytes += size;

        std::size_t paddedSize = static_cast<std::size_t>((size + 511) / 512 * 512);
        if (offset + 512 + paddedSize > tarBytes->size()) {
            validation.errorMessage = "archive tar data is truncated";
            return validation;
        }
        offset += 512 + paddedSize;
    }

    if (!foundConfig) {
        validation.errorMessage = "archive must include config.toml";
        return validation;
    }
    validation.ok = true;
    return validation;
}

std::optional<std::map<std::string, MultipartPart>> parseMultipartForm(
    const std::string& contentType,
    const std::string& body,
    std::string& errorMessage
) {
    std::string lowerType = lowerCopy(contentType);
    std::string marker = "boundary=";
    std::size_t boundaryStart = lowerType.find(marker);
    if (!startsWith(lowerType, "multipart/form-data") || boundaryStart == std::string::npos) {
        errorMessage = "content type must be multipart/form-data";
        return std::nullopt;
    }

    boundaryStart += marker.size();
    std::string boundary = contentType.substr(boundaryStart);
    boundary = trimCopy(boundary);
    if (!boundary.empty() && boundary.front() == '"' && boundary.back() == '"') {
        boundary = boundary.substr(1, boundary.size() - 2);
    }
    if (boundary.empty()) {
        errorMessage = "multipart boundary is missing";
        return std::nullopt;
    }

    std::string delimiter = "--" + boundary;
    std::size_t position = body.find(delimiter);
    if (position == std::string::npos) {
        errorMessage = "multipart body does not contain the boundary";
        return std::nullopt;
    }

    std::map<std::string, MultipartPart> parts;
    while (position != std::string::npos) {
        position += delimiter.size();
        if (position + 2 <= body.size() && body.substr(position, 2) == "--") {
            break;
        }
        if (position + 2 <= body.size() && body.substr(position, 2) == "\r\n") {
            position += 2;
        }

        std::size_t headerEnd = body.find("\r\n\r\n", position);
        if (headerEnd == std::string::npos) {
            errorMessage = "multipart part is missing headers";
            return std::nullopt;
        }

        std::map<std::string, std::string> headers;
        std::string headersText = body.substr(position, headerEnd - position);
        for (const auto& line : splitString(headersText, '\n')) {
            std::string cleaned = trimCopy(line);
            if (!cleaned.empty() && cleaned.back() == '\r') {
                cleaned.pop_back();
            }
            std::size_t colon = cleaned.find(':');
            if (colon == std::string::npos) {
                continue;
            }
            headers[lowerCopy(trimCopy(cleaned.substr(0, colon)))] = trimCopy(cleaned.substr(colon + 1));
        }

        std::size_t dataStart = headerEnd + 4;
        std::string nextNeedle = "\r\n" + delimiter;
        std::size_t next = body.find(nextNeedle, dataStart);
        if (next == std::string::npos) {
            errorMessage = "multipart part is missing a closing boundary";
            return std::nullopt;
        }

        std::string disposition = headerValue(headers, "content-disposition");
        std::string name = contentDispositionParameter(disposition, "name");
        if (!name.empty()) {
            MultipartPart part;
            part.name = name;
            part.fileName = contentDispositionParameter(disposition, "filename");
            part.headers = std::move(headers);
            part.data = body.substr(dataStart, next - dataStart);
            parts[name] = std::move(part);
        }

        position = next + 2;
    }

    return parts;
}

std::string sha256Hex(const std::string& value) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), digest);
    return bytesToHex(digest, SHA256_DIGEST_LENGTH);
}

std::string hmacSha256Bytes(const std::string& key, const std::string& value) {
    unsigned int length = SHA256_DIGEST_LENGTH;
    unsigned char digest[SHA256_DIGEST_LENGTH];
    HMAC(EVP_sha256(),
         key.data(),
         static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(value.data()),
         value.size(),
         digest,
         &length);
    return std::string(reinterpret_cast<const char*>(digest), length);
}

std::string randomToken(std::size_t byteCount) {
    std::vector<unsigned char> bytes(byteCount);
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
        throw std::runtime_error("failed to generate secure random bytes");
    }
    return "cloud_" + bytesToHex(bytes.data(), bytes.size());
}

bool constantTimeEquals(const std::string& left, const std::string& right) {
    if (left.size() != right.size()) {
        return false;
    }
    unsigned char diff = 0;
    for (std::size_t index = 0; index < left.size(); ++index) {
        diff |= static_cast<unsigned char>(left[index]) ^ static_cast<unsigned char>(right[index]);
    }
    return diff == 0;
}

std::string hexEncode(const unsigned char* bytes, std::size_t size) {
    return bytesToHex(bytes, size);
}

std::string urlEncode(const std::string& value, bool keepSlash) {
    static constexpr char alphabet[] = "0123456789ABCDEF";
    std::string output;
    for (unsigned char ch : value) {
        bool safe = std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~' || (keepSlash && ch == '/');
        if (safe) {
            output.push_back(static_cast<char>(ch));
        } else {
            output.push_back('%');
            output.push_back(alphabet[(ch >> 4) & 0x0f]);
            output.push_back(alphabet[ch & 0x0f]);
        }
    }
    return output;
}

std::string urlDecode(const std::string& value) {
    std::string output;
    for (std::size_t index = 0; index < value.size(); ++index) {
        char ch = value[index];
        if (ch == '%' && index + 2 < value.size()) {
            unsigned int decoded = 0;
            std::stringstream stream;
            stream << std::hex << value.substr(index + 1, 2);
            if (stream >> decoded) {
                output.push_back(static_cast<char>(decoded));
                index += 2;
                continue;
            }
        }
        output.push_back(ch == '+' ? ' ' : ch);
    }
    return output;
}

std::string utcDateStamp() {
    return utcFormat("%Y%m%d");
}

std::string utcAmzDate() {
    return utcFormat("%Y%m%dT%H%M%SZ");
}

std::string trimCopy(const std::string& value) {
    std::size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) {
        ++start;
    }
    std::size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return value.substr(start, end - start);
}

std::string lowerCopy(const std::string& value) {
    std::string output = value;
    std::transform(output.begin(), output.end(), output.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return output;
}

}
