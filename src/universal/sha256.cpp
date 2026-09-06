#include <universal/q_shared.h>
#include "sha256.h"

#include <qcommon/qcommon.h>

#include <cstring>

// SHA-256 per FIPS 180-4. The round constants are the first 32 bits of the
// fractional parts of the cube roots of the first 64 primes; the initial state
// is the same of the square roots of the first 8. Both are from the standard.

static const uint32_t sha256_k[64] =
{
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static inline uint32_t SHA256_Ror(uint32_t x, uint32_t n)
{
    return (x >> n) | (x << (32 - n));
}

static void SHA256_Transform(sha256_ctx *ctx, const uint8_t block[SHA256_BLOCK_SIZE])
{
    uint32_t w[64];

    for (int i = 0; i < 16; ++i)
    {
        w[i] = ((uint32_t)block[i * 4 + 0] << 24)
             | ((uint32_t)block[i * 4 + 1] << 16)
             | ((uint32_t)block[i * 4 + 2] << 8)
             | ((uint32_t)block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i)
    {
        const uint32_t s0 = SHA256_Ror(w[i - 15], 7) ^ SHA256_Ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = SHA256_Ror(w[i - 2], 17) ^ SHA256_Ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = ctx->state[0];
    uint32_t b = ctx->state[1];
    uint32_t c = ctx->state[2];
    uint32_t d = ctx->state[3];
    uint32_t e = ctx->state[4];
    uint32_t f = ctx->state[5];
    uint32_t g = ctx->state[6];
    uint32_t h = ctx->state[7];

    for (int i = 0; i < 64; ++i)
    {
        const uint32_t S1 = SHA256_Ror(e, 6) ^ SHA256_Ror(e, 11) ^ SHA256_Ror(e, 25);
        const uint32_t ch = (e & f) ^ ((~e) & g);
        const uint32_t t1 = h + S1 + ch + sha256_k[i] + w[i];
        const uint32_t S0 = SHA256_Ror(a, 2) ^ SHA256_Ror(a, 13) ^ SHA256_Ror(a, 22);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = S0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

void SHA256_Init(sha256_ctx *ctx)
{
    iassert(ctx);

    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
    ctx->bitCount = 0;
    ctx->bufferLen = 0;
}

void SHA256_Update(sha256_ctx *ctx, const void *data, size_t len)
{
    iassert(ctx);
    iassert(data || len == 0);

    const uint8_t *in = (const uint8_t *)data;
    ctx->bitCount += (uint64_t)len * 8;

    // Top up a partial block first, then run whole blocks straight out of the
    // caller's buffer, then keep the remainder.
    if (ctx->bufferLen)
    {
        const uint32_t need = SHA256_BLOCK_SIZE - ctx->bufferLen;
        const uint32_t take = (len < need) ? (uint32_t)len : need;

        memcpy(ctx->buffer + ctx->bufferLen, in, take);
        ctx->bufferLen += take;
        in += take;
        len -= take;

        if (ctx->bufferLen < SHA256_BLOCK_SIZE)
            return;

        SHA256_Transform(ctx, ctx->buffer);
        ctx->bufferLen = 0;
    }

    while (len >= SHA256_BLOCK_SIZE)
    {
        SHA256_Transform(ctx, in);
        in += SHA256_BLOCK_SIZE;
        len -= SHA256_BLOCK_SIZE;
    }

    if (len)
    {
        memcpy(ctx->buffer, in, len);
        ctx->bufferLen = (uint32_t)len;
    }
}

void SHA256_Final(sha256_ctx *ctx, uint8_t digest[SHA256_DIGEST_SIZE])
{
    iassert(ctx);
    iassert(digest);

    const uint64_t bitCount = ctx->bitCount;

    // 0x80, then zeroes, until 8 bytes short of a block boundary.
    uint8_t pad = 0x80;
    SHA256_Update(ctx, &pad, 1);
    ctx->bitCount = bitCount;   // padding is not message length

    pad = 0x00;
    while (ctx->bufferLen != SHA256_BLOCK_SIZE - 8)
    {
        SHA256_Update(ctx, &pad, 1);
        ctx->bitCount = bitCount;
    }

    uint8_t lengthBytes[8];
    for (int i = 0; i < 8; ++i)
        lengthBytes[i] = (uint8_t)(bitCount >> (56 - i * 8));

    SHA256_Update(ctx, lengthBytes, 8);

    for (int i = 0; i < 8; ++i)
    {
        digest[i * 4 + 0] = (uint8_t)(ctx->state[i] >> 24);
        digest[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        digest[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        digest[i * 4 + 3] = (uint8_t)(ctx->state[i]);
    }

    memset(ctx, 0, sizeof(*ctx));
}

static void SHA256_ToHex(const uint8_t digest[SHA256_DIGEST_SIZE], char *out)
{
    static const char hex[] = "0123456789abcdef";

    for (int i = 0; i < SHA256_DIGEST_SIZE; ++i)
    {
        out[i * 2 + 0] = hex[(digest[i] >> 4) & 0xF];
        out[i * 2 + 1] = hex[digest[i] & 0xF];
    }
    out[SHA256_DIGEST_SIZE * 2] = 0;
}

void SHA256_HashToHex(const void *data, size_t len, char *out)
{
    iassert(out);

    sha256_ctx ctx;
    uint8_t digest[SHA256_DIGEST_SIZE];

    SHA256_Init(&ctx);
    SHA256_Update(&ctx, data, len);
    SHA256_Final(&ctx, digest);
    SHA256_ToHex(digest, out);
}

void SHA256_HashPassword(const char *password, const char *salt, char *out)
{
    iassert(password);
    iassert(salt);
    iassert(out);

    sha256_ctx ctx;
    uint8_t digest[SHA256_DIGEST_SIZE];

    // First pass over salt || password, then iterate on the digest. Feeding the
    // salt back in on every round keeps each iteration bound to this record, so
    // work done attacking one admin's hash is worthless against another's.
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, salt, strlen(salt));
    SHA256_Update(&ctx, password, strlen(password));
    SHA256_Final(&ctx, digest);

    const size_t saltLen = strlen(salt);
    for (int i = 1; i < SHA256_PASSWORD_ITERATIONS; ++i)
    {
        SHA256_Init(&ctx);
        SHA256_Update(&ctx, digest, sizeof(digest));
        SHA256_Update(&ctx, salt, saltLen);
        SHA256_Final(&ctx, digest);
    }

    SHA256_ToHex(digest, out);
}

void SHA256_GenerateSalt(char *out)
{
    iassert(out);

    uint8_t raw[SHA256_DIGEST_SIZE];

    if (Sys_RandomBytes(raw, sizeof(raw)))
    {
        SHA256_ToHex(raw, out);
        return;
    }

    // A predictable salt does not break authentication, but it does let one
    // precomputed table cover every server that hit this path, so say so loudly
    // rather than silently degrading.
    Com_PrintWarning(15, "WARNING: no system entropy available for the admin password salt.\n");
    Com_PrintWarning(15, "WARNING: falling back to a weak salt; admin hashes on this server are easier to attack.\n");

    sha256_ctx ctx;
    const int64_t now = Sys_Milliseconds();
    const void *addr = (const void *)out;

    SHA256_Init(&ctx);
    SHA256_Update(&ctx, &now, sizeof(now));
    SHA256_Update(&ctx, &addr, sizeof(addr));
    SHA256_Final(&ctx, raw);
    SHA256_ToHex(raw, out);
}

bool SHA256_HexEquals(const char *a, const char *b)
{
    if (!a || !b)
        return false;

    // Compare the full fixed width every time. Returning early on the first
    // mismatching character would leak, through timing, how many leading
    // characters of a guessed hash were right.
    uint32_t diff = 0;

    for (int i = 0; i < SHA256_DIGEST_SIZE * 2; ++i)
    {
        const uint8_t ca = (uint8_t)a[i];
        const uint8_t cb = (uint8_t)b[i];

        diff |= (uint32_t)(ca ^ cb);

        if (ca == 0 || cb == 0)
            break;
    }

    return diff == 0;
}
