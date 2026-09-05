// Portions of this file are derived from CoD4x_Server
//   https://github.com/callofduty4x/CoD4x_Server
//   Copyright (C) the CoD4x authors
// Licensed under the GNU Affero General Public License v3 (see LICENSE.AGPLv3).
//
// Adapted to KisakCOD's engine APIs. The combined work is conveyed under
// GPLv3 section 13; see LICENSING.md.

#include <universal/q_shared.h>
#include "sv_bots_mp.h"
#include "server_mp.h"

#include <universal/com_math.h>
#include <game_mp/g_public_mp.h>
#include <game_mp/g_main_mp.h>
#include <qcommon/msg_mp.h>
#include <script/scr_const.h>
#include <script/scr_vm.h>
#include <game_mp/g_scr_builtins_mp.h>

#include <math.h>

BotMovementInfo_t g_botai[MAX_CLIENTS];

struct BotAction_t
{
    const char *action;
    int32_t     buttons;
};

// CoD4x names bit 19 KEY_MASK_ADS and sets it alongside bit 11 for "ads". Bit 19
// is really the +throw (grenade throwback) button -- client_mp/cl_input.cpp binds
// it from key index 26, and the pairing CoD4x observed comes from the default
// right-mouse bind being +speed_throw, which presses both keys at once. Setting
// both is still the faithful thing to do, because that is what a real client
// sends; nothing in the engine reads bit 19.
static const BotAction_t s_botActions[] =
{
    { "gostand",    BUTTON_JUMP                },
    { "gocrouch",   BUTTON_CROUCH              },
    { "goprone",    BUTTON_PRONE               },
    { "fire",       BUTTON_ATTACK              },
    { "melee",      BUTTON_MELEE               },
    { "frag",       BUTTON_FRAG                },
    { "smoke",      BUTTON_SMOKE               },
    { "reload",     BUTTON_RELOAD              },
    { "sprint",     BUTTON_SPRINT              },
    { "leanleft",   BUTTON_LEAN_LEFT           },
    { "leanright",  BUTTON_LEAN_RIGHT          },
    { "ads",        BUTTON_ADS | BUTTON_THROW  },
    { "holdbreath", BUTTON_BREATH              },
    { "activate",   BUTTON_USE                 },
};

// Resolve an entity method's entref to its bot input slot.
//
// GetPlayerEntity already raises a script error for non-players, which implies a
// client slot -- but g_botai is only MAX_CLIENTS wide while entref.entnum ranges
// up to MAX_GENTITIES. CoD4x indexes g_botai straight off entnum and leans on
// that implication. Check it directly instead, and return null so callers cannot
// write out of bounds even if the error path returns.
static BotMovementInfo_t *SV_BotAiForEntRef(scr_entref_t entref, gentity_s **outEnt)
{
    gentity_s *ent = GetPlayerEntity(entref);

    if (!ent || !ent->client || entref.entnum >= (uint32_t)MAX_CLIENTS)
    {
        Scr_ObjectError("not a client entity");
        return nullptr;
    }

    if (outEnt)
        *outEnt = ent;

    // Any bot* call hands this slot over to script control for good.
    g_botai[entref.entnum].scripted = true;

    return &g_botai[entref.entnum];
}

// Work out the per-frame view delta that turns the bot towards a world position
// over the given duration in seconds.
static void SV_BotCalculateRotationForOrigin(
    gentity_s *bot,
    BotMovementInfo_t *ai,
    const float *target,
    float duration)
{
    float view[3];
    float delta[3];
    float rot[3];
    float botView[3];

    // CoD4x divides by this without a floor. botLookAtPlayer passes 1.0/sv_fps,
    // which reaches 1 in exact arithmetic but is one rounding error away from 0.
    // Clamp rather than rely on that.
    int32_t iterations = (int32_t)(duration * sv_fps->current.integer);
    if (iterations < 1)
        iterations = 1;
    ai->rotIterCount = iterations;

    // Eye position: origin plus a stance-dependent offset. These are the values
    // GSC bullettrace callers use, so bots aim from where scripts expect.
    Vec3Copy(bot->r.currentOrigin, view);
    if (bot->client->ps.pm_flags & PMF_PRONE)
        view[2] = view[2] + 11.0f;
    else if (bot->client->ps.pm_flags & PMF_DUCKED)
        view[2] = view[2] + 40.0f;
    else
        view[2] = view[2] + 60.0f;

    Vec3Sub(target, view, delta);
    vectoangles(delta, rot);
    Vec3Copy(bot->client->ps.viewangles, botView);

    // Pitch and yaw only; roll is always zero for a player.
    for (int32_t i = 0; i < 2; ++i)
    {
        if (botView[i] < 0.0f)
            botView[i] = botView[i] + 360.0f;

        rot[i] = rot[i] - botView[i];

        if (rot[i] > 180.0f)
            rot[i] = rot[i] - 360.0f;
        else if (rot[i] < -180.0f)
            rot[i] = rot[i] + 360.0f;

        ai->rotFrac[i] = (int16_t)((rot[i] / 360.0f * 65535.0f) / iterations);
    }
}

bool __cdecl SV_BotIsScripted(int32_t clientNum)
{
    iassert(clientNum >= 0 && clientNum < MAX_CLIENTS);
    return g_botai[clientNum].scripted;
}

void __cdecl SV_BotApplyScriptedInput(
    gentity_s *bot,
    int32_t clientNum,
    const int32_t *prevAngles,
    usercmd_s *cmd)
{
    iassert(bot);
    iassert(cmd);
    iassert(prevAngles);
    iassert(clientNum >= 0 && clientNum < MAX_CLIENTS);

    BotMovementInfo_t *ai = &g_botai[clientNum];

    cmd->buttons = ai->buttons;

    // Index 0 is "none", which is also what an untouched slot holds, so treat it
    // as "script has not chosen a weapon" and leave the playerstate one alone.
    if (ai->weapon)
        cmd->weapon = ai->weapon;

    if (ai->doMove)
    {
        // forwardmove runs along X, rightmove along Y.
        float move[2];
        move[0] = ai->moveTo[0] - bot->r.currentOrigin[0];
        move[1] = ai->moveTo[1] - bot->r.currentOrigin[1];

        float distance = sqrtf(move[0] * move[0] + move[1] * move[1]);
        ai->doMove = distance > 7.0f ? 1 : 0;

        // Rotate the goal into the bot's own frame.
        float yaw = -bot->r.currentAngles[1] * 0.017453292f; // deg -> rad
        float sy = sinf(yaw);
        float cy = cosf(yaw);
        float rx = move[0] * cy - move[1] * sy;
        float ry = move[0] * sy + move[1] * cy;

        // Scale so the larger component saturates at 127. CoD4x divides by this
        // unguarded, which is a divide by zero once the bot is exactly on target.
        float ax = fabsf(rx);
        float ay = fabsf(ry);
        float maxabs = ax > ay ? ax : ay;
        if (maxabs > 0.0f)
        {
            float scale = 127.0f / maxabs;
            rx = floorf(rx * scale);
            ry = floorf(ry * scale);
        }
        else
        {
            rx = 0.0f;
            ry = 0.0f;
        }

        if (rx > 127.0f) rx = 127.0f;
        else if (rx < -127.0f) rx = -127.0f;
        if (ry > 127.0f) ry = 127.0f;
        else if (ry < -127.0f) ry = -127.0f;

        cmd->forwardmove = (char)(int32_t)rx;
        cmd->rightmove = (char)(int32_t)(-ry);

        // Notify once, on the frame the goal is reached.
        if (!ai->doMove)
            Scr_Notify(bot, (uint16_t)scr_const.movedone, 0);
    }

    // Carry last frame's view, then step it towards the requested angles.
    cmd->angles[0] = prevAngles[0];
    cmd->angles[1] = prevAngles[1];
    cmd->angles[2] = prevAngles[2];

    if (ai->rotIterCount)
    {
        --ai->rotIterCount;

        cmd->angles[0] += ai->rotFrac[0];
        cmd->angles[1] += ai->rotFrac[1];

        for (int32_t i = 0; i < 3; ++i)
        {
            if (cmd->angles[i] < 0)
                cmd->angles[i] += 0xFFFF;
            else if (cmd->angles[i] > 0xFFFF)
                cmd->angles[i] -= 0xFFFF;
        }

        if (!ai->rotIterCount)
            Scr_Notify(bot, (uint16_t)scr_const.rotatedone, 0);
    }

    if (SV_BotShouldSpamUseButton(bot))
        cmd->buttons |= BUTTON_USE | BUTTON_USE_RELOAD;
}

void __cdecl SV_BotClearMovementInfo(int32_t clientNum)
{
    iassert(clientNum >= 0 && clientNum < MAX_CLIENTS);

    BotMovementInfo_t *ai = &g_botai[clientNum];

    ai->buttons = 0;
    ai->doMove = 0;
    ai->rotIterCount = 0;
    ai->weapon = 1; // weapon index 0 is "none"
}

// True once the bot has been dead for the spam delay, so it presses use to
// respawn. Mirrors a player mashing F when scr_player_forcerespawn is 0.
bool __cdecl SV_BotShouldSpamUseButton(gentity_s *bot)
{
    iassert(bot);
    iassert(bot->s.number >= 0 && bot->s.number < MAX_CLIENTS);

    BotMovementInfo_t *ai = &g_botai[bot->s.number];
    bool isAlive = bot->health > 0;

    if (ai->useSpamDelay)
        --ai->useSpamDelay;

    // Was alive last frame, dead now: hold off for three seconds. CoD4x writes
    // sv_fps->boolean here, which is 0 or 1, so its delay is three frames rather
    // than the three seconds its own comment claims.
    if (ai->lastAliveState && !isAlive)
    {
        int32_t delay = sv_fps->current.integer * 3;
        ai->useSpamDelay = (uint8_t)(delay > 255 ? 255 : delay);
    }

    ai->lastAliveState = isAlive;

    return !isAlive && ai->useSpamDelay == 0;
}

// <bot> botMoveTo( <vec3 position> );
void __cdecl GScr_BotMoveTo(scr_entref_t entref)
{
    BotMovementInfo_t *ai = SV_BotAiForEntRef(entref, nullptr);
    if (!ai)
        return;

    if (Scr_GetNumParam() != 1)
    {
        Scr_Error("Usage: <bot> botMoveTo( <vec3 position> );");
        return;
    }

    float moveTo[3];
    Scr_GetVector(0, moveTo);

    ai->moveTo[0] = moveTo[0];
    ai->moveTo[1] = moveTo[1];
    ai->doMove = 1;
}

// <bot> botLookAt( <vec3 origin>, [float duration] );
void __cdecl GScr_BotLookAt(scr_entref_t entref)
{
    gentity_s *bot = nullptr;
    BotMovementInfo_t *ai = SV_BotAiForEntRef(entref, &bot);
    if (!ai)
        return;

    uint32_t argc = Scr_GetNumParam();
    if (argc != 1 && argc != 2)
    {
        Scr_Error("Usage: <bot> botLookAt( <origin>, [duration] );");
        return;
    }

    float lookAt[3];
    Scr_GetVector(0, lookAt);

    float minDuration = 1.0f / sv_fps->current.integer;
    float duration = minDuration;
    if (argc == 2)
        duration = Scr_GetFloat(1);

    if (duration < minDuration)
    {
        Scr_ParamError(1, va("min duration must be %.4f", minDuration));
        return;
    }

    SV_BotCalculateRotationForOrigin(bot, ai, lookAt, duration);
}

// <bot> botStop();
void __cdecl GScr_BotStop(scr_entref_t entref)
{
    BotMovementInfo_t *ai = SV_BotAiForEntRef(entref, nullptr);
    if (!ai)
        return;

    if (Scr_GetNumParam() != 0)
    {
        Scr_Error("Usage: <bot> botStop();");
        return;
    }

    SV_BotClearMovementInfo((int32_t)entref.entnum);
}

// <bot> botAction( <"+action" or "-action"> );
void __cdecl GScr_BotAction(scr_entref_t entref)
{
    BotMovementInfo_t *ai = SV_BotAiForEntRef(entref, nullptr);
    if (!ai)
        return;

    if (Scr_GetNumParam() != 1)
    {
        Scr_Error("Usage: <bot> botAction( <action> );");
        return;
    }

    const char *action = Scr_GetString(0);
    if (!action || (action[0] != '+' && action[0] != '-'))
    {
        Scr_ParamError(0, "Sign for action must be '+' or '-'.");
        return;
    }

    for (int32_t i = 0; i < (int32_t)ARRAY_COUNT(s_botActions); ++i)
    {
        if (!I_stricmp(&action[1], s_botActions[i].action))
        {
            if (action[0] == '+')
                ai->buttons |= s_botActions[i].buttons;
            else
                ai->buttons &= ~s_botActions[i].buttons;

            return;
        }
    }

    char known[1024];
    known[0] = '\0';
    for (int32_t i = 0; i < (int32_t)ARRAY_COUNT(s_botActions); ++i)
    {
        I_strncat(known, sizeof(known), " ");
        I_strncat(known, sizeof(known), s_botActions[i].action);
    }

    Scr_ParamError(0, va("Unknown action. Must be one of:%s.", known));
}

// <bot> botLookAtPlayer( <player>, [tag_name] );
void __cdecl GScr_BotLookAtPlayer(scr_entref_t entref)
{
    gentity_s *bot = nullptr;
    BotMovementInfo_t *ai = SV_BotAiForEntRef(entref, &bot);
    if (!ai)
        return;

    uint32_t argc = Scr_GetNumParam();
    if (argc != 1 && argc != 2)
    {
        Scr_Error("Usage: <bot> botLookAtPlayer( <player>, [tag_name] );");
        return;
    }

    gentity_s *target = Scr_GetEntity(0);
    if (!target || !target->client)
    {
        Scr_ParamError(0, "Not a client.");
        return;
    }

    uint32_t tagName = scr_const.pelvis;
    if (argc == 2)
        tagName = Scr_GetConstString(1);

    if (!GScr_UpdateTagInternal(target, tagName, &level.cachedTagMat, 1))
        return;

    // Lead the target by one frame of its own velocity. CoD4x compares the whole
    // pm_flags word against 32768 here, so its sprint multiplier effectively
    // never applies; PMF_SPRINTING is a bit, so test it as one.
    float multiplier = 1.0f;
    if (target->client->ps.pm_flags & PMF_SPRINTING)
        multiplier = 2.0f;

    float lookOrigin[3];
    Vec3Scale(target->client->ps.velocity, multiplier / sv_fps->current.integer, lookOrigin);
    Vec3Add(lookOrigin, level.cachedTagMat.tagMat[3], lookOrigin);

    SV_BotCalculateRotationForOrigin(bot, ai, lookOrigin, 1.0f / sv_fps->current.integer);
}

// <bot> botWeapon( <weapon name> );
void __cdecl GScr_BotWeapon(scr_entref_t entref)
{
    BotMovementInfo_t *ai = SV_BotAiForEntRef(entref, nullptr);
    if (!ai)
        return;

    if (Scr_GetNumParam() != 1)
    {
        Scr_Error("Usage: <bot> botWeapon( <weapon> );");
        return;
    }

    const char *weapon = Scr_GetString(0);

    if (!weapon || !*weapon)
    {
        ai->weapon = 1; // weapon index 0 is "none"
        return;
    }

    int32_t weaponIndex = G_GetWeaponIndexForName(weapon);

    // The usercmd carries the weapon in MAX_WEAPONS_BITS (7) bits and the pool is
    // 128 entries, so anything wider than a byte is already a bug upstream.
    if (weaponIndex < 0 || weaponIndex >= 256)
    {
        Scr_ParamError(0, va("weapon index %i out of range", weaponIndex));
        return;
    }

    ai->weapon = (uint8_t)weaponIndex;
}

// Stock CoD4 has SV_AddTestClient and no counterpart, so bots accumulate until
// the map ends. CoD4x matches on netchan.remoteAddress.type == NA_BOT; this
// tests bIsTestClient instead, because NA_BOT is zero in this tree's netadr
// enum and a zeroed address would therefore read as a bot.
gentity_s *__cdecl SV_RemoveTestClient()
{
    for (int32_t i = 0; i < sv_maxclients->current.integer; ++i)
    {
        client_t *cl = &svs.clients[i];

        if (cl->header.state <= CS_FREE || !cl->bIsTestClient)
            continue;

        // Whatever script had queued for this slot dies with the client. The
        // slot is reused by the next connect, so a stale doMove there would
        // steer a real player.
        SV_BotClearMovementInfo(i);
        SV_DropClient(cl, "EXE_DISCONNECTED", 1);

        return SV_GentityNumLocal(i);
    }

    return nullptr;
}

int32_t __cdecl SV_RemoveAllTestClients()
{
    int32_t removed = 0;

    for (int32_t i = 0; i < sv_maxclients->current.integer; ++i)
    {
        client_t *cl = &svs.clients[i];

        if (cl->header.state <= CS_FREE || !cl->bIsTestClient)
            continue;

        SV_BotClearMovementInfo(i);
        SV_DropClient(cl, "EXE_DISCONNECTED", 1);
        ++removed;
    }

    return removed;
}

// entity = removetestclient()
//
// Returns the entity that was dropped so a caller can log it; undefined when
// there was no bot to remove.
void __cdecl GScr_RemoveTestClient()
{
    if (Scr_GetNumParam() != 0)
    {
        Scr_Error("Usage: entity = removeTestClient();");
        return;
    }

    gentity_s *ent = SV_RemoveTestClient();

    if (ent)
        Scr_AddEntity(ent);
}

// int = removealltestclients()
void __cdecl GScr_RemoveAllTestClients()
{
    if (Scr_GetNumParam() != 0)
    {
        Scr_Error("Usage: int = removeAllTestClients();");
        return;
    }

    Scr_AddInt(SV_RemoveAllTestClients());
}

// Registered at VM init rather than as rows in the static tables. This is how
// CoD4x does it, and it keeps those tables untouched.
void __cdecl Scr_AddBotsMovement()
{
    Scr_AddMethod("botmoveto", GScr_BotMoveTo, 0);
    Scr_AddMethod("botlookat", GScr_BotLookAt, 0);
    Scr_AddMethod("botstop", GScr_BotStop, 0);
    Scr_AddMethod("botaction", GScr_BotAction, 0);
    Scr_AddMethod("botlookatplayer", GScr_BotLookAtPlayer, 0);
    Scr_AddMethod("botweapon", GScr_BotWeapon, 0);

    // addtestclient is a stock builtin; only the removals are new.
    Scr_AddFunction("removetestclient", GScr_RemoveTestClient, 0);
    Scr_AddFunction("removealltestclients", GScr_RemoveAllTestClients, 0);
}
