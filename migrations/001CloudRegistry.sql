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
