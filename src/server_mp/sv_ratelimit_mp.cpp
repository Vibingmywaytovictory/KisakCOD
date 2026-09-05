// Portions of this file are derived from CoD4x_Server
//   https://github.com/callofduty4x/CoD4x_Server
//   Copyright (C) 2010-2013 Ninja and TheKelm
//   Copyright (C) the CoD4x authors
// which in turn carries the leaky bucket limiter from ioquake3
//   Copyright (C) 1999-2005 Id Software, Inc.
// Licensed under the GNU Affero General Public License v3 (see LICENSE.AGPLv3).
//
// Adapted to KisakCOD's engine APIs. The combined work is conveyed under
// GPLv3 section 13; see LICENSING.md.

#ifndef KISAK_MP
#error This File is MultiPlayer Only
#endif

#include "sv_ratelimit_mp.h"
#include "server_mp.h"

#include <qcommon/qcommon.h>
#include <win32/win_local.h>

#include <string.h>

// CoD4x sizes its pool from sv_queryIgnoreMegs and mallocs it at startup. A
// fixed pool is used here instead, matching how the rest of this engine sizes
// things: nothing to fail at startup, nothing to leak, and the table is small
// enough that the dvar was never worth the knob. 16384 distinct source
// addresses is a wide enough window that a flood has to come from more
// sources than that before the pool fills, and once it does the limiter
// fails closed rather than opening up.
#define SV_RATELIMIT_MAX_BUCKETS 16384

// Power of two: the hash is masked with this, not divided.
#define SV_RATELIMIT_HASH_SIZE 4096

struct SvLeakyBucket
{
    netadrtype_t     type;      // NA_BAD marks the entry as free
    uint8_t          ip[4];
    uint32_t         lastTime;  // Sys_Milliseconds, wraps every ~49 days
    int32_t          burst;
    int32_t          hash;
    SvLeakyBucket   *prev;
    SvLeakyBucket   *next;
};

static SvLeakyBucket  g_svRateBuckets[SV_RATELIMIT_MAX_BUCKETS];
static SvLeakyBucket *g_svRateHashes[SV_RATELIMIT_HASH_SIZE];
static SvLeakyBucket  g_svRateGlobal[SV_RATELIMIT_BUCKET_COUNT];

// Mixed into the address hash so a caller cannot pick addresses that all land
// in one chain and turn the lookup into a linear scan.
static uint32_t g_svRateHashSeed;

static const dvar_t *sv_queryLimit;
static const dvar_t *sv_queryIgnoreTime;

void __cdecl SV_RateLimitInit()
{
    // CoD4x also has sv_queryIgnoreMegs, which sizes its bucket pool. There is
    // no counterpart here because the pool is fixed; the name is not
    // registered rather than accepted and ignored, so a config carried over
    // from CoD4x reports it as unknown instead of appearing to take effect.
    sv_queryLimit = Dvar_RegisterBool(
        "sv_queryLimit", true, DVAR_ARCHIVE,
        "Rate limit connectionless queries, so this server cannot be used to amplify a flood");

    sv_queryIgnoreTime = Dvar_RegisterInt(
        "sv_queryIgnoreTime", 2000, 100, 100000, DVAR_ARCHIVE,
        "Milliseconds one address must wait between connectionless queries");

    Com_Memset(g_svRateBuckets, 0, sizeof(g_svRateBuckets));
    Com_Memset(g_svRateHashes, 0, sizeof(g_svRateHashes));
    Com_Memset(g_svRateGlobal, 0, sizeof(g_svRateGlobal));

    // NA_BAD is 1 in this tree, not 0, so a zeroed pool is not a free pool.
    for (uint32_t i = 0; i < ARRAY_COUNT(g_svRateBuckets); ++i)
        g_svRateBuckets[i].type = NA_BAD;

    g_svRateHashSeed = Sys_Milliseconds();
}

int32_t __cdecl SV_QueryIgnoreTime()
{
    return sv_queryIgnoreTime ? sv_queryIgnoreTime->current.integer : 2000;
}

static int32_t SV_RateLimitHash(const netadr_t *address)
{
    uint32_t hash = 0;

    for (uint32_t i = 0; i < ARRAY_COUNT(address->ip); ++i)
        hash += (uint32_t)address->ip[i] * (i + 119);

    hash ^= (hash >> 10) ^ (hash >> 20) ^ g_svRateHashSeed;

    return (int32_t)(hash & (SV_RATELIMIT_HASH_SIZE - 1));
}

static void SV_RateLimitUnlink(SvLeakyBucket *bucket)
{
    if (bucket->prev)
        bucket->prev->next = bucket->next;
    else
        g_svRateHashes[bucket->hash] = bucket->next;

    if (bucket->next)
        bucket->next->prev = bucket->prev;
}

// Finds the bucket for this address, or takes over an expired one. Returns
// null only when every bucket is live, meaning more distinct addresses are
// mid-window than the pool holds. The caller then fails closed and drops the
// packet: under a flood that wide, refusing to answer is the whole point.
static SvLeakyBucket *SV_RateLimitBucketFor(const netadr_t *address, int32_t burst, int32_t periodMsec)
{
    const int32_t hash = SV_RateLimitHash(address);
    const uint32_t now = Sys_Milliseconds();

    for (SvLeakyBucket *bucket = g_svRateHashes[hash]; bucket; bucket = bucket->next)
    {
        if (bucket->type == NA_IP && !memcmp(bucket->ip, address->ip, sizeof(bucket->ip)))
            return bucket;
    }

    for (uint32_t i = 0; i < ARRAY_COUNT(g_svRateBuckets); ++i)
    {
        SvLeakyBucket *bucket = &g_svRateBuckets[i];

        // Signed difference of unsigned times, so the millisecond counter
        // wrapping reads as a large interval rather than a negative one.
        const int32_t interval = (int32_t)(now - bucket->lastTime);

        if (bucket->type != NA_BAD && interval > burst * periodMsec)
        {
            SV_RateLimitUnlink(bucket);
            Com_Memset(bucket, 0, sizeof(*bucket));
            bucket->type = NA_BAD;
        }

        if (bucket->type == NA_BAD)
        {
            bucket->type = NA_IP;
            memcpy(bucket->ip, address->ip, sizeof(bucket->ip));
            bucket->lastTime = now;
            bucket->burst = 0;
            bucket->hash = hash;

            bucket->prev = nullptr;
            bucket->next = g_svRateHashes[hash];

            if (g_svRateHashes[hash])
                g_svRateHashes[hash]->prev = bucket;

            g_svRateHashes[hash] = bucket;

            return bucket;
        }
    }

    return nullptr;
}

// The leaky bucket proper: drain one token per period, refuse once `burst`
// tokens are outstanding.
static bool SV_RateLimitBucket(SvLeakyBucket *bucket, int32_t burst, int32_t periodMsec)
{
    if (!bucket)
        return true;

    const uint32_t now = Sys_Milliseconds();
    const int32_t interval = (int32_t)(now - bucket->lastTime);
    const int32_t expired = interval / periodMsec;
    const int32_t expiredRemainder = interval % periodMsec;

    if (expired > bucket->burst)
    {
        bucket->burst = 0;
        bucket->lastTime = now;
    }
    else
    {
        bucket->burst -= expired;
        bucket->lastTime = now - expiredRemainder;
    }

    if (bucket->burst < burst)
    {
        ++bucket->burst;
        return false;
    }

    return true;
}

bool __cdecl SV_RateLimitAddress(netadr_t from, int32_t burst, int32_t periodMsec)
{
    if (!sv_queryLimit || !sv_queryLimit->current.enabled)
        return false;

    // A bot address is the server talking to itself.
    if (from.type != NA_IP || Sys_IsLANAddress(from))
        return false;

    if (periodMsec < 1)
        periodMsec = 1;

    return SV_RateLimitBucket(SV_RateLimitBucketFor(&from, burst, periodMsec), burst, periodMsec);
}

bool __cdecl SV_RateLimitGlobal(SvRateLimitBucket bucket, int32_t burst, int32_t periodMsec)
{
    if (!sv_queryLimit || !sv_queryLimit->current.enabled)
        return false;

    if (bucket < 0 || bucket >= SV_RATELIMIT_BUCKET_COUNT)
    {
        iassert(bucket >= 0 && bucket < SV_RATELIMIT_BUCKET_COUNT);
        return false;
    }

    if (periodMsec < 1)
        periodMsec = 1;

    return SV_RateLimitBucket(&g_svRateGlobal[bucket], burst, periodMsec);
}
