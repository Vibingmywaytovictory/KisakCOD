// Portions of this file are derived from CoD4x_Server
//   https://github.com/callofduty4x/CoD4x_Server
//   Copyright (C) 2010-2013 Ninja and TheKelm
//   Copyright (C) the CoD4x authors
// Licensed under the GNU Affero General Public License v3 (see LICENSE.AGPLv3).
//
// Adapted to KisakCOD's engine APIs. The combined work is conveyed under
// GPLv3 section 13; see LICENSING.md.

#pragma once

#include <universal/q_shared.h>

struct client_t;

// Server-console messaging, from CoD4x's sv_cmds.c.
//
// Stock CoD4 gives an operator two ways to talk to players -- say and tell --
// and both put the text in the chat feed with a hardcoded "console: " in front
// of it. A player running cg_teamChatsOnly never sees either one, which is
// exactly the player an admin most often needs to reach.
//
// CoD4x adds the same message in two other places: the game message feed and
// the on-screen announcement. Those go out as server commands 'e' and 'c',
// both of which this tree's client already handles (CG_GameMessage and
// CG_BoldGameMessage in cg_servercmds_mp.cpp), so nothing on the wire is new.
//
//   consay      <text>            game message feed, everyone
//   screensay   <text>            on-screen announcement, everyone
//   contell     <player> <text>   game message feed, one player
//   screentell  <player> <text>   on-screen announcement, one player
//
// The prefix is sv_consayName / sv_contellName rather than hardcoded. Both
// default to "console: ", so these read exactly like say does today; CoD4x
// defaults its tell prefix to "^5PM: ^7" instead, which is worth knowing about
// but is not what an existing KisakCOD admin would expect.
void __cdecl SV_ConSay_AddCommands();

// Builds "<prefix><args from firstArg onwards>", stripping one layer of
// surrounding quotes the way the stock assembler did.
//
// Reads through SV_Cmd_Argv, not Cmd_ArgsBuffer. The stock
// SV_AssembleConSayMessage mixed the two: it tested SV_Cmd_Argc but pulled the
// text out of the console tokenizer, which holds something else entirely when
// the command arrives over rcon.
void __cdecl SV_ConSay_Assemble(int32_t firstArg, const char *prefix, char *out, uint32_t outSize);

// Resolves argv(1) as a client number when it reads as one, and as a player
// name otherwise. Prints its own diagnostic and returns null on no match.
client_t *__cdecl SV_ConSay_ResolveTarget();
