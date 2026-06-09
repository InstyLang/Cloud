#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace CloudServer {

enum class JsonType {
    Null,
    Bool,
    Number,
    String,
    Array,
    Object
};

class JsonValue {
public:
    using Array = std::vector<JsonValue>;
    using Object = std::map<std::string, JsonValue>;

    JsonValue();
    JsonValue(std::nullptr_t);
    JsonValue(bool value);
    JsonValue(double value);
    JsonValue(int value);
    JsonValue(std::int64_t value);
    JsonValue(const char* value);
    JsonValue(std::string value);
    JsonValue(Array value);
    JsonValue(Object value);
    ~JsonValue();

    JsonValue(const JsonValue& other);
    JsonValue(JsonValue&& other) noexcept;
    JsonValue& operator=(const JsonValue& other);
    JsonValue& operator=(JsonValue&& other) noexcept;

    JsonType type() const;
    bool isNull() const;
    bool isBool() const;
    bool isNumber() const;
    bool isString() const;
    bool isArray() const;
    bool isObject() const;

    bool asBool(bool defaultValue = false) const;
    double asNumber(double defaultValue = 0) const;
    std::int64_t asInt(std::int64_t defaultValue = 0) const;
    const std::string& asString() const;
    const Array& asArray() const;
    const Object& asObject() const;

    std::optional<std::string> getString(const std::string& key) const;
    std::optional<JsonValue> getValue(const std::string& key) const;

    std::string serialize() const;
    static JsonValue parse(const std::string& source);

private:
    struct State;
    State* state;
};

struct SemVer {
    int majorVersion = 0;
    int minorVersion = 0;
    int patchVersion = 0;
    std::string prerelease;
    std::string build;

    std::string text() const;
    int compare(const SemVer& other) const;
    bool matchesRange(const std::string& range) const;

    static std::optional<SemVer> parse(const std::string& value);
};

bool operator<(const SemVer& left, const SemVer& right);
bool operator==(const SemVer& left, const SemVer& right);

struct PackageManifest {
    std::string name;
    std::string owner;
    std::string packageName;
    std::string version;
    std::string mainFile;
    JsonValue dependencies;
    JsonValue raw;
};

bool isValidOwnerName(const std::string& value);
bool isValidPackageName(const std::string& value);
bool isValidScopedPackageName(const std::string& value);
std::optional<PackageManifest> parsePackageManifest(
    const std::string& manifestJson,
    const std::string& expectedOwner,
    const std::string& expectedPackage,
    const std::string& expectedVersion,
    std::string& errorMessage
);

struct ArchiveValidation {
    bool ok = false;
    std::string errorMessage;
    std::uint64_t unpackedBytes = 0;
    std::vector<std::string> entries;
};

ArchiveValidation validateSourceArchive(const std::string& archiveBytes, std::size_t maxPackageBytes);

struct MultipartPart {
    std::string name;
    std::string fileName;
    std::map<std::string, std::string> headers;
    std::string data;
};

std::optional<std::map<std::string, MultipartPart>> parseMultipartForm(
    const std::string& contentType,
    const std::string& body,
    std::string& errorMessage
);

std::string sha256Hex(const std::string& value);
std::string hmacSha256Bytes(const std::string& key, const std::string& value);
std::string randomToken(std::size_t byteCount = 32);
bool constantTimeEquals(const std::string& left, const std::string& right);

std::string hexEncode(const unsigned char* bytes, std::size_t size);
std::string urlEncode(const std::string& value, bool keepSlash = false);
std::string urlDecode(const std::string& value);
std::string utcDateStamp();
std::string utcAmzDate();
std::string trimCopy(const std::string& value);
std::string lowerCopy(const std::string& value);

}
