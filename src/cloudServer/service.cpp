#include <cloudServer/service.hpp>

#include <curl/curl.h>
#include <event2/buffer.h>
#include <event2/event.h>
#include <event2/http.h>
#include <event2/keyvalq_struct.h>
#include <libpq-fe.h>
#include <openssl/sha.h>

#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace CloudServer {

namespace {

const char* migrationSql = R"SQL(
CREATE TABLE IF NOT EXISTS accounts (
    id BIGSERIAL PRIMARY KEY,
    name TEXT NOT NULL UNIQUE,
    "createdAt" TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS "authTokens" (
    id BIGSERIAL PRIMARY KEY,
    "accountId" BIGINT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    "tokenHash" TEXT NOT NULL UNIQUE,
    "tokenPrefix" TEXT NOT NULL,
    scope TEXT NOT NULL,
    "expiresAt" TIMESTAMPTZ,
    "revokedAt" TIMESTAMPTZ,
    "createdAt" TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS packages (
    id BIGSERIAL PRIMARY KEY,
    "ownerName" TEXT NOT NULL,
    "packageName" TEXT NOT NULL,
    "createdAt" TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE ("ownerName", "packageName")
);

CREATE TABLE IF NOT EXISTS "packageOwners" (
    "packageId" BIGINT NOT NULL REFERENCES packages(id) ON DELETE CASCADE,
    "accountId" BIGINT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    "canPublish" BOOLEAN NOT NULL DEFAULT true,
    "createdAt" TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY ("packageId", "accountId")
);

CREATE TABLE IF NOT EXISTS "packageVersions" (
    id BIGSERIAL PRIMARY KEY,
    "packageId" BIGINT NOT NULL REFERENCES packages(id) ON DELETE CASCADE,
    version TEXT NOT NULL,
    major INTEGER NOT NULL,
    minor INTEGER NOT NULL,
    patch INTEGER NOT NULL,
    prerelease TEXT NOT NULL DEFAULT '',
    manifest JSONB NOT NULL,
    "checksumSha256" TEXT NOT NULL,
    "sizeBytes" BIGINT NOT NULL,
    "objectKey" TEXT NOT NULL,
    yanked BOOLEAN NOT NULL DEFAULT false,
    "publishedBy" BIGINT NOT NULL REFERENCES accounts(id),
    "publishedAt" TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE ("packageId", version)
);

CREATE INDEX IF NOT EXISTS "packageVersionsLookup"
    ON "packageVersions" ("packageId", major, minor, patch, prerelease);
)SQL";

std::string envValue(const char* key, const std::string& defaultValue = "") {
    const char* value = std::getenv(key);
    if (!value) {
        return defaultValue;
    }
    return value;
}

std::size_t parseSize(const std::string& value, std::size_t defaultValue) {
    if (value.empty()) {
        return defaultValue;
    }
    try {
        return static_cast<std::size_t>(std::stoull(value));
    } catch (...) {
        return defaultValue;
    }
}

std::string bearerToken(const std::string& authorization) {
    std::string trimmed = trimCopy(authorization);
    if (trimmed.size() < 8) {
        return "";
    }
    std::string prefix = lowerCopy(trimmed.substr(0, 7));
    if (prefix != "bearer ") {
        return "";
    }
    return trimCopy(trimmed.substr(7));
}

bool scopeAllowsPublish(const AuthIdentity& identity) {
    return identity.scope == "publish" || identity.scope == "admin";
}

bool scopeAllowsAdmin(const AuthIdentity& identity) {
    return identity.scope == "admin";
}

class PgError : public std::runtime_error {
public:
    PgError(std::string message, std::string stateCode)
        : std::runtime_error(std::move(message)), stateCode(std::move(stateCode)) {}

    std::string stateCode;
};

class PgResult {
public:
    explicit PgResult(PGresult* result) : result(result) {}
    ~PgResult() {
        if (result) {
            PQclear(result);
        }
    }

    PgResult(const PgResult&) = delete;
    PgResult& operator=(const PgResult&) = delete;

    PgResult(PgResult&& other) noexcept : result(other.result) {
        other.result = nullptr;
    }

    PgResult& operator=(PgResult&& other) noexcept {
        if (this != &other) {
            if (result) {
                PQclear(result);
            }
            result = other.result;
            other.result = nullptr;
        }
        return *this;
    }

    int rows() const {
        return PQntuples(result);
    }

    std::string value(int row, const char* column) const {
        int index = PQfnumber(result, column);
        if (index < 0 || PQgetisnull(result, row, index)) {
            return "";
        }
        return PQgetvalue(result, row, index);
    }

    bool boolValue(int row, const char* column) const {
        std::string text = lowerCopy(value(row, column));
        return text == "t" || text == "true" || text == "1";
    }

    std::uint64_t uintValue(int row, const char* column) const {
        std::string text = value(row, column);
        if (text.empty()) {
            return 0;
        }
        return static_cast<std::uint64_t>(std::stoull(text));
    }

private:
    PGresult* result = nullptr;
};

void ensureResultOk(PGconn* connection, PGresult* result, const std::string& context) {
    ExecStatusType status = PQresultStatus(result);
    if (status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK) {
        return;
    }
    const char* state = PQresultErrorField(result, PG_DIAG_SQLSTATE);
    std::string message = context + ": " + PQerrorMessage(connection);
    std::string stateCode = state ? state : "";
    PQclear(result);
    throw PgError(message, stateCode);
}

PgResult execSql(PGconn* connection, const std::string& sql) {
    PGresult* result = PQexec(connection, sql.c_str());
    ensureResultOk(connection, result, "postgres query failed");
    return PgResult(result);
}

PgResult execParams(PGconn* connection, const std::string& sql, const std::vector<std::string>& params) {
    std::vector<const char*> values;
    values.reserve(params.size());
    for (const auto& param : params) {
        values.push_back(param.c_str());
    }
    PGresult* result = PQexecParams(
        connection,
        sql.c_str(),
        static_cast<int>(params.size()),
        nullptr,
        values.data(),
        nullptr,
        nullptr,
        0
    );
    ensureResultOk(connection, result, "postgres query failed");
    return PgResult(result);
}

PackageVersionInfo packageVersionFromRow(const PgResult& result, int row) {
    PackageVersionInfo version;
    version.owner = result.value(row, "ownerName");
    version.packageName = result.value(row, "packageName");
    version.name = "@" + version.owner + "/" + version.packageName;
    version.version = result.value(row, "version");
    version.manifestJson = result.value(row, "manifest");
    version.checksumSha256 = result.value(row, "checksumSha256");
    version.sizeBytes = result.uintValue(row, "sizeBytes");
    version.objectKey = result.value(row, "objectKey");
    version.yanked = result.boolValue(row, "yanked");
    version.publishedAt = result.value(row, "publishedAt");
    return version;
}

const char* versionSelectSql = R"SQL(
SELECT p."ownerName" AS "ownerName",
       p."packageName" AS "packageName",
       v.version AS version,
       v.manifest::text AS manifest,
       v."checksumSha256" AS "checksumSha256",
       v."sizeBytes"::text AS "sizeBytes",
       v."objectKey" AS "objectKey",
       v.yanked::text AS yanked,
       v."publishedAt"::text AS "publishedAt"
FROM "packageVersions" v
JOIN packages p ON p.id = v."packageId"
)SQL";

struct ParsedEndpoint {
    std::string origin;
    std::string signingHost;
};

ParsedEndpoint parseEndpoint(const std::string& rawEndpoint) {
    std::string endpoint = trimCopy(rawEndpoint);
    while (!endpoint.empty() && endpoint.back() == '/') {
        endpoint.pop_back();
    }

    std::size_t schemeEnd = endpoint.find("://");
    std::string rest = schemeEnd == std::string::npos ? endpoint : endpoint.substr(schemeEnd + 3);
    std::size_t slash = rest.find('/');
    std::string host = slash == std::string::npos ? rest : rest.substr(0, slash);
    return ParsedEndpoint{endpoint, host};
}

std::string canonicalQuery(const std::map<std::string, std::string>& params) {
    std::string query;
    bool first = true;
    for (const auto& [key, value] : params) {
        if (!first) query += "&";
        query += urlEncode(key);
        query += "=";
        query += urlEncode(value);
        first = false;
    }
    return query;
}

std::string s3SigningKey(const ServerConfig& config, const std::string& dateStamp) {
    std::string dateKey = hmacSha256Bytes("AWS4" + config.s3SecretAccessKey, dateStamp);
    std::string regionKey = hmacSha256Bytes(dateKey, config.s3Region);
    std::string serviceKey = hmacSha256Bytes(regionKey, "s3");
    return hmacSha256Bytes(serviceKey, "aws4_request");
}

std::string s3CredentialScope(const ServerConfig& config, const std::string& dateStamp) {
    return dateStamp + "/" + config.s3Region + "/s3/aws4_request";
}

std::string signS3Request(const ServerConfig& config, const ParsedEndpoint& endpoint, const std::string& method,
                          const std::string& canonicalPath, const std::string& canonicalQueryString,
                          const std::string& signedHeaders, const std::string& canonicalHeaders,
                          const std::string& payloadHash, const std::string& amzDate,
                          const std::string& dateStamp) {
    std::string canonicalRequest = method + "\n" +
        canonicalPath + "\n" +
        canonicalQueryString + "\n" +
        canonicalHeaders + "\n" +
        signedHeaders + "\n" +
        payloadHash;

    std::string scope = s3CredentialScope(config, dateStamp);
    std::string requestHash = sha256Hex(canonicalRequest);
    std::string stringToSign = "AWS4-HMAC-SHA256\n" + amzDate + "\n" + scope + "\n" + requestHash;
    std::string signatureBytes = hmacSha256Bytes(s3SigningKey(config, dateStamp), stringToSign);
    return hexEncode(reinterpret_cast<const unsigned char*>(signatureBytes.data()), signatureBytes.size());
}

std::size_t discardCurlWrite(char* ptr, std::size_t size, std::size_t nmemb, void*) {
    (void)ptr;
    return size * nmemb;
}

std::vector<std::string> sortedHighestFirst(std::vector<std::string> versions) {
    std::sort(versions.begin(), versions.end(), [](const std::string& left, const std::string& right) {
        auto leftVersion = SemVer::parse(left);
        auto rightVersion = SemVer::parse(right);
        if (!leftVersion || !rightVersion) {
            return left > right;
        }
        return rightVersion->compare(*leftVersion) < 0;
    });
    return versions;
}

std::string objectKeyFor(const PackageManifest& manifest, const std::string& checksumSha256) {
    return "packages/" + manifest.owner + "/" + manifest.packageName + "/" + manifest.version + "/" + checksumSha256 + ".tar.gz";
}

std::string reasonPhrase(int status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 302: return "Found";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 409: return "Conflict";
        case 500: return "Internal Server Error";
        case 502: return "Bad Gateway";
        default: return "HTTP";
    }
}

std::string stringFromMethod(evhttp_cmd_type method) {
    switch (method) {
        case EVHTTP_REQ_GET: return "GET";
        case EVHTTP_REQ_POST: return "POST";
        case EVHTTP_REQ_PUT: return "PUT";
        case EVHTTP_REQ_DELETE: return "DELETE";
        default: return "UNKNOWN";
    }
}

HttpRequest eventRequestToHttp(evhttp_request* eventRequest) {
    HttpRequest request;
    request.method = stringFromMethod(evhttp_request_get_command(eventRequest));

    const char* rawUri = evhttp_request_get_uri(eventRequest);
    evhttp_uri* uri = evhttp_uri_parse(rawUri ? rawUri : "/");
    if (uri) {
        const char* path = evhttp_uri_get_path(uri);
        const char* query = evhttp_uri_get_query(uri);
        request.path = path && *path ? path : "/";
        request.query = parseQueryString(query ? query : "");
        evhttp_uri_free(uri);
    } else {
        request.path = "/";
    }

    evkeyvalq* inputHeaders = evhttp_request_get_input_headers(eventRequest);
    for (evkeyval* header = inputHeaders->tqh_first; header; header = header->next.tqe_next) {
        request.headers[lowerCopy(header->key)] = header->value ? header->value : "";
    }

    evbuffer* input = evhttp_request_get_input_buffer(eventRequest);
    std::size_t size = evbuffer_get_length(input);
    request.body.resize(size);
    if (size > 0) {
        evbuffer_remove(input, request.body.data(), size);
    }
    return request;
}

void sendEventResponse(evhttp_request* eventRequest, const HttpResponse& response) {
    evkeyvalq* outputHeaders = evhttp_request_get_output_headers(eventRequest);
    bool hasContentType = false;
    for (const auto& [key, value] : response.headers) {
        evhttp_add_header(outputHeaders, key.c_str(), value.c_str());
        if (lowerCopy(key) == "content-type") {
            hasContentType = true;
        }
    }
    if (!hasContentType) {
        evhttp_add_header(outputHeaders, "Content-Type", "application/json");
    }

    evbuffer* buffer = evbuffer_new();
    evbuffer_add(buffer, response.body.data(), response.body.size());
    std::string reason = reasonPhrase(response.status);
    evhttp_send_reply(eventRequest, response.status, reason.c_str(), buffer);
    evbuffer_free(buffer);
}

}

struct RegistryDatabase::State {
    explicit State(const ServerConfig& config, int poolSize) : databaseUrl(config.databaseUrl) {
        for (int index = 0; index < poolSize; ++index) {
            PGconn* connection = PQconnectdb(databaseUrl.c_str());
            if (PQstatus(connection) != CONNECTION_OK) {
                std::string message = PQerrorMessage(connection);
                PQfinish(connection);
                throw std::runtime_error("failed to connect to Postgres: " + message);
            }
            connections.push_back(connection);
            available.push_back(connection);
        }
    }

    ~State() {
        for (PGconn* connection : connections) {
            PQfinish(connection);
        }
    }

    struct Lease {
        State& state;
        PGconn* connection = nullptr;

        explicit Lease(State& state) : state(state), connection(state.acquire()) {}
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& other) noexcept : state(other.state), connection(other.connection) {
            other.connection = nullptr;
        }
        ~Lease() {
            if (connection) {
                state.release(connection);
            }
        }
    };

    Lease lease() {
        return Lease(*this);
    }

    PGconn* acquire() {
        std::unique_lock lock(mutex);
        condition.wait(lock, [&] { return !available.empty(); });
        PGconn* connection = available.back();
        available.pop_back();
        return connection;
    }

    void release(PGconn* connection) {
        {
            std::lock_guard lock(mutex);
            available.push_back(connection);
        }
        condition.notify_one();
    }

    std::string databaseUrl;
    std::vector<PGconn*> connections;
    std::vector<PGconn*> available;
    std::mutex mutex;
    std::condition_variable condition;
};

struct S3Storage::State {
    explicit State(ServerConfig config) : config(std::move(config)), endpoint(parseEndpoint(this->config.s3Endpoint)) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    }

    ~State() {
        curl_global_cleanup();
    }

    ServerConfig config;
    ParsedEndpoint endpoint;
};

struct HttpServer::State {
    State(ServerConfig config, RegistryService& service) : config(std::move(config)), service(service) {}

    ServerConfig config;
    RegistryService& service;
    event_base* eventBase = nullptr;
    evhttp* http = nullptr;
};

ServerConfig ServerConfig::fromEnvironment() {
    ServerConfig config;
    std::string port = envValue("CLOUD_PORT", "8080");
    config.port = std::stoi(port);
    config.databaseUrl = envValue("DATABASE_URL");
    config.s3Endpoint = envValue("S3_ENDPOINT");
    config.s3Bucket = envValue("S3_BUCKET");
    config.s3Region = envValue("S3_REGION", "us-east-1");
    config.s3AccessKeyId = envValue("S3_ACCESS_KEY_ID");
    config.s3SecretAccessKey = envValue("S3_SECRET_ACCESS_KEY");
    config.bootstrapToken = envValue("CLOUD_BOOTSTRAP_TOKEN");
    config.maxPackageBytes = parseSize(envValue("CLOUD_MAX_PACKAGE_BYTES"), config.maxPackageBytes);
    return config;
}

std::optional<std::string> ServerConfig::validateForServe() const {
    if (databaseUrl.empty()) return "DATABASE_URL is required";
    if (s3Endpoint.empty()) return "S3_ENDPOINT is required";
    if (s3Bucket.empty()) return "S3_BUCKET is required";
    if (s3Region.empty()) return "S3_REGION is required";
    if (s3AccessKeyId.empty()) return "S3_ACCESS_KEY_ID is required";
    if (s3SecretAccessKey.empty()) return "S3_SECRET_ACCESS_KEY is required";
    if (bootstrapToken.empty()) return "CLOUD_BOOTSTRAP_TOKEN is required";
    if (maxPackageBytes == 0) return "CLOUD_MAX_PACKAGE_BYTES must be greater than zero";
    return std::nullopt;
}

std::string HttpRequest::header(const std::string& key) const {
    auto found = headers.find(lowerCopy(key));
    return found == headers.end() ? "" : found->second;
}

HttpResponse HttpResponse::json(int status, const JsonValue& value) {
    HttpResponse response;
    response.status = status;
    response.body = value.serialize();
    response.headers["Content-Type"] = "application/json";
    return response;
}

HttpResponse HttpResponse::error(int status, const std::string& message) {
    JsonValue::Object body;
    body["error"] = message;
    return json(status, body);
}

HttpResponse HttpResponse::redirect(const std::string& location) {
    HttpResponse response;
    response.status = 302;
    response.headers["Location"] = location;
    response.headers["Cache-Control"] = "private, max-age=300";
    return response;
}

RegistryDatabase::RegistryDatabase(const ServerConfig& config, int poolSize)
    : state(std::make_unique<State>(config, poolSize)) {}

RegistryDatabase::~RegistryDatabase() = default;

bool RegistryDatabase::ping() {
    try {
        auto lease = state->lease();
        auto result = execSql(lease.connection, "SELECT 1");
        return result.rows() == 1;
    } catch (...) {
        return false;
    }
}

void RegistryDatabase::runMigrations() {
    auto lease = state->lease();
    execSql(lease.connection, migrationSql);
}

std::optional<AuthIdentity> RegistryDatabase::authenticate(const std::string& token) {
    if (token.empty()) {
        return std::nullopt;
    }
    auto lease = state->lease();
    std::string tokenHash = sha256Hex(token);
    auto result = execParams(lease.connection, R"SQL(
SELECT a.id::text AS "accountId", a.name AS "accountName", t.scope AS scope
FROM "authTokens" t
JOIN accounts a ON a.id = t."accountId"
WHERE t."tokenHash" = $1
  AND t."revokedAt" IS NULL
  AND (t."expiresAt" IS NULL OR t."expiresAt" > now())
)SQL", {tokenHash});

    if (result.rows() == 0) {
        return std::nullopt;
    }

    return AuthIdentity{
        std::stol(result.value(0, "accountId")),
        result.value(0, "accountName"),
        result.value(0, "scope")
    };
}

CreatedToken RegistryDatabase::createToken(const std::string& accountName, const std::string& scope) {
    std::string token = randomToken();
    std::string tokenHash = sha256Hex(token);
    std::string tokenPrefix = token.substr(0, std::min<std::size_t>(token.size(), 18));

    auto lease = state->lease();
    execSql(lease.connection, "BEGIN");
    try {
        auto account = execParams(lease.connection, R"SQL(
INSERT INTO accounts (name)
VALUES ($1)
ON CONFLICT (name) DO UPDATE SET name = EXCLUDED.name
RETURNING id::text AS id, name
)SQL", {accountName});

        std::string accountId = account.value(0, "id");
        execParams(lease.connection, R"SQL(
INSERT INTO "authTokens" ("accountId", "tokenHash", "tokenPrefix", scope)
VALUES ($1, $2, $3, $4)
)SQL", {accountId, tokenHash, tokenPrefix, scope});
        execSql(lease.connection, "COMMIT");

        return CreatedToken{
            std::stol(accountId),
            accountName,
            scope,
            token,
            tokenPrefix
        };
    } catch (...) {
        execSql(lease.connection, "ROLLBACK");
        throw;
    }
}

std::optional<PackageVersionInfo> RegistryDatabase::getVersion(
    const std::string& owner,
    const std::string& packageName,
    const std::string& version
) {
    auto lease = state->lease();
    auto result = execParams(lease.connection, std::string(versionSelectSql) + R"SQL(
WHERE p."ownerName" = $1 AND p."packageName" = $2 AND v.version = $3
)SQL", {owner, packageName, version});
    if (result.rows() == 0) {
        return std::nullopt;
    }
    return packageVersionFromRow(result, 0);
}

std::vector<PackageVersionInfo> RegistryDatabase::listVersions(
    const std::string& owner,
    const std::string& packageName,
    bool includeYanked
) {
    auto lease = state->lease();
    std::string sql = std::string(versionSelectSql) + R"SQL(
WHERE p."ownerName" = $1 AND p."packageName" = $2
)SQL";
    if (!includeYanked) {
        sql += " AND v.yanked = false";
    }
    sql += R"SQL(
ORDER BY v.major DESC, v.minor DESC, v.patch DESC, v.prerelease DESC, v."publishedAt" DESC
)SQL";

    auto result = execParams(lease.connection, sql, {owner, packageName});
    std::vector<PackageVersionInfo> versions;
    for (int row = 0; row < result.rows(); ++row) {
        versions.push_back(packageVersionFromRow(result, row));
    }
    return versions;
}

PublishStatus RegistryDatabase::authorizePublish(
    const AuthIdentity& identity,
    const std::string& owner,
    const std::string& packageName,
    const std::string& version,
    std::string& errorMessage
) {
    if (!scopeAllowsPublish(identity)) {
        errorMessage = "token does not allow publishing";
        return PublishStatus::Unauthorized;
    }

    auto lease = state->lease();
    auto package = execParams(lease.connection, R"SQL(
SELECT id::text AS id
FROM packages
WHERE "ownerName" = $1 AND "packageName" = $2
)SQL", {owner, packageName});

    if (package.rows() == 0) {
        if (identity.accountName != owner && !scopeAllowsAdmin(identity)) {
            errorMessage = "token cannot create packages for this owner";
            return PublishStatus::Unauthorized;
        }
        return PublishStatus::Created;
    }

    std::string packageId = package.value(0, "id");
    auto duplicate = execParams(lease.connection, R"SQL(
SELECT 1
FROM "packageVersions"
WHERE "packageId" = $1 AND version = $2
)SQL", {packageId, version});
    if (duplicate.rows() > 0) {
        errorMessage = "package version already exists";
        return PublishStatus::Duplicate;
    }

    if (scopeAllowsAdmin(identity)) {
        return PublishStatus::Created;
    }

    auto allowed = execParams(lease.connection, R"SQL(
SELECT 1
FROM "packageOwners"
WHERE "packageId" = $1 AND "accountId" = $2 AND "canPublish" = true
)SQL", {packageId, std::to_string(identity.accountId)});
    if (allowed.rows() == 0) {
        errorMessage = "token cannot publish this package";
        return PublishStatus::Unauthorized;
    }
    return PublishStatus::Created;
}

PublishResult RegistryDatabase::publishVersion(
    const AuthIdentity& identity,
    const PackageManifest& manifest,
    const std::string& manifestJson,
    const std::string& checksumSha256,
    std::uint64_t sizeBytes,
    const std::string& objectKey
) {
    PublishResult output;
    auto semver = SemVer::parse(manifest.version);
    if (!semver) {
        output.errorMessage = "invalid SemVer version";
        return output;
    }

    auto lease = state->lease();
    execSql(lease.connection, "BEGIN");
    try {
        auto package = execParams(lease.connection, R"SQL(
SELECT id::text AS id
FROM packages
WHERE "ownerName" = $1 AND "packageName" = $2
FOR UPDATE
)SQL", {manifest.owner, manifest.packageName});

        std::string packageId;
        if (package.rows() == 0) {
            if (identity.accountName != manifest.owner && !scopeAllowsAdmin(identity)) {
                execSql(lease.connection, "ROLLBACK");
                output.status = PublishStatus::Unauthorized;
                output.errorMessage = "token cannot create packages for this owner";
                return output;
            }

            auto inserted = execParams(lease.connection, R"SQL(
INSERT INTO packages ("ownerName", "packageName")
VALUES ($1, $2)
RETURNING id::text AS id
)SQL", {manifest.owner, manifest.packageName});
            packageId = inserted.value(0, "id");
            execParams(lease.connection, R"SQL(
INSERT INTO "packageOwners" ("packageId", "accountId", "canPublish")
VALUES ($1, $2, true)
ON CONFLICT DO NOTHING
)SQL", {packageId, std::to_string(identity.accountId)});
        } else {
            packageId = package.value(0, "id");
            if (!scopeAllowsAdmin(identity)) {
                auto allowed = execParams(lease.connection, R"SQL(
SELECT 1
FROM "packageOwners"
WHERE "packageId" = $1 AND "accountId" = $2 AND "canPublish" = true
)SQL", {packageId, std::to_string(identity.accountId)});
                if (allowed.rows() == 0) {
                    execSql(lease.connection, "ROLLBACK");
                    output.status = PublishStatus::Unauthorized;
                    output.errorMessage = "token cannot publish this package";
                    return output;
                }
            }
        }

        auto duplicate = execParams(lease.connection, R"SQL(
SELECT 1 FROM "packageVersions"
WHERE "packageId" = $1 AND version = $2
)SQL", {packageId, manifest.version});
        if (duplicate.rows() > 0) {
            execSql(lease.connection, "ROLLBACK");
            output.status = PublishStatus::Duplicate;
            output.errorMessage = "package version already exists";
            return output;
        }

        execParams(lease.connection, R"SQL(
INSERT INTO "packageVersions"
("packageId", version, major, minor, patch, prerelease, manifest, "checksumSha256", "sizeBytes", "objectKey", "publishedBy")
VALUES ($1, $2, $3, $4, $5, $6, $7::jsonb, $8, $9, $10, $11)
)SQL", {
            packageId,
            manifest.version,
            std::to_string(semver->majorVersion),
            std::to_string(semver->minorVersion),
            std::to_string(semver->patchVersion),
            semver->prerelease,
            manifestJson,
            checksumSha256,
            std::to_string(sizeBytes),
            objectKey,
            std::to_string(identity.accountId)
        });

        execSql(lease.connection, "COMMIT");
        output.status = PublishStatus::Created;
        auto version = getVersion(manifest.owner, manifest.packageName, manifest.version);
        if (version) {
            output.version = *version;
        }
        return output;
    } catch (const PgError& error) {
        execSql(lease.connection, "ROLLBACK");
        if (error.stateCode == "23505") {
            output.status = PublishStatus::Duplicate;
            output.errorMessage = "package version already exists";
            return output;
        }
        output.errorMessage = error.what();
        return output;
    } catch (const std::exception& error) {
        execSql(lease.connection, "ROLLBACK");
        output.errorMessage = error.what();
        return output;
    }
}

YankResult RegistryDatabase::yankVersion(
    const AuthIdentity& identity,
    const std::string& owner,
    const std::string& packageName,
    const std::string& version
) {
    YankResult output;
    auto lease = state->lease();
    execSql(lease.connection, "BEGIN");
    try {
        auto package = execParams(lease.connection, R"SQL(
SELECT id::text AS id
FROM packages
WHERE "ownerName" = $1 AND "packageName" = $2
FOR UPDATE
)SQL", {owner, packageName});

        if (package.rows() == 0) {
            execSql(lease.connection, "ROLLBACK");
            output.status = YankStatus::Missing;
            output.errorMessage = "package not found";
            return output;
        }

        std::string packageId = package.value(0, "id");
        if (!scopeAllowsAdmin(identity)) {
            auto allowed = execParams(lease.connection, R"SQL(
SELECT 1
FROM "packageOwners"
WHERE "packageId" = $1 AND "accountId" = $2 AND "canPublish" = true
)SQL", {packageId, std::to_string(identity.accountId)});
            if (allowed.rows() == 0) {
                execSql(lease.connection, "ROLLBACK");
                output.status = YankStatus::Unauthorized;
                output.errorMessage = "token cannot yank this package";
                return output;
            }
        }

        auto updated = execParams(lease.connection, R"SQL(
UPDATE "packageVersions"
SET yanked = true
WHERE "packageId" = $1 AND version = $2
RETURNING id::text AS id
)SQL", {packageId, version});
        if (updated.rows() == 0) {
            execSql(lease.connection, "ROLLBACK");
            output.status = YankStatus::Missing;
            output.errorMessage = "package version not found";
            return output;
        }

        execSql(lease.connection, "COMMIT");
        output.status = YankStatus::Updated;
        auto versionInfo = getVersion(owner, packageName, version);
        if (versionInfo) {
            output.version = *versionInfo;
        }
        return output;
    } catch (const std::exception& error) {
        execSql(lease.connection, "ROLLBACK");
        output.errorMessage = error.what();
        return output;
    }
}

S3Storage::S3Storage(ServerConfig config) : state(std::make_unique<State>(std::move(config))) {}
S3Storage::~S3Storage() = default;

bool S3Storage::putObject(const std::string& objectKey, const std::string& body, std::string& errorMessage) {
    std::string dateStamp = utcDateStamp();
    std::string amzDate = utcAmzDate();
    std::string payloadHash = sha256Hex(body);
    std::string canonicalPath = "/" + urlEncode(state->config.s3Bucket) + "/" + urlEncode(objectKey, true);
    std::string signedHeaders = "host;x-amz-content-sha256;x-amz-date";
    std::string canonicalHeaders = "host:" + state->endpoint.signingHost + "\n" +
        "x-amz-content-sha256:" + payloadHash + "\n" +
        "x-amz-date:" + amzDate + "\n";
    std::string signature = signS3Request(
        state->config,
        state->endpoint,
        "PUT",
        canonicalPath,
        "",
        signedHeaders,
        canonicalHeaders,
        payloadHash,
        amzDate,
        dateStamp
    );
    std::string scope = s3CredentialScope(state->config, dateStamp);
    std::string authorization = "AWS4-HMAC-SHA256 Credential=" + state->config.s3AccessKeyId + "/" + scope +
        ", SignedHeaders=" + signedHeaders + ", Signature=" + signature;

    CURL* curl = curl_easy_init();
    if (!curl) {
        errorMessage = "failed to initialize curl";
        return false;
    }

    std::string url = state->endpoint.origin + canonicalPath;
    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("Host: " + state->endpoint.signingHost).c_str());
    headers = curl_slist_append(headers, ("x-amz-content-sha256: " + payloadHash).c_str());
    headers = curl_slist_append(headers, ("x-amz-date: " + amzDate).c_str());
    headers = curl_slist_append(headers, ("Authorization: " + authorization).c_str());
    headers = curl_slist_append(headers, "Content-Type: application/gzip");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discardCurlWrite);

    CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (result != CURLE_OK) {
        errorMessage = curl_easy_strerror(result);
        return false;
    }
    if (status < 200 || status >= 300) {
        errorMessage = "S3 upload failed with HTTP " + std::to_string(status);
        return false;
    }
    return true;
}

std::string S3Storage::presignedGetUrl(const std::string& objectKey, int expiresSeconds) const {
    std::string dateStamp = utcDateStamp();
    std::string amzDate = utcAmzDate();
    std::string scope = s3CredentialScope(state->config, dateStamp);
    std::string canonicalPath = "/" + urlEncode(state->config.s3Bucket) + "/" + urlEncode(objectKey, true);
    std::map<std::string, std::string> query = {
        {"X-Amz-Algorithm", "AWS4-HMAC-SHA256"},
        {"X-Amz-Credential", state->config.s3AccessKeyId + "/" + scope},
        {"X-Amz-Date", amzDate},
        {"X-Amz-Expires", std::to_string(expiresSeconds)},
        {"X-Amz-SignedHeaders", "host"}
    };
    std::string canonicalQueryString = canonicalQuery(query);
    std::string canonicalHeaders = "host:" + state->endpoint.signingHost + "\n";
    std::string signature = signS3Request(
        state->config,
        state->endpoint,
        "GET",
        canonicalPath,
        canonicalQueryString,
        "host",
        canonicalHeaders,
        "UNSIGNED-PAYLOAD",
        amzDate,
        dateStamp
    );
    return state->endpoint.origin + canonicalPath + "?" + canonicalQueryString + "&X-Amz-Signature=" + signature;
}

RegistryService::RegistryService(ServerConfig config, RegistryDatabase& database, S3Storage& storage)
    : config(std::move(config)), database(database), storage(storage) {}

HttpResponse RegistryService::handle(const HttpRequest& request) {
    try {
        std::vector<std::string> segments = splitPath(request.path);
        if (request.method == "GET" && segments.size() == 1 && segments[0] == "health") {
            return handleHealth();
        }
        if (request.method == "POST" && segments.size() == 2 && segments[0] == "v1" && segments[1] == "tokens") {
            return handleTokenCreate(request);
        }
        if (segments.size() >= 4 && segments[0] == "v1" && segments[1] == "packages") {
            return handlePackageRoute(request, segments);
        }
        return HttpResponse::error(404, "route not found");
    } catch (const std::exception& error) {
        return HttpResponse::error(500, error.what());
    }
}

HttpResponse RegistryService::handleHealth() {
    JsonValue::Object body;
    body["status"] = "ok";
    body["database"] = database.ping();
    body["storage"] = !config.s3Endpoint.empty() && !config.s3Bucket.empty();
    return HttpResponse::json(200, body);
}

HttpResponse RegistryService::handleTokenCreate(const HttpRequest& request) {
    std::string token = bearerToken(request.header("authorization"));
    if (token.empty() || !constantTimeEquals(token, config.bootstrapToken)) {
        return HttpResponse::error(401, "bootstrap token required");
    }

    JsonValue body = JsonValue::parse(request.body);
    auto accountName = body.getString("accountName");
    std::string scope = body.getString("scope").value_or("publish");
    if (!accountName || !isValidOwnerName(*accountName)) {
        return HttpResponse::error(400, "accountName must be a lower-kebab owner name");
    }
    if (scope != "publish" && scope != "admin") {
        return HttpResponse::error(400, "scope must be publish or admin");
    }

    CreatedToken created = database.createToken(*accountName, scope);
    JsonValue::Object response;
    response["accountName"] = created.accountName;
    response["scope"] = created.scope;
    response["token"] = created.token;
    response["tokenPrefix"] = created.tokenPrefix;
    return HttpResponse::json(201, response);
}

HttpResponse RegistryService::handlePackageRoute(const HttpRequest& request, const std::vector<std::string>& segments) {
    std::string owner = segments[2];
    if (!owner.empty() && owner.front() == '@') {
        owner.erase(owner.begin());
    }
    std::string packageName = segments[3];
    if (!isValidOwnerName(owner) || !isValidPackageName(packageName)) {
        return HttpResponse::error(400, "package route must use lower-kebab owner and package names");
    }

    if (request.method == "GET" && segments.size() == 4) {
        std::vector<PackageVersionInfo> versions = database.listVersions(owner, packageName, false);
        if (versions.empty()) {
            return HttpResponse::error(404, "package not found");
        }
        std::vector<std::string> versionNames;
        for (const auto& version : versions) {
            versionNames.push_back(version.version);
        }
        versionNames = sortedHighestFirst(std::move(versionNames));

        JsonValue::Object body;
        body["name"] = "@" + owner + "/" + packageName;
        body["latestVersion"] = versionNames.empty() ? JsonValue() : JsonValue(versionNames.front());
        body["versionCount"] = static_cast<std::int64_t>(versions.size());
        return HttpResponse::json(200, body);
    }

    if (request.method == "GET" && segments.size() == 5 && segments[4] == "versions") {
        std::vector<PackageVersionInfo> versions = database.listVersions(owner, packageName, true);
        JsonValue::Array items;
        for (const auto& version : versions) {
            items.push_back(versionToJson(version));
        }
        JsonValue::Object body;
        body["name"] = "@" + owner + "/" + packageName;
        body["versions"] = items;
        return HttpResponse::json(200, body);
    }

    if (request.method == "GET" && segments.size() == 5 && segments[4] == "resolve") {
        std::string range = "*";
        auto found = request.query.find("range");
        if (found != request.query.end()) {
            range = found->second;
        }
        std::vector<PackageVersionInfo> versions = database.listVersions(owner, packageName, false);
        std::optional<PackageVersionInfo> best;
        for (const auto& version : versions) {
            auto semver = SemVer::parse(version.version);
            if (!semver || !semver->matchesRange(range)) {
                continue;
            }
            if (!best) {
                best = version;
                continue;
            }
            auto bestSemVer = SemVer::parse(best->version);
            if (bestSemVer && bestSemVer->compare(*semver) < 0) {
                best = version;
            }
        }
        if (!best) {
            return HttpResponse::error(404, "no version matches the requested range");
        }
        return HttpResponse::json(200, versionToJson(*best));
    }

    if (segments.size() >= 6 && segments[4] == "versions") {
        std::string version = segments[5];
        if (!SemVer::parse(version)) {
            return HttpResponse::error(400, "version must be SemVer");
        }
        if (request.method == "GET" && segments.size() == 6) {
            auto versionInfo = database.getVersion(owner, packageName, version);
            if (!versionInfo) {
                return HttpResponse::error(404, "package version not found");
            }
            return HttpResponse::json(200, versionToJson(*versionInfo));
        }
        if (request.method == "GET" && segments.size() == 7 && segments[6] == "source") {
            auto versionInfo = database.getVersion(owner, packageName, version);
            if (!versionInfo) {
                return HttpResponse::error(404, "package version not found");
            }
            return HttpResponse::redirect(storage.presignedGetUrl(versionInfo->objectKey));
        }
        if (request.method == "POST" && segments.size() == 6) {
            return handlePublish(request, owner, packageName, version);
        }
        if (request.method == "POST" && segments.size() == 7 && segments[6] == "yank") {
            return handleYank(request, owner, packageName, version);
        }
    }

    return HttpResponse::error(404, "route not found");
}

HttpResponse RegistryService::handlePublish(
    const HttpRequest& request,
    const std::string& owner,
    const std::string& packageName,
    const std::string& version
) {
    HttpResponse authFailure;
    auto identity = authenticateBearer(request, authFailure);
    if (!identity) {
        return authFailure;
    }

    std::string errorMessage;
    auto parts = parseMultipartForm(request.header("content-type"), request.body, errorMessage);
    if (!parts) {
        return HttpResponse::error(400, errorMessage);
    }
    auto manifestPart = parts->find("manifest");
    auto archivePart = parts->find("archive");
    if (manifestPart == parts->end() || archivePart == parts->end()) {
        return HttpResponse::error(400, "multipart upload requires manifest and archive fields");
    }

    auto manifest = parsePackageManifest(manifestPart->second.data, owner, packageName, version, errorMessage);
    if (!manifest) {
        return HttpResponse::error(400, errorMessage);
    }

    PublishStatus preflight = database.authorizePublish(*identity, owner, packageName, version, errorMessage);
    if (preflight == PublishStatus::Duplicate) {
        return HttpResponse::error(409, errorMessage);
    }
    if (preflight == PublishStatus::Unauthorized) {
        return HttpResponse::error(403, errorMessage);
    }
    if (preflight == PublishStatus::Error) {
        return HttpResponse::error(500, errorMessage.empty() ? "publish preflight failed" : errorMessage);
    }

    ArchiveValidation archive = validateSourceArchive(archivePart->second.data, config.maxPackageBytes);
    if (!archive.ok) {
        return HttpResponse::error(400, archive.errorMessage);
    }

    std::string checksum = sha256Hex(archivePart->second.data);
    std::string objectKey = objectKeyFor(*manifest, checksum);
    if (!storage.putObject(objectKey, archivePart->second.data, errorMessage)) {
        return HttpResponse::error(502, "object storage upload failed: " + errorMessage);
    }

    PublishResult result = database.publishVersion(
        *identity,
        *manifest,
        manifestPart->second.data,
        checksum,
        archivePart->second.data.size(),
        objectKey
    );
    if (result.status == PublishStatus::Duplicate) {
        return HttpResponse::error(409, result.errorMessage);
    }
    if (result.status == PublishStatus::Unauthorized) {
        return HttpResponse::error(403, result.errorMessage);
    }
    if (result.status != PublishStatus::Created) {
        return HttpResponse::error(500, result.errorMessage.empty() ? "publish failed" : result.errorMessage);
    }

    return HttpResponse::json(201, versionToJson(result.version, storage.presignedGetUrl(result.version.objectKey)));
}

HttpResponse RegistryService::handleYank(
    const HttpRequest& request,
    const std::string& owner,
    const std::string& packageName,
    const std::string& version
) {
    HttpResponse authFailure;
    auto identity = authenticateBearer(request, authFailure);
    if (!identity) {
        return authFailure;
    }

    YankResult result = database.yankVersion(*identity, owner, packageName, version);
    if (result.status == YankStatus::Missing) {
        return HttpResponse::error(404, result.errorMessage);
    }
    if (result.status == YankStatus::Unauthorized) {
        return HttpResponse::error(403, result.errorMessage);
    }
    if (result.status != YankStatus::Updated) {
        return HttpResponse::error(500, result.errorMessage.empty() ? "yank failed" : result.errorMessage);
    }
    return HttpResponse::json(200, versionToJson(result.version));
}

std::optional<AuthIdentity> RegistryService::authenticateBearer(const HttpRequest& request, HttpResponse& failure) {
    std::string token = bearerToken(request.header("authorization"));
    if (token.empty()) {
        failure = HttpResponse::error(401, "bearer token required");
        return std::nullopt;
    }
    auto identity = database.authenticate(token);
    if (!identity) {
        failure = HttpResponse::error(401, "invalid bearer token");
        return std::nullopt;
    }
    return identity;
}

HttpServer::HttpServer(ServerConfig config, RegistryService& service)
    : state(std::make_unique<State>(std::move(config), service)) {}

HttpServer::~HttpServer() {
    if (state->http) {
        evhttp_free(state->http);
    }
    if (state->eventBase) {
        event_base_free(state->eventBase);
    }
}

void HttpServer::start() {
    state->eventBase = event_base_new();
    if (!state->eventBase) {
        throw std::runtime_error("failed to create event base");
    }
    state->http = evhttp_new(state->eventBase);
    if (!state->http) {
        throw std::runtime_error("failed to create HTTP server");
    }

    evhttp_set_gencb(state->http, [](evhttp_request* request, void* context) {
        auto* serverState = static_cast<State*>(context);
        HttpRequest httpRequest = eventRequestToHttp(request);
        HttpResponse response = serverState->service.handle(httpRequest);
        sendEventResponse(request, response);
    }, state.get());

    if (evhttp_bind_socket(state->http, "0.0.0.0", state->config.port) != 0) {
        throw std::runtime_error("failed to bind cloud-server to port " + std::to_string(state->config.port));
    }

    std::cout << "cloud-server listening on port " << state->config.port << "\n";
    event_base_dispatch(state->eventBase);
}

std::vector<std::string> splitPath(const std::string& path) {
    std::vector<std::string> segments;
    std::string current;
    for (char ch : path) {
        if (ch == '/') {
            if (!current.empty()) {
                segments.push_back(urlDecode(current));
                current.clear();
            }
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty()) {
        segments.push_back(urlDecode(current));
    }
    return segments;
}

std::map<std::string, std::string> parseQueryString(const std::string& query) {
    std::map<std::string, std::string> values;
    std::size_t position = 0;
    while (position <= query.size()) {
        std::size_t next = query.find('&', position);
        std::string pair = query.substr(position, next == std::string::npos ? std::string::npos : next - position);
        if (!pair.empty()) {
            std::size_t equals = pair.find('=');
            std::string key = equals == std::string::npos ? pair : pair.substr(0, equals);
            std::string value = equals == std::string::npos ? "" : pair.substr(equals + 1);
            values[urlDecode(key)] = urlDecode(value);
        }
        if (next == std::string::npos) {
            break;
        }
        position = next + 1;
    }
    return values;
}

JsonValue versionToJson(const PackageVersionInfo& version, const std::optional<std::string>& sourceUrl) {
    JsonValue::Object body;
    body["name"] = version.name;
    body["version"] = version.version;
    body["yanked"] = version.yanked;
    body["checksumSha256"] = version.checksumSha256;
    body["sizeBytes"] = static_cast<std::int64_t>(version.sizeBytes);
    body["publishedAt"] = version.publishedAt;
    body["manifest"] = JsonValue::parse(version.manifestJson);
    if (sourceUrl) {
        body["sourceUrl"] = *sourceUrl;
    }
    return body;
}

}
