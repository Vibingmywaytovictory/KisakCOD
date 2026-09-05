// Portions of this file are derived from CoD4x_Server
//   https://github.com/callofduty4x/CoD4x_Server
//   Copyright (C) the CoD4x authors
// Licensed under the GNU Affero General Public License v3 (see LICENSE.AGPLv3).
//
// Adapted to KisakCOD's engine APIs. The combined work is conveyed under
// GPLv3 section 13; see LICENSING.md.

#pragma once

#include <universal/q_shared.h>
#include <bgame/bg_local.h>
#include <script/scr_variable.h>

struct gentity_s;
struct usercmd_s;

// Desired input state for a single bot client. GSC writes it through the bot*
// entity methods below; SV_BotUserMove drains it once per server frame when it
// builds that bot's usercmd.
//
// CoD4x marks its equivalent __attribute__((aligned(4))) with a comment about
// GCC otherwise mangling the field sizes. That is a GCC-specific workaround and
// is deliberately not carried over -- MSVC lays this out naturally, and nothing
// serialises the struct, so its layout is not load-bearing.
struct BotMovementInfo_t
{
    int32_t buttons;        // BUTTON_* mask, applied verbatim to the usercmd
    int32_t doMove;         // non-zero while moveTo is still being pursued
    float   moveTo[2];      // world-space XY goal
    int32_t rotIterCount;   // server frames remaining in the current turn
    int16_t rotFrac[2];     // per-frame pitch/yaw delta, in usercmd angle units
    bool    lastAliveState; // alive state as of the previous frame
    uint8_t useSpamDelay;   // frames until "+activate" spam resumes while dead
    uint8_t weapon;         // weapon index written into the usercmd
    bool    scripted;       // a bot* method has driven this slot at least once
};

extern BotMovementInfo_t g_botai[MAX_CLIENTS];

// True once any bot* method has touched this slot. Until then SV_BotUserMove
// keeps the stock random-walk behaviour, so plain addtestclient() is unchanged.
bool __cdecl SV_BotIsScripted(int32_t clientNum);

// Translate the slot's desired input into this frame's usercmd.
void __cdecl SV_BotApplyScriptedInput(
    gentity_s *bot,
    int32_t clientNum,
    const int32_t *prevAngles,
    usercmd_s *cmd);

void __cdecl SV_BotClearMovementInfo(int32_t clientNum);
bool __cdecl SV_BotShouldSpamUseButton(gentity_s *bot);

// Registered as entity methods in methods_2[] (game_mp/g_scr_main_mp.cpp).
void __cdecl GScr_BotMoveTo(scr_entref_t entref);
void __cdecl GScr_BotLookAt(scr_entref_t entref);
void __cdecl GScr_BotStop(scr_entref_t entref);
void __cdecl GScr_BotAction(scr_entref_t entref);
void __cdecl GScr_BotLookAtPlayer(scr_entref_t entref);
void __cdecl GScr_BotWeapon(scr_entref_t entref);
