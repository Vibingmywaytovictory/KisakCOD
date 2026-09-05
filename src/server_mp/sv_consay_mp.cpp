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

#include "sv_consay_mp.h"
#include "server_mp.h"

#include <qcommon/qcommon.h>
#include <qcommon/cmd.h>

#include <stdlib.h>
#include <string.h>

// Server command characters, as this tree's client dispatches them in
// CG_DeployServerCommand:
//   'h' chat feed, suppressed for a player running cg_teamChatsOnly
//   'e' game message feed
//   'c' on-screen announcement, drawn bold
#define SV_CONSAY_CMD_CHAT     'h'
#define SV_CONSAY_CMD_MESSAGE  'e'
#define SV_CONSAY_CMD_SCREEN   'c'

static const dvar_t *sv_consayName;
static const dvar_t *sv_contellName;

void __cdecl SV_ConSay_Assemble(int32_t firstArg, const char *prefix, char *out, uint32_t outSize)
{
    I_strncpyz(out, prefix, outSize);

    uint32_t len = (uint32_t)strlen(out);

    for (int32_t i = firstArg; i < SV_Cmd_Argc(); ++i)
    {
        const char *arg = SV_Cmd_Argv(i);

        if (i != firstArg && len < outSize - 1)
            out[len++] = ' ';

        for (; *arg && len < outSize - 1; ++arg)
            out[len++] = *arg;
    }

    out[len] = 0;

    // A single quoted argument arrives with its quotes already stripped by the
    // tokenizer, but `say "hello"` typed with the quotes doubled up still gets
    // here wrapped. The stock assembler removed one layer; keep that.
    char *text = out + strlen(prefix);

    if (text[0] == '"')
    {
        const uint32_t textLen = (uint32_t)strlen(text);

        if (textLen >= 2 && text[textLen - 1] == '"')
        {
            memmove(text, text + 1, textLen - 2);
            text[textLen - 2] = 0;
        }
    }
}

client_t *__cdecl SV_ConSay_ResolveTarget()
{
    if (SV_Cmd_Argc() < 2)
    {
        Com_Printf(0, "No player specified.\n");
        return nullptr;
    }

    const char *arg = SV_Cmd_Argv(1);

    // atoi yields 0 for a name, which would silently target client 0 -- the
    // same trap the mute commands guard against. Only treat this as a number
    // when every character is a digit.
    bool numeric = arg[0] != 0;

    for (const char *c = arg; *c; ++c)
    {
        if (*c < '0' || *c > '9')
        {
            numeric = false;
            break;
        }
    }

    if (numeric)
    {
        const int32_t clientNum = atoi(arg);

        if (clientNum < 0 || clientNum >= sv_maxclients->current.integer)
        {
            Com_Printf(0, "Bad client slot: %i\n", clientNum);
            return nullptr;
        }

        if (svs.clients[clientNum].header.state != CS_ACTIVE)
        {
            Com_Printf(0, "Client %i is not active\n", clientNum);
            return nullptr;
        }

        return &svs.clients[clientNum];
    }

    // Prints its own "Player %s is not on the server" on a miss.
    return SV_GetPlayerByName();
}

static void SV_ConSay_Broadcast(char commandChar, const char *prefix)
{
    char text[1028];

    if (!com_sv_running->current.enabled)
    {
        Com_Printf(0, "Server is not running.\n");
        return;
    }

    if (SV_Cmd_Argc() < 2)
    {
        Com_Printf(0, "Usage: %s <text>\n", SV_Cmd_Argv(0));
        return;
    }

    SV_ConSay_Assemble(1, prefix, text, sizeof(text));
    SV_SendServerCommand(nullptr, SV_CMD_CAN_IGNORE, "%c \"%s\"", commandChar, text);
    Com_Printf(15, "%s\n", text);
}

static void SV_ConSay_ToOne(char commandChar, const char *prefix)
{
    char text[1028];

    if (!com_sv_running->current.enabled)
    {
        Com_Printf(0, "Server is not running.\n");
        return;
    }

    if (SV_Cmd_Argc() < 3)
    {
        Com_Printf(0, "Usage: %s <client number or name> <text>\n", SV_Cmd_Argv(0));
        return;
    }

    client_t *cl = SV_ConSay_ResolveTarget();

    if (!cl)
        return;

    SV_ConSay_Assemble(2, prefix, text, sizeof(text));
    SV_SendServerCommand(cl, SV_CMD_CAN_IGNORE, "%c \"%s\"", commandChar, text);
    Com_Printf(15, "to %s: %s\n", cl->name, text);
}

static void __cdecl SV_ConSayMessage_f()
{
    SV_ConSay_Broadcast(SV_CONSAY_CMD_MESSAGE, sv_consayName->current.string);
}

static void __cdecl SV_ConSayScreen_f()
{
    SV_ConSay_Broadcast(SV_CONSAY_CMD_SCREEN, sv_consayName->current.string);
}

static void __cdecl SV_ConTellMessage_f()
{
    SV_ConSay_ToOne(SV_CONSAY_CMD_MESSAGE, sv_contellName->current.string);
}

static void __cdecl SV_ConTellScreen_f()
{
    SV_ConSay_ToOne(SV_CONSAY_CMD_SCREEN, sv_contellName->current.string);
}

static cmd_function_s SV_ConSayMessage_f_VAR;
static cmd_function_s SV_ConSayMessage_f_VAR_SERVER;
static cmd_function_s SV_ConSayScreen_f_VAR;
static cmd_function_s SV_ConSayScreen_f_VAR_SERVER;
static cmd_function_s SV_ConTellMessage_f_VAR;
static cmd_function_s SV_ConTellMessage_f_VAR_SERVER;
static cmd_function_s SV_ConTellScreen_f_VAR;
static cmd_function_s SV_ConTellScreen_f_VAR_SERVER;

void __cdecl SV_ConSay_AddCommands()
{
    sv_consayName = Dvar_RegisterString(
        "sv_consayName", "console: ", DVAR_ARCHIVE,
        "Prefix put in front of a message sent with say, consay or screensay");

    sv_contellName = Dvar_RegisterString(
        "sv_contellName", "console: ", DVAR_ARCHIVE,
        "Prefix put in front of a message sent with tell, contell or screentell");

    Cmd_AddCommandInternal("consay", Cbuf_AddServerText_f, &SV_ConSayMessage_f_VAR);
    Cmd_AddServerCommandInternal("consay", SV_ConSayMessage_f, &SV_ConSayMessage_f_VAR_SERVER);

    Cmd_AddCommandInternal("screensay", Cbuf_AddServerText_f, &SV_ConSayScreen_f_VAR);
    Cmd_AddServerCommandInternal("screensay", SV_ConSayScreen_f, &SV_ConSayScreen_f_VAR_SERVER);

    Cmd_AddCommandInternal("contell", Cbuf_AddServerText_f, &SV_ConTellMessage_f_VAR);
    Cmd_AddServerCommandInternal("contell", SV_ConTellMessage_f, &SV_ConTellMessage_f_VAR_SERVER);

    Cmd_AddCommandInternal("screentell", Cbuf_AddServerText_f, &SV_ConTellScreen_f_VAR);
    Cmd_AddServerCommandInternal("screentell", SV_ConTellScreen_f, &SV_ConTellScreen_f_VAR_SERVER);
}
