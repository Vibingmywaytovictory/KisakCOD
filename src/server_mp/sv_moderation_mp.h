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

// Server-side mute, on top of the client_t::mutelevel field taken from CoD4x.
//
// CoD4 ships one moderation verb -- kick -- so the answer to a player who will
// not stop is to remove them. A mute is the proportionate response and the game
// has never had one. CoD4x carries the mutelevel field and honours it in its
// voice and chat paths, but sets it only from plugins and its admin system; the
// commands below are this engine's own, so that the field is reachable from
// rcon and the server console without pulling in that infrastructure.
//
// Levels match CoD4x so that a plugin written against it would agree:
//   0 - unrestricted
//   1 - voice blocked
//   2 - voice and text chat blocked
#define MUTELEVEL_NONE   0
#define MUTELEVEL_VOICE  1
#define MUTELEVEL_ALL    2

void __cdecl SV_MuteClient_f();     // muteClient <clientnum> [level]
void __cdecl SV_MuteUser_f();       // muteUser <name> [level]
void __cdecl SV_UnmuteClient_f();   // unmuteClient <clientnum>
void __cdecl SV_UnmuteUser_f();     // unmuteUser <name>
void __cdecl SV_MuteStatus_f();     // mutestatus

// Registered from SV_AddOperatorCommands.
void __cdecl SV_AddModerationCommands();
