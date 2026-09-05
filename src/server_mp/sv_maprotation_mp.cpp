// Portions of this file are derived from CoD4x_Server
//   https://github.com/callofduty4x/CoD4x_Server
//   Copyright (C) 2010-2013 Ninja and TheKelm
//   Copyright (C) the CoD4x authors
// Licensed under the GNU Affero General Public License v3 (see LICENSE.AGPLv3).
//
// Adapted to KisakCOD's engine APIs. The combined work is conveyed under
// GPLv3 section 13; see LICENSING.md.

#ifndef KISAK_MP
#error This File is MultiPlayer Only
#endif

#include "sv_maprotation_mp.h"
#include "server_mp.h"

#include <qcommon/qcommon.h>

#include <stdlib.h>
#include <string.h>

// A rotation is "gametype dm map mp_crash gametype war map mp_backlot ...".
// Long ones are the point of this feature, so the working buffer is generous;
// a twenty map rotation is around 600 characters.
#define SV_ROTATION_BUFFER 8192

// Entries kept for the shuffle. CoD4x sizes its array at CVAR_STRING_SIZE / 8
// from the shortest possible entry, then fills it without ever checking the
// index against that bound, so a long enough sv_mapRotation walks off the end
// of its stack frame. Bounded here, and a rotation longer than this keeps its
// first SV_ROTATION_MAX_ENTRIES entries rather than corrupting anything.
#define SV_ROTATION_MAX_ENTRIES 256

struct SvRotationEntry
{
    char gametype[64];
    char mapname[MAX_QPATH];
};

static const dvar_t *sv_randomMapRotation;

void __cdecl SV_MapRotation_Init()
{
    sv_randomMapRotation = Dvar_RegisterBool(
        "sv_randomMapRotation", false, DVAR_ARCHIVE,
        "Shuffle sv_mapRotation each time the rotation is refilled");
}

// Self-contained so the shared Com_Parse state is untouched: SV_MapRotate_f is
// itself mid-parse of sv_mapRotationCurrent when it asks for a refill, and
// CoD4x's strtok would be no better, since it keeps static state of its own.
static bool SV_Rotation_NextToken(const char **data, char *token, uint32_t size)
{
    const char *in = *data;
    uint32_t out = 0;

    while (*in && (uint8_t)*in <= ' ')
        ++in;

    if (!*in)
    {
        *data = in;
        return false;
    }

    while (*in && (uint8_t)*in > ' ')
    {
        if (out < size - 1)
            token[out++] = *in;
        ++in;
    }

    token[out] = 0;
    *data = in;
    return true;
}

// Splits the rotation into gametype/map pairs. A map that carries no gametype
// of its own inherits the one most recently named, which is how the rotation
// reads when it is played in order.
static uint32_t SV_Rotation_Parse(const char *rotation, SvRotationEntry *entries, uint32_t maxEntries)
{
    char token[MAX_QPATH];
    char gametype[64];
    uint32_t count = 0;

    I_strncpyz(gametype, sv_gametype->current.string, sizeof(gametype));

    while (SV_Rotation_NextToken(&rotation, token, sizeof(token)))
    {
        if (!I_stricmp(token, "gametype"))
        {
            if (!SV_Rotation_NextToken(&rotation, token, sizeof(token)))
                break;

            I_strncpyz(gametype, token, sizeof(gametype));
            continue;
        }

        if (!I_stricmp(token, "map"))
        {
            if (!SV_Rotation_NextToken(&rotation, token, sizeof(token)))
                break;

            if (count == maxEntries)
            {
                Com_PrintWarning(15, "sv_mapRotation has more than %i entries; the rest are not shuffled\n", maxEntries);
                break;
            }

            I_strncpyz(entries[count].gametype, gametype, sizeof(entries[count].gametype));
            I_strncpyz(entries[count].mapname, token, sizeof(entries[count].mapname));
            ++count;
            continue;
        }

        // Anything else is a keyword this build does not know. Skipping it
        // matches what SV_MapRotate_f does when it meets one in order.
    }

    return count;
}

void __cdecl SV_MapRotation_Refill()
{
    if (!sv_randomMapRotation || !sv_randomMapRotation->current.enabled)
    {
        Dvar_SetString((dvar_s *)sv_mapRotationCurrent, (char *)sv_mapRotation->current.string);
        return;
    }

    // 256 entries is 80KB, too much for this stack frame, and this runs once
    // per rotation cycle rather than per frame.
    static SvRotationEntry entries[SV_ROTATION_MAX_ENTRIES];

    const uint32_t count = SV_Rotation_Parse(sv_mapRotation->current.string, entries, ARRAY_COUNT(entries));

    if (count < 2)
    {
        // Nothing to shuffle. Fall back rather than write an empty rotation,
        // which SV_MapRotate_f would answer with a map_restart.
        Dvar_SetString((dvar_s *)sv_mapRotationCurrent, (char *)sv_mapRotation->current.string);
        return;
    }

    // Fisher-Yates. rand() is what the rest of this engine uses for anything
    // not security relevant, and a map order is exactly that.
    for (uint32_t i = count - 1; i > 0; --i)
    {
        const uint32_t j = (uint32_t)rand() % (i + 1);

        if (j == i)
            continue;

        const SvRotationEntry tmp = entries[i];
        entries[i] = entries[j];
        entries[j] = tmp;
    }

    // Every entry is written with its own gametype. Emitting one only when it
    // changes would be shorter, but the shuffle has just made "the gametype
    // before this map" mean something different than it did in the source
    // rotation, and being explicit is what keeps that correct.
    char out[SV_ROTATION_BUFFER];
    uint32_t len = 0;

    for (uint32_t i = 0; i < count; ++i)
    {
        const char *piece = va("gametype %s map %s ", entries[i].gametype, entries[i].mapname);
        const uint32_t pieceLen = (uint32_t)strlen(piece);

        if (len + pieceLen >= sizeof(out))
        {
            Com_PrintWarning(15, "sv_mapRotation is too long to shuffle in full; %i of %i entries kept\n", i, count);
            break;
        }

        memcpy(out + len, piece, pieceLen);
        len += pieceLen;
    }

    out[len] = 0;

    Dvar_SetString((dvar_s *)sv_mapRotationCurrent, out);
}
