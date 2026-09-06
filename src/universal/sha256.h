#pragma once

#include <cstdint>
#include <cstddef>

// SHA-256, FIPS 180-4. Written from the standard rather than taken from CoD4x,
// which carries its own copy under AGPL; the algorithm is a published spec and
// this implementation owes nothing to theirs.
//
// This exists for the admin password store (sv_auth_mp.cpp) and nothing else.
// It is NOT a password hash on its own -- a single SHA-256 pass is far too fast
// to stand alone against an offline attack on a stolen admin file. What makes it
// tolerable here is the salt plus the iteration count in SHA256_HashPassword,
// and the fact that the file it protects is server-local.

#define SHA256_DIGEST_SIZE  32
#define SHA256_BLOCK_SIZE   64

// Room for the 64 hex characters plus a terminator.
#define SHA256_HEX_SIZE     65

struct sha256_ctx
{
    uint32_t state[8];
    uint64_t bitCount;
    uint8_t  buffer[SHA256_BLOCK_SIZE];
    uint32_t bufferLen;
};

void SHA256_Init(sha256_ctx *ctx);
void SHA256_Update(sha256_ctx *ctx, const void *data, size_t len);
void SHA256_Final(sha256_ctx *ctx, uint8_t digest[SHA256_DIGEST_SIZE]);

// Convenience: hash one buffer straight to lowercase hex. `out` must hold
// SHA256_HEX_SIZE bytes.
void SHA256_HashToHex(const void *data, size_t len, char *out);

// Salted, iterated password hash. Deliberately not a bare SHA-256 of the
// password: iterating raises the cost of an offline dictionary attack on the
// admin file by the iteration count, and the salt stops one precomputed table
// covering every server. `out` must hold SHA256_HEX_SIZE bytes.
//
// The iteration count is fixed rather than stored per record on purpose -- it is
// part of the format, so changing it invalidates existing hashes and that has to
// be a deliberate migration, not a silent config change.
#define SHA256_PASSWORD_ITERATIONS 60000

void SHA256_HashPassword(const char *password, const char *salt, char *out);

// Fills `out` with SHA256_HEX_SIZE bytes: a fresh random salt as lowercase hex.
// Uses the platform CSPRNG; falls back to a time/address mix only if that fails,
// which is logged as a warning because a predictable salt weakens the store.
void SHA256_GenerateSalt(char *out);

// Constant-time comparison of two NUL-terminated hex digests. Used instead of
// strcmp so that a wrong password cannot be narrowed down by timing how long the
// comparison took.
bool SHA256_HexEquals(const char *a, const char *b);
