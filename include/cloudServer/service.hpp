#pragma once

#include <cloudServer/core.hpp>

#include <cstddef>
#include <cstdint>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace CloudServer {

struct ServerConfig {
    int port = 8080;
    std::string databaseUrl;
    std::string s3Endpoint;
    std::string s3Bucket;
    std::string s3Region = "us-east-1";
    std::string s3AccessKeyId;
    std::string s3SecretAccessKey;
    std::string bootstrapToken;
    std::size_t maxPackageBytes = 50 * 1024 * 1024;

    // Per-client (token or IP) write rate limiting for mutating endpoints
    // (publish, yank, token create/revoke). 0 disables limiting.
    int writeRateLimit = 30;          // requests allowed per window
    int writeRateWindowSeconds = 60;  // sliding window length

    static ServerConfig fromEnvironment();
    std::optional<std::string> validateForServe() const;
};

struct HttpRequest {
    std::string method;
    std::string path;
    std::map<std::string, std::string> query;
    std::map<std::string, std::string> headers;
    std::string body;
    
    std::string header(const std::string& key) const;
};

struct HttpResponse {
    int status = 200;
    std::string body;
    std::map<std::string, std::string> headers;

    static HttpResponse json(int status, const JsonValue& value);
    static HttpResponse error(int status, const std::string& message);
    static HttpResponse redirect(const std::string& location);
};

struct AuthIdentity {
    long accountId = 0;
    std::string accountName;
    std::string scope;
};

struct CreatedToken {
    long accountId = 0;
    std::string accountName;
    std::string scope;
    std::string token;
    std::string tokenPrefix;
};

struct PackageVersionInfo {
    std::string name;
    std::string owner;
    std::string packageName;
    std::string version;
    std::string manifestJson;
    std::string checksumSha256;
    std::uint64_t sizeBytes = 0;
    std::string objectKey;
    bool yanked = false;
    std::string publishedAt;
};

enum class PublishStatus {
    Created,
    Duplicate,
    Unauthorized,
    Error
};

struct PublishResult {
    PublishStatus status = PublishStatus::Error;
    std::string errorMessage;
    PackageVersionInfo version;
};

enum class YankStatus {
    Updated,
    Missing,
    Unauthorized,
    Error
};

struct YankResult {
    YankStatus status = YankStatus::Error;
    std::string errorMessage;
    PackageVersionInfo version;
};

class RegistryDatabase {
public:
    explicit RegistryDatabase(const ServerConfig& config, int poolSize = 4);
    ~RegistryDatabase();

    bool ping();
    void runMigrations();
    std::optional<AuthIdentity> authenticate(const std::string& token);
    CreatedToken createToken(const std::string& accountName, const std::string& scope,
                             std::optional<long> expiresInSeconds = std::nullopt);
    // Revokes the token whose hash matches `token` (the caller's own token).
    // Returns true if a live token was revoked.
    bool revokeToken(const std::string& token);
    // Admin revoke by token prefix; revokes all live tokens with that prefix.
    // Returns the number of tokens revoked.
    int revokeTokensByPrefix(const std::string& tokenPrefix);
    std::optional<PackageVersionInfo> getVersion(
        const std::string& owner,
        const std::string& packageName,
        const std::string& version
    );
    std::vector<PackageVersionInfo> listVersions(
        const std::string& owner,
        const std::string& packageName,
        bool includeYanked
    );
    PublishStatus authorizePublish(
        const AuthIdentity& identity,
        const std::string& owner,
        const std::string& packageName,
        const std::string& version,
        std::string& errorMessage
    );
    PublishResult publishVersion(
        const AuthIdentity& identity,
        const PackageManifest& manifest,
        const std::string& manifestJson,
        const std::string& checksumSha256,
        std::uint64_t sizeBytes,
        const std::string& objectKey
    );
    YankResult yankVersion(
        const AuthIdentity& identity,
        const std::string& owner,
        const std::string& packageName,
        const std::string& version
    );

private:
    struct State;
    std::unique_ptr<State> state;
};

class S3Storage {
public:
    explicit S3Storage(ServerConfig config);
    ~S3Storage();

    bool putObject(const std::string& objectKey, const std::string& body, std::string& errorMessage);
    std::string presignedGetUrl(const std::string& objectKey, int expiresSeconds = 300) const;

private:
    struct State;
    std::unique_ptr<State> state;
};

// Thread-safe sliding-window rate limiter keyed by an arbitrary client id
// (auth token hash or remote address). Used to throttle mutating endpoints.
class RateLimiter {
public:
    RateLimiter(int maxRequests, int windowSeconds);

    // Records a hit for `clientId`; returns true if the request is allowed,
    // false if the client exceeded the configured rate. A non-positive limit
    // disables throttling (always allowed).
    bool allow(const std::string& clientId);

private:
    struct Bucket {
        std::chrono::steady_clock::time_point windowStart;
        int count = 0;
    };
    int maxRequests_;
    std::chrono::seconds window_;
    std::mutex mutex_;
    std::unordered_map<std::string, Bucket> buckets_;
};

class RegistryService {
public:
    RegistryService(ServerConfig config, RegistryDatabase& database, S3Storage& storage);
    HttpResponse handle(const HttpRequest& request);

private:
    ServerConfig config;
    RegistryDatabase& database;
    S3Storage& storage;
    RateLimiter writeLimiter;

    HttpResponse handleHealth();
    HttpResponse handleTokenCreate(const HttpRequest& request);
    HttpResponse handleTokenRevoke(const HttpRequest& request);
    HttpResponse handlePackageRoute(const HttpRequest& request, const std::vector<std::string>& segments);
    HttpResponse handlePublish(
        const HttpRequest& request,
        const std::string& owner,
        const std::string& packageName,
        const std::string& version
    );
    HttpResponse handleYank(
        const HttpRequest& request,
        const std::string& owner,
        const std::string& packageName,
        const std::string& version
    );
    std::optional<AuthIdentity> authenticateBearer(const HttpRequest& request, HttpResponse& failure);
    // Returns a failure response (and false) when the client exceeded the write
    // rate limit; the client id prefers the bearer token, falling back to the
    // X-Forwarded-For / remote address header.
    bool enforceWriteRate(const HttpRequest& request, HttpResponse& failure);
};

class HttpServer {
public:
    HttpServer(ServerConfig config, RegistryService& service);
    ~HttpServer();

    void start();

private:
    struct State;
    std::unique_ptr<State> state;
};

std::vector<std::string> splitPath(const std::string& path);
std::map<std::string, std::string> parseQueryString(const std::string& query);
JsonValue versionToJson(const PackageVersionInfo& version, const std::optional<std::string>& sourceUrl = std::nullopt);

}
