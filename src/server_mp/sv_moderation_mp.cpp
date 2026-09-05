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

#include <universal/q_shared.h>
#include "sv_moderation_mp.h"
#include "server_mp.h"

#include <qcommon/cmd.h>
#include <qcommon/qcommon.h>

#include <stdlib.h>

static cmd_function_s SV_MuteClient_f_VAR;
static cmd_function_s SV_MuteClient_f_VAR_SERVER;
static cmd_function_s SV_MuteUser_f_VAR;
static cmd_function_s SV_MuteUser_f_VAR_SERVER;
static cmd_function_s SV_UnmuteClient_f_VAR;
static cmd_function_s SV_UnmuteClient_f_VAR_SERVER;
static cmd_function_s SV_UnmuteUser_f_VAR;
static cmd_function_s SV_UnmuteUser_f_VAR_SERVER;
static cmd_function_s SV_MuteStatus_f_VAR;
static cmd_function_s SV_MuteStatus_f_VAR_SERVER;

static void SV_ApplyMute(client_t *cl, int32_t level)
{
    iassert(cl);
    iassert(level >= MUTELEVEL_NONE && level <= MUTELEVEL_ALL);

    char cleanName[64];
    I_strncpyz(cleanName, cl->name, sizeof(cleanName));
    I_CleanStr(cleanName);

    cl->mutelevel = level;

    switch (level)
    {
    case MUTELEVEL_NONE:
        Com_Printf(CON_CHANNEL_SERVER, "%s (client %i) is no longer muted\n", cleanName, (int32_t)(cl - svs.clients));
        SV_SendServerCommand(cl, SV_CMD_RELIABLE, "%c \"You are no longer muted\"", 101);
        break;

    case MUTELEVEL_VOICE:
        Com_Printf(CON_CHANNEL_SERVER, "%s (client %i) muted: voice\n", cleanName, (int32_t)(cl - svs.clients));
        SV_SendServerCommand(cl, SV_CMD_RELIABLE, "%c \"You have been muted on voice chat\"", 101);
        break;

    default:
        Com_Printf(CON_CHANNEL_SERVER, "%s (client %i) muted: voice and text\n", cleanName, (int32_t)(cl - svs.clients));
        SV_SendServerCommand(cl, SV_CMD_RELIABLE, "%c \"You have been muted\"", 101);
        break;
    }
}

// Optional trailing level argument, defaulting to a full mute. Anything outside
// 1..2 is refused rather than clamped, so a typo cannot silently do less than
// the admin asked for.
static bool SV_ParseMuteLevel(int32_t argIndex, int32_t *outLevel)
{
    *outLevel = MUTELEVEL_ALL;

    if (SV_Cmd_Argc() <= argIndex)
        return true;

    const char *arg = SV_Cmd_Argv(argIndex);
    const int32_t level = atoi(arg);

    if (level < MUTELEVEL_VOICE || level > MUTELEVEL_ALL)
    {
        Com_Printf(CON_CHANNEL_SERVER,
                   "Invalid mute level '%s'. Use %i for voice only or %i for voice and text\n",
                   arg, MUTELEVEL_VOICE, MUTELEVEL_ALL);
        return false;
    }

    *outLevel = level;
    return true;
}

static client_t *SV_MuteTargetByNum()
{
    if (SV_Cmd_Argc() < 2)
    {
        Com_Printf(CON_CHANNEL_SERVER, "No client number specified.\n");
        return nullptr;
    }

    const char *arg = SV_Cmd_Argv(1);
    const int32_t clientNum = atoi(arg);

    // atoi yields 0 for a non-numeric argument, which would silently target
    // client 0, so require the text to actually read as that number.
    if ((clientNum == 0 && I_stricmp(arg, "0")) || clientNum < 0 || clientNum >= sv_maxclients->current.integer)
    {
        Com_Printf(CON_CHANNEL_SERVER, "Bad client number '%s'\n", arg);
        return nullptr;
    }

    client_t *cl = &svs.clients[clientNum];
    if (!cl->header.state)
    {
        Com_Printf(CON_CHANNEL_SERVER, "Client %i is not active\n", clientNum);
        return nullptr;
    }

    return cl;
}

void __cdecl SV_MuteClient_f()
{
    if (!com_sv_running->current.enabled)
    {
        Com_Printf(CON_CHANNEL_SERVER, "Server is not running.\n");
        return;
    }

    client_t *cl = SV_MuteTargetByNum();
    if (!cl)
    {
        Com_Printf(CON_CHANNEL_SERVER, "Usage: muteClient <client number> [1 = voice, 2 = voice and text]\n");
        return;
    }

    int32_t level;
    if (!SV_ParseMuteLevel(2, &level))
        return;

    SV_ApplyMute(cl, level);
}

void __cdecl SV_MuteUser_f()
{
    if (!com_sv_running->current.enabled)
    {
        Com_Printf(CON_CHANNEL_SERVER, "Server is not running.\n");
        return;
    }

    if (SV_Cmd_Argc() < 2)
    {
        Com_Printf(CON_CHANNEL_SERVER, "Usage: muteUser <player name> [1 = voice, 2 = voice and text]\n");
        return;
    }

    // SV_GetPlayerByName reads argv(1) itself and reports its own failure.
    client_t *cl = SV_GetPlayerByName();
    if (!cl)
        return;

    int32_t level;
    if (!SV_ParseMuteLevel(2, &level))
        return;

    SV_ApplyMute(cl, level);
}

void __cdecl SV_UnmuteClient_f()
{
    if (!com_sv_running->current.enabled)
    {
        Com_Printf(CON_CHANNEL_SERVER, "Server is not running.\n");
        return;
    }

    client_t *cl = SV_MuteTargetByNum();
    if (!cl)
    {
        Com_Printf(CON_CHANNEL_SERVER, "Usage: unmuteClient <client number>\n");
        return;
    }

    SV_ApplyMute(cl, MUTELEVEL_NONE);
}

void __cdecl SV_UnmuteUser_f()
{
    if (!com_sv_running->current.enabled)
    {
        Com_Printf(CON_CHANNEL_SERVER, "Server is not running.\n");
        return;
    }

    if (SV_Cmd_Argc() < 2)
    {
        Com_Printf(CON_CHANNEL_SERVER, "Usage: unmuteUser <player name>\n");
        return;
    }

    client_t *cl = SV_GetPlayerByName();
    if (!cl)
        return;

    SV_ApplyMute(cl, MUTELEVEL_NONE);
}

void __cdecl SV_MuteStatus_f()
{
    if (!com_sv_running->current.enabled)
    {
        Com_Printf(CON_CHANNEL_SERVER, "Server is not running.\n");
        return;
    }

    int32_t muted = 0;
    char cleanName[64];

    for (int32_t i = 0; i < sv_maxclients->current.integer; ++i)
    {
        client_t *cl = &svs.clients[i];
        if (!cl->header.state || cl->mutelevel == MUTELEVEL_NONE)
            continue;

        if (!muted)
            Com_Printf(CON_CHANNEL_SERVER, "num  level  name\n---  -----  ----\n");

        I_strncpyz(cleanName, cl->name, sizeof(cleanName));
        I_CleanStr(cleanName);
        Com_Printf(CON_CHANNEL_SERVER, "%3i  %-5s  %s\n",
                   i, cl->mutelevel == MUTELEVEL_VOICE ? "voice" : "all", cleanName);
        ++muted;
    }

    if (!muted)
        Com_Printf(CON_CHANNEL_SERVER, "No clients are muted.\n");
}

static const dvar_t *sv_disablechat;

bool __cdecl SV_ChatDisabled()
{
    return sv_disablechat && sv_disablechat->current.enabled;
}

void __cdecl SV_AddModerationCommands()
{
    sv_disablechat = Dvar_RegisterBool(
        "sv_disablechat", false, DVAR_ARCHIVE,
        "Refuse chat messages from all clients");

    // Mirrors the kick/ban pairs in SV_AddOperatorCommands: the console entry
    // routes through the server text buffer, and the server entry does the work.
    Cmd_AddCommandInternal("muteClient", Cbuf_AddServerText_f, &SV_MuteClient_f_VAR);
    Cmd_AddServerCommandInternal("muteClient", SV_MuteClient_f, &SV_MuteClient_f_VAR_SERVER);
    Cmd_AddCommandInternal("muteUser", Cbuf_AddServerText_f, &SV_MuteUser_f_VAR);
    Cmd_AddServerCommandInternal("muteUser", SV_MuteUser_f, &SV_MuteUser_f_VAR_SERVER);
    Cmd_AddCommandInternal("unmuteClient", Cbuf_AddServerText_f, &SV_UnmuteClient_f_VAR);
    Cmd_AddServerCommandInternal("unmuteClient", SV_UnmuteClient_f, &SV_UnmuteClient_f_VAR_SERVER);
    Cmd_AddCommandInternal("unmuteUser", Cbuf_AddServerText_f, &SV_UnmuteUser_f_VAR);
    Cmd_AddServerCommandInternal("unmuteUser", SV_UnmuteUser_f, &SV_UnmuteUser_f_VAR_SERVER);
    Cmd_AddCommandInternal("mutestatus", Cbuf_AddServerText_f, &SV_MuteStatus_f_VAR);
    Cmd_AddServerCommandInternal("mutestatus", SV_MuteStatus_f, &SV_MuteStatus_f_VAR_SERVER);
}
