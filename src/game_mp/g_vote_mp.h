// Portions of this file are derived from CoD4x_Server
//   https://github.com/callofduty4x/CoD4x_Server
//   Copyright (C) 2010-2013 Ninja and TheKelm
//   Copyright (C) 1999-2005 Id Software, Inc.
//   Copyright (C) the CoD4x authors
// Licensed under the GNU Affero General Public License v3 (see LICENSE.AGPLv3).
//
// Adapted to KisakCOD's engine APIs. The combined work is conveyed under
// GPLv3 section 13; see LICENSING.md.

#pragma once

#include <universal/q_shared.h>

struct gentity_s;

// Server-side policy for callvote, ported from CoD4x's g_sv_cmds.c.
//
// Stock CoD4 gives a server owner one switch -- g_allowVote -- and hardcodes
// everything else: 30 second votes, three votes per player per map, and any
// vote type available to anyone who is not a spectator. Vote-kick abuse is the
// usual consequence. These add per-type switches, a gametype whitelist, an
// optional map-rotation restriction and a minimum player count for kick votes.
//
// Every default here reproduces stock KisakCOD behaviour exactly, so adding
// this file changes nothing until an admin sets one of the dvars. That is a
// deliberate departure from CoD4x, whose defaults (2 votes, rotation-only maps,
// 5 player kick minimum) would silently retune an existing server on upgrade.
//
// Unlike CoD4x, which snapshots the dvars into a flag word once at init, these
// are read at vote time -- so `rcon set g_voteAllowKick 0` takes effect on the
// next vote rather than at the next map load.
void __cdecl G_InitVoteRestrictions();

// The checks below refuse a vote by telling the caller why and returning false.
// `voteType` is arg1 of the callvote, matched case-insensitively; a type this
// does not recognise is permitted, leaving it to the caller or to the script
// vote handler, which may implement vote types of its own.
bool __cdecl G_VoteAllowedType(gentity_s *ent, const char *voteType);

// Empty g_voteVoteGametypes means every gametype is votable.
bool __cdecl G_VoteAllowedGametype(gentity_s *ent, const char *gametype);

// Only consulted when g_voteAllowMap is 1, where the map must appear in
// sv_mapRotation. g_voteAllowMap 2 (the default) permits any map that exists.
bool __cdecl G_VoteAllowedMap(gentity_s *ent, const char *mapname);

// Kick votes need g_voteKickMinPlayers non-spectating players connected.
bool __cdecl G_VoteAllowedKickPlayerCount(gentity_s *ent);

int32_t __cdecl G_VoteMaxVotes();
int32_t __cdecl G_VoteDurationMsec();
