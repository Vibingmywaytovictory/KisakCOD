// Portions of this file are derived from CoD4x_Server
//   https://github.com/callofduty4x/CoD4x_Server
//   Copyright (C) the CoD4x authors
// Licensed under the GNU Affero General Public License v3 (see LICENSE.AGPLv3).
//
// Adapted to KisakCOD's engine APIs. The combined work is conveyed under
// GPLv3 section 13; see LICENSING.md.

#include <universal/q_shared.h>
#include "g_scr_builtins_mp.h"
#include "g_public_mp.h"

#include <qcommon/qcommon.h>

#include <string.h>

#define MAX_DYNAMIC_BUILTINS 256
#define MAX_BUILTIN_NAME_LEN 64

struct DynamicBuiltin_t
{
    char    name[MAX_BUILTIN_NAME_LEN];
    void   *function;   // ScrFunction_t or ScrMethod_t, per which pool it is in
    int32_t developer;
    bool    inUse;
};

static DynamicBuiltin_t s_dynFunctions[MAX_DYNAMIC_BUILTINS];
static DynamicBuiltin_t s_dynMethods[MAX_DYNAMIC_BUILTINS];

// The script compiler lowercases a name before resolving it, and the static
// tables are lowercase throughout, so normalise here rather than leave a
// registration that can never be matched.
static bool SV_NormalizeBuiltinName(const char *in, char *out, uint32_t outSize)
{
    if (!in || !*in || !out)
        return false;

    uint32_t n = 0;
    for (const char *p = in; *p; ++p)
    {
        if (n + 1 >= outSize)
            return false;   // too long to register

        char c = *p;
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');

        out[n++] = c;
    }

    out[n] = '\0';
    return true;
}

static DynamicBuiltin_t *SV_FindDynamicSlot(DynamicBuiltin_t *pool, const char *name)
{
    for (int32_t i = 0; i < MAX_DYNAMIC_BUILTINS; ++i)
    {
        if (pool[i].inUse && !strcmp(pool[i].name, name))
            return &pool[i];
    }
    return nullptr;
}

static DynamicBuiltin_t *SV_AllocDynamicSlot(DynamicBuiltin_t *pool)
{
    for (int32_t i = 0; i < MAX_DYNAMIC_BUILTINS; ++i)
    {
        if (!pool[i].inUse)
            return &pool[i];
    }
    return nullptr;
}

bool __cdecl Scr_AddFunction(const char *name, ScrFunction_t function, int32_t developer)
{
    char lower[MAX_BUILTIN_NAME_LEN];

    if (!function || !SV_NormalizeBuiltinName(name, lower, sizeof(lower)))
    {
        Com_Printf(CON_CHANNEL_SCRIPT, "Scr_AddFunction: bad name or null function\n");
        return false;
    }

    // Resolving through the normal path catches a clash with a stock builtin as
    // well as with another dynamic one. Stock names are not overridable: the
    // static tables are searched first, so a shadowing entry would never be hit.
    {
        const char *probe = lower;
        int32_t probeType = 0;
        if (Scr_GetFunction(&probe, &probeType))
        {
            Com_Printf(CON_CHANNEL_SCRIPT, "Scr_AddFunction: '%s' is already defined\n", lower);
            return false;
        }
    }

    DynamicBuiltin_t *slot = SV_AllocDynamicSlot(s_dynFunctions);
    if (!slot)
    {
        Com_Printf(CON_CHANNEL_SCRIPT,
                   "Scr_AddFunction: out of slots (max %i), '%s' not registered\n",
                   MAX_DYNAMIC_BUILTINS, lower);
        return false;
    }

    I_strncpyz(slot->name, lower, sizeof(slot->name));
    slot->function = (void *)function;
    slot->developer = developer;
    slot->inUse = true;
    return true;
}

bool __cdecl Scr_AddMethod(const char *name, ScrMethod_t method, int32_t developer)
{
    char lower[MAX_BUILTIN_NAME_LEN];

    if (!method || !SV_NormalizeBuiltinName(name, lower, sizeof(lower)))
    {
        Com_Printf(CON_CHANNEL_SCRIPT, "Scr_AddMethod: bad name or null method\n");
        return false;
    }

    {
        const char *probe = lower;
        int32_t probeType = 0;
        if (BuiltIn_GetMethod(&probe, &probeType))
        {
            Com_Printf(CON_CHANNEL_SCRIPT, "Scr_AddMethod: '%s' is already defined\n", lower);
            return false;
        }
    }

    DynamicBuiltin_t *slot = SV_AllocDynamicSlot(s_dynMethods);
    if (!slot)
    {
        Com_Printf(CON_CHANNEL_SCRIPT,
                   "Scr_AddMethod: out of slots (max %i), '%s' not registered\n",
                   MAX_DYNAMIC_BUILTINS, lower);
        return false;
    }

    I_strncpyz(slot->name, lower, sizeof(slot->name));
    slot->function = (void *)method;
    slot->developer = developer;
    slot->inUse = true;
    return true;
}

bool __cdecl Scr_RemoveFunction(const char *name)
{
    char lower[MAX_BUILTIN_NAME_LEN];
    if (!SV_NormalizeBuiltinName(name, lower, sizeof(lower)))
        return false;

    DynamicBuiltin_t *slot = SV_FindDynamicSlot(s_dynFunctions, lower);
    if (!slot)
        return false;

    memset(slot, 0, sizeof(*slot));
    return true;
}

bool __cdecl Scr_RemoveMethod(const char *name)
{
    char lower[MAX_BUILTIN_NAME_LEN];
    if (!SV_NormalizeBuiltinName(name, lower, sizeof(lower)))
        return false;

    DynamicBuiltin_t *slot = SV_FindDynamicSlot(s_dynMethods, lower);
    if (!slot)
        return false;

    memset(slot, 0, sizeof(*slot));
    return true;
}

void __cdecl Scr_ClearDynamicBuiltins()
{
    memset(s_dynFunctions, 0, sizeof(s_dynFunctions));
    memset(s_dynMethods, 0, sizeof(s_dynMethods));
}

ScrFunction_t __cdecl Scr_FindDynamicFunction(const char **pName, int32_t *type)
{
    iassert(pName);
    iassert(type);

    DynamicBuiltin_t *slot = SV_FindDynamicSlot(s_dynFunctions, *pName);
    if (!slot)
        return nullptr;

    *pName = slot->name;   // stable for the life of the registration
    *type = slot->developer;
    return (ScrFunction_t)slot->function;
}

ScrMethod_t __cdecl Scr_FindDynamicMethod(const char **pName, int32_t *type)
{
    iassert(pName);
    iassert(type);

    DynamicBuiltin_t *slot = SV_FindDynamicSlot(s_dynMethods, *pName);
    if (!slot)
        return nullptr;

    *pName = slot->name;
    *type = slot->developer;
    return (ScrMethod_t)slot->function;
}
