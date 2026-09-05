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

// One address gets a separate allowance per query kind. CoD4x keeps a single
// bucket per address and passes a different burst and period from each call
// site, so whichever query an address sent last drains the budget the others
// need: a server browser refreshing getstatus and getinfo eats the allowance
// the getchallenge behind it is about to want, and the connect then fails for
// a reason that has nothing to do with the player. Found by flooding rcon from
// one address and watching every getchallenge from it be refused afterwards.
struct SvLeakyBucket
{
    netadrtype_t     type;      // NA_BAD marks the entry as free
    uint8_t          ip[4];
    uint32_t         lastSeen;  // any category, for reclaiming the entry
    uint32_t         lastTime[SV_RATELIMIT_BUCKET_COUNT];
    int32_t          burst[SV_RATELIMIT_BUCKET_COUNT];
    int32_t          hash;
    SvLeakyBucket   *prev;
    SvLeakyBucket   *next;
};

static SvLeakyBucket  g_svRateBuckets[SV_RATELIMIT_MAX_BUCKETS];
static SvLeakyBucket *g_svRateHashes[SV_RATELIMIT_HASH_SIZE];

static uint32_t g_svRateGlobalTime[SV_RATELIMIT_BUCKET_COUNT];
static int32_t  g_svRateGlobalBurst[SV_RATELIMIT_BUCKET_COUNT];

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
    Com_Memset(g_svRateGlobalTime, 0, sizeof(g_svRateGlobalTime));
    Com_Memset(g_svRateGlobalBurst, 0, sizeof(g_svRateGlobalBurst));

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

// The leaky bucket proper: drain one token per period, refuse once `burst`
// tokens are outstanding. Operates on one category's counters, whether those
// belong to an address entry or to the global pair.
static bool SV_RateLimitDrain(uint32_t *lastTime, int32_t *outstanding, int32_t burst, int32_t periodMsec)
{
    const uint32_t now = Sys_Milliseconds();

    // Signed difference of unsigned times, so the millisecond counter wrapping
    // reads as a large interval rather than a negative one.
    const int32_t interval = (int32_t)(now - *lastTime);
    const int32_t expired = interval / periodMsec;
    const int32_t expiredRemainder = interval % periodMsec;

    if (expired > *outstanding)
    {
        *outstanding = 0;
        *lastTime = now;
    }
    else
    {
        *outstanding -= expired;
        *lastTime = now - expiredRemainder;
    }

    if (*outstanding < burst)
    {
        ++*outstanding;
        return false;
    }

    return true;
}

// Finds the entry for this address, or takes over one nothing has used for
// longer than staleAfterMsec. Returns null only when every entry is live,
// meaning more distinct addresses are mid-window than the pool holds. The
// caller then fails closed and drops the packet: under a flood that wide,
// refusing to answer is the whole point.
static SvLeakyBucket *SV_RateLimitEntryFor(const netadr_t *address, int32_t staleAfterMsec)
{
    const int32_t hash = SV_RateLimitHash(address);
    const uint32_t now = Sys_Milliseconds();

    for (SvLeakyBucket *bucket = g_svRateHashes[hash]; bucket; bucket = bucket->next)
    {
        if (bucket->type == NA_IP && !memcmp(bucket->ip, address->ip, sizeof(bucket->ip)))
        {
            bucket->lastSeen = now;
            return bucket;
        }
    }

    for (uint32_t i = 0; i < ARRAY_COUNT(g_svRateBuckets); ++i)
    {
        SvLeakyBucket *bucket = &g_svRateBuckets[i];

        if (bucket->type != NA_BAD && (int32_t)(now - bucket->lastSeen) > staleAfterMsec)
        {
            SV_RateLimitUnlink(bucket);
            Com_Memset(bucket, 0, sizeof(*bucket));
            bucket->type = NA_BAD;
        }

        if (bucket->type == NA_BAD)
        {
            bucket->type = NA_IP;
            memcpy(bucket->ip, address->ip, sizeof(bucket->ip));
            bucket->lastSeen = now;
            bucket->hash = hash;

            for (uint32_t c = 0; c < SV_RATELIMIT_BUCKET_COUNT; ++c)
            {
                bucket->lastTime[c] = now;
                bucket->burst[c] = 0;
            }

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

bool __cdecl SV_RateLimitAddress(netadr_t from, SvRateLimitBucket category, int32_t burst, int32_t periodMsec)
{
    if (!sv_queryLimit || !sv_queryLimit->current.enabled)
        return false;

    if (category < 0 || category >= SV_RATELIMIT_BUCKET_COUNT)
    {
        iassert(category >= 0 && category < SV_RATELIMIT_BUCKET_COUNT);
        return false;
    }

    // A bot address is the server talking to itself.
    if (from.type != NA_IP || Sys_IsLANAddress(from))
        return false;

    if (periodMsec < 1)
        periodMsec = 1;

    SvLeakyBucket *bucket = SV_RateLimitEntryFor(&from, burst * periodMsec);

    if (!bucket)
        return true;

    return SV_RateLimitDrain(&bucket->lastTime[category], &bucket->burst[category], burst, periodMsec);
}

bool __cdecl SV_RateLimitGlobal(SvRateLimitBucket category, int32_t burst, int32_t periodMsec)
{
    if (!sv_queryLimit || !sv_queryLimit->current.enabled)
        return false;

    if (category < 0 || category >= SV_RATELIMIT_BUCKET_COUNT)
    {
        iassert(category >= 0 && category < SV_RATELIMIT_BUCKET_COUNT);
        return false;
    }

    if (periodMsec < 1)
        periodMsec = 1;

    return SV_RateLimitDrain(&g_svRateGlobalTime[category], &g_svRateGlobalBurst[category], burst, periodMsec);
}
