// Portions of this file are derived from CoD4x_Server
//   https://github.com/callofduty4x/CoD4x_Server
//   Copyright (C) 2010-2013 Ninja and TheKelm
//   Copyright (C) 1999-2005 Id Software, Inc.
//   Copyright (C) the CoD4x authors
// Licensed under the GNU Affero General Public License v3 (see LICENSE.AGPLv3).
//
// Adapted to KisakCOD's engine APIs. The combined work is conveyed under
// GPLv3 section 13; see LICENSING.md.

#ifndef KISAK_MP
#error This File is MultiPlayer Only
#endif

#include <universal/q_shared.h>
#include "g_vote_mp.h"
#include "g_public_mp.h"
#include "g_main_mp.h"

#include <qcommon/qcommon.h>
#include <server/sv_game.h>
#include <server_mp/server_mp.h>

#include <string.h>

static const dvar_t *g_voteTime;
static const dvar_t *g_voteMaxVotes;
static const dvar_t *g_voteKickMinPlayers;
static const dvar_t *g_voteVoteGametypes;
static const dvar_t *g_voteAllowRestart;
static const dvar_t *g_voteAllowMaprotate;
static const dvar_t *g_voteAllowKick;
static const dvar_t *g_voteAllowGametype;
static const dvar_t *g_voteAllowMap;

void __cdecl G_InitVoteRestrictions()
{
    // Stock behaviour is a hardcoded 30 second vote; keep that as the default.
    g_voteTime = Dvar_RegisterInt(
        "g_voteTime", 30, 10, 90, DVAR_ARCHIVE,
        "Seconds a called vote stays open");

    // Stock behaviour is a hardcoded limit of three votes per player per map.
    g_voteMaxVotes = Dvar_RegisterInt(
        "g_voteMaxVotes", 3, 1, 10, DVAR_ARCHIVE,
        "How many votes one player may call per map");

    // 0 keeps stock behaviour, which imposes no kick-specific minimum beyond
    // the two connected clients callvote already requires.
    g_voteKickMinPlayers = Dvar_RegisterInt(
        "g_voteKickMinPlayers", 0, 0, MAX_CLIENTS, DVAR_ARCHIVE,
        "Non-spectating players required before a kick vote may be called");

    g_voteVoteGametypes = Dvar_RegisterString(
        "g_voteVoteGametypes", "", DVAR_ARCHIVE,
        "Space separated list of gametypes that may be voted for. Empty means all");

    g_voteAllowRestart = Dvar_RegisterBool(
        "g_voteAllowRestart", true, DVAR_ARCHIVE, "Allow calling map_restart votes");
    g_voteAllowMaprotate = Dvar_RegisterBool(
        "g_voteAllowMaprotate", true, DVAR_ARCHIVE, "Allow calling map_rotate votes");
    g_voteAllowKick = Dvar_RegisterBool(
        "g_voteAllowKick", true, DVAR_ARCHIVE, "Allow calling kick and tempban votes");
    g_voteAllowGametype = Dvar_RegisterBool(
        "g_voteAllowGametype", true, DVAR_ARCHIVE, "Allow calling gametype votes");

    // 2 is the stock behaviour: any map that exists may be voted for. CoD4x
    // defaults this to 1, which would quietly restrict an upgraded server to
    // its rotation, so the tightening is left for the admin to opt into.
    g_voteAllowMap = Dvar_RegisterInt(
        "g_voteAllowMap", 2, 0, 2, DVAR_ARCHIVE,
        "Allow calling map votes - 0 = no, 1 = only maps in sv_mapRotation, 2 = any map");
}

static void G_VoteRefuse(gentity_s *ent, const char *fmt, ...)
{
    va_list argptr;
    char reason[256];

    va_start(argptr, fmt);
    _vsnprintf(reason, sizeof(reason), fmt, argptr);
    va_end(argptr);
    reason[sizeof(reason) - 1] = 0;   // _vsnprintf does not terminate on truncation

    SV_GameSendServerCommand(ent - g_entities, SV_CMD_CAN_IGNORE, va("%c \"%s\"", 101, reason));
}

// Whitespace-delimited token walk. Com_Parse would do this, but it keeps parse
// state on a shared stack and this runs from inside command handling, so a
// self-contained reader is the safer of the two.
static const char *G_VoteNextToken(const char **data, char *out, uint32_t outSize)
{
    const char *p = *data;

    while (*p && (unsigned char)*p <= ' ')
        ++p;

    if (!*p)
    {
        *data = p;
        return nullptr;
    }

    uint32_t n = 0;
    while (*p && (unsigned char)*p > ' ')
    {
        if (n + 1 < outSize)
            out[n++] = *p;
        ++p;
    }

    out[n] = 0;
    *data = p;
    return out;
}

// CoD4x tests this with strstr(rotation, "map <name>"), which matches a prefix:
// with mp_crash_snow in the rotation, a vote for mp_crash passes. Walk the
// rotation as tokens and compare whole names instead.
static bool G_VoteMapIsInRotation(const char *mapname)
{
    if (!sv_mapRotation || !sv_mapRotation->current.string)
        return false;

    const char *data = sv_mapRotation->current.string;
    char token[MAX_QPATH];
    bool nextTokenIsMap = false;

    while (G_VoteNextToken(&data, token, sizeof(token)))
    {
        if (nextTokenIsMap)
        {
            if (!I_stricmp(token, mapname))
                return true;
            nextTokenIsMap = false;
            continue;
        }

        if (!I_stricmp(token, "map"))
            nextTokenIsMap = true;
    }

    return false;
}

bool __cdecl G_VoteAllowedType(gentity_s *ent, const char *voteType)
{
    iassert(ent);
    iassert(voteType);

    bool allowed;

    if (!I_stricmp(voteType, "map_restart"))
    {
        allowed = g_voteAllowRestart->current.enabled;
    }
    else if (!I_stricmp(voteType, "map_rotate"))
    {
        allowed = g_voteAllowMaprotate->current.enabled;
    }
    else if (!I_stricmp(voteType, "map"))
    {
        allowed = g_voteAllowMap->current.integer != 0;
    }
    else if (!I_stricmp(voteType, "typemap"))
    {
        // Changes both, so it needs both permissions.
        allowed = g_voteAllowMap->current.integer != 0 && g_voteAllowGametype->current.enabled;
    }
    else if (!I_stricmp(voteType, "g_gametype"))
    {
        allowed = g_voteAllowGametype->current.enabled;
    }
    else if (!I_stricmp(voteType, "kick")
          || !I_stricmp(voteType, "clientkick")
          || !I_stricmp(voteType, "tempBanUser")
          || !I_stricmp(voteType, "tempBanClient"))
    {
        allowed = g_voteAllowKick->current.enabled;
    }
    else
    {
        // Not a type this knows about. Under script voting the gametype may
        // define its own, and under engine voting the caller rejects it a few
        // lines later with GAME_INVALIDVOTESTRING.
        return true;
    }

    if (!allowed)
    {
        G_VoteRefuse(ent, "Voting for %s is disabled on this server", voteType);
        return false;
    }

    return true;
}

bool __cdecl G_VoteAllowedGametype(gentity_s *ent, const char *gametype)
{
    iassert(ent);
    iassert(gametype);

    if (!g_voteVoteGametypes->current.string || !g_voteVoteGametypes->current.string[0])
        return true;   // no whitelist configured

    const char *data = g_voteVoteGametypes->current.string;
    char token[64];

    // CoD4x uses strstr here too, so listing "dm" also permits "dm2". Match
    // whole entries.
    while (G_VoteNextToken(&data, token, sizeof(token)))
    {
        if (!I_stricmp(token, gametype))
            return true;
    }

    G_VoteRefuse(ent, "Voting for gametype %s is disabled on this server", gametype);
    return false;
}

bool __cdecl G_VoteAllowedMap(gentity_s *ent, const char *mapname)
{
    iassert(ent);
    iassert(mapname);

    if (g_voteAllowMap->current.integer != 1)
        return true;   // 0 was refused by G_VoteAllowedType; 2 permits any map

    if (G_VoteMapIsInRotation(mapname))
        return true;

    G_VoteRefuse(ent, "This server only allows voting for maps in its rotation");
    return false;
}

bool __cdecl G_VoteAllowedKickPlayerCount(gentity_s *ent)
{
    iassert(ent);

    if (g_voteKickMinPlayers->current.integer <= 0)
        return true;

    int32_t activePlayers = 0;
    for (int32_t i = 0; i < level.maxclients; ++i)
    {
        if (level.clients[i].sess.connected != CON_CONNECTED)
            continue;
        if (level.clients[i].sess.cs.team == TEAM_SPECTATOR)
            continue;
        ++activePlayers;
    }

    if (activePlayers >= g_voteKickMinPlayers->current.integer)
        return true;

    G_VoteRefuse(ent, "GAME_VOTINGNOTENOUGHPLAYERS");
    return false;
}

int32_t __cdecl G_VoteMaxVotes()
{
    return g_voteMaxVotes->current.integer;
}

int32_t __cdecl G_VoteDurationMsec()
{
    return 1000 * g_voteTime->current.integer;
}
