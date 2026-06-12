#include <cloudServer/core.hpp>
#include <cloudServer/service.hpp>

#include <zlib.h>

#include <cassert>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

std::string octalValue(std::uint64_t value, std::size_t width) {
    std::string output(width, '0');
    std::string digits;
    do {
        digits.insert(digits.begin(), static_cast<char>('0' + (value & 7)));
        value >>= 3;
    } while (value > 0);
    std::size_t start = width > digits.size() + 1 ? width - digits.size() - 1 : 0;
    std::copy(digits.begin(), digits.end(), output.begin() + start);
    output[width - 1] = '\0';
    return output;
}

std::string tarHeader(const std::string& name, std::size_t size, char type) {
    std::string header(512, '\0');
    std::memcpy(header.data(), name.data(), std::min<std::size_t>(name.size(), 100));
    std::string mode = octalValue(0644, 8);
    std::string owner = octalValue(0, 8);
    std::string sizeText = octalValue(size, 12);
    std::string timeText = octalValue(0, 12);
    std::memcpy(header.data() + 100, mode.data(), 8);
    std::memcpy(header.data() + 108, owner.data(), 8);
    std::memcpy(header.data() + 116, owner.data(), 8);
    std::memcpy(header.data() + 124, sizeText.data(), 12);
    std::memcpy(header.data() + 136, timeText.data(), 12);
    std::memset(header.data() + 148, ' ', 8);
    header[156] = type;
    std::memcpy(header.data() + 257, "ustar", 5);
    unsigned int checksum = 0;
    for (unsigned char ch : header) {
        checksum += ch;
    }
    std::string checksumText = octalValue(checksum, 8);
    std::memcpy(header.data() + 148, checksumText.data(), 8);
    return header;
}

std::string gzipBytes(const std::string& input) {
    z_stream stream{};
    if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return "";
    }

    std::string output;
    std::vector<char> buffer(16384);
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
    stream.avail_in = static_cast<uInt>(input.size());
    int result = Z_OK;
    while (result == Z_OK) {
        stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
        stream.avail_out = static_cast<uInt>(buffer.size());
        result = deflate(&stream, Z_FINISH);
        output.append(buffer.data(), buffer.size() - stream.avail_out);
    }
    deflateEnd(&stream);
    return output;
}

std::string makeArchive(const std::map<std::string, std::string>& files) {
    std::string tar;
    for (const auto& [path, body] : files) {
        tar += tarHeader(path, body.size(), '0');
        tar += body;
        std::size_t padding = (512 - (body.size() % 512)) % 512;
        tar.append(padding, '\0');
    }
    tar.append(1024, '\0');
    return gzipBytes(tar);
}

void testSemVer() {
    auto parsed = CloudServer::SemVer::parse("1.2.3-alpha.1+build.5");
    assert(parsed);
    assert(parsed->majorVersion == 1);
    assert(parsed->minorVersion == 2);
    assert(parsed->patchVersion == 3);
    assert(parsed->prerelease == "alpha.1");
    assert(CloudServer::SemVer::parse("1.3.0")->matchesRange("^1.2.0"));
    assert(!CloudServer::SemVer::parse("2.0.0")->matchesRange("^1.2.0"));
    assert(CloudServer::SemVer::parse("1.2.9")->matchesRange("~1.2.0"));
    assert(!CloudServer::SemVer::parse("1.3.0")->matchesRange("~1.2.0"));
}

void testPackageNames() {
    assert(CloudServer::isValidScopedPackageName("@landing-pad/http-core"));
    assert(!CloudServer::isValidScopedPackageName("landing-pad/http-core"));
    assert(!CloudServer::isValidScopedPackageName("@Landing/http-core"));
    assert(!CloudServer::isValidScopedPackageName("@landing/http_core"));
}

void testManifest() {
    std::string error;
    auto manifest = CloudServer::parsePackageManifest(
        R"({"name":"@landing/http-core","version":"1.0.0","main":"src/main.ins","dependencies":{}})",
        "landing",
        "http-core",
        "1.0.0",
        error
    );
    assert(manifest);
    assert(manifest->name == "@landing/http-core");

    auto bad = CloudServer::parsePackageManifest(
        R"({"name":"@other/http-core","version":"1.0.0","main":"src/main.ins","dependencies":{}})",
        "landing",
        "http-core",
        "1.0.0",
        error
    );
    assert(!bad);
}

void testArchiveValidation() {
    std::string archive = makeArchive({
        {"config.toml", "[project]\nname = \"demo\"\n"},
        {"src/main.ins", "module main\n"}
    });
    auto validation = CloudServer::validateSourceArchive(archive, 1024 * 1024);
    assert(validation.ok);
    assert(validation.entries.size() == 2);

    std::string denied = makeArchive({
        {"config.toml", ""},
        {".git/config", ""}
    });
    auto deniedValidation = CloudServer::validateSourceArchive(denied, 1024 * 1024);
    assert(!deniedValidation.ok);
}

void testMultipart() {
    std::string body =
        "--abc\r\n"
        "Content-Disposition: form-data; name=\"manifest\"\r\n\r\n"
        "{}\r\n"
        "--abc\r\n"
        "Content-Disposition: form-data; name=\"archive\"; filename=\"package.tar.gz\"\r\n\r\n"
        "bytes\r\n"
        "--abc--\r\n";
    std::string error;
    auto parts = CloudServer::parseMultipartForm("multipart/form-data; boundary=abc", body, error);
    assert(parts);
    assert(parts->at("manifest").data == "{}");
    assert(parts->at("archive").fileName == "package.tar.gz");
}

void testCrypto() {
    assert(CloudServer::sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    std::string token = CloudServer::randomToken(8);
    assert(token.rfind("cloud_", 0) == 0);
    assert(CloudServer::constantTimeEquals("same", "same"));
    assert(!CloudServer::constantTimeEquals("same", "nope"));
}

void testRateLimiter() {
    // 3 requests per window allowed; the 4th is rejected. Disabled limiter
    // (limit <= 0) always allows.
    CloudServer::RateLimiter limiter(3, 60);
    assert(limiter.allow("client-a"));
    assert(limiter.allow("client-a"));
    assert(limiter.allow("client-a"));
    assert(!limiter.allow("client-a"));
    // A different client has its own independent bucket.
    assert(limiter.allow("client-b"));

    CloudServer::RateLimiter disabled(0, 60);
    for (int i = 0; i < 100; ++i) {
        assert(disabled.allow("anyone"));
    }
}

}

int main() {
    testSemVer();
    testPackageNames();
    testManifest();
    testArchiveValidation();
    testMultipart();
    testCrypto();
    testRateLimiter();
    std::cout << "cloud-server tests passed\n";
    return 0;
}
