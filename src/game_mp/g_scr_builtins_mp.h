// Portions of this file are derived from CoD4x_Server
//   https://github.com/callofduty4x/CoD4x_Server
//   Copyright (C) the CoD4x authors
// Licensed under the GNU Affero General Public License v3 (see LICENSE.AGPLv3).
//
// Adapted to KisakCOD's engine APIs. The combined work is conveyed under
// GPLv3 section 13; see LICENSING.md.

#pragma once

#include <universal/q_shared.h>
#include <script/scr_variable.h>

// Runtime registration of GSC builtins, on top of the static functions[] and
// methods_2[] tables in g_scr_main_mp.cpp.
//
// The static tables stay exactly as they are and remain the built-in set; these
// are an overlay consulted only after a name misses there, so a dynamic entry can
// never shadow a stock builtin. That ordering is deliberate -- a plugin should not
// be able to silently replace engine behaviour that scripts rely on.
//
// CoD4x keeps these in malloc'd linked lists. This uses a fixed pool instead,
// matching how the rest of this engine sizes things, so there is no allocation
// lifetime to get wrong and clearing is trivial.

typedef void(__cdecl *ScrFunction_t)();
typedef void(__cdecl *ScrMethod_t)(scr_entref_t);

// Register a global function or an entity method. Returns false if the pool is
// full, the name is unusable, or the name is already taken -- by another dynamic
// entry OR by a stock builtin, which cannot be overridden.
//
// `name` is normalised to lower case, because the script compiler lowercases
// before it looks a name up and the static tables are lowercase by convention.
// `developer` mirrors the third column of the static tables.
bool __cdecl Scr_AddFunction(const char *name, ScrFunction_t function, int32_t developer);
bool __cdecl Scr_AddMethod(const char *name, ScrMethod_t method, int32_t developer);

bool __cdecl Scr_RemoveFunction(const char *name);
bool __cdecl Scr_RemoveMethod(const char *name);

// Drop every dynamic registration. Runs on game shutdown so a stale function
// pointer cannot survive into the next map.
void __cdecl Scr_ClearDynamicBuiltins();

// Consulted by Scr_GetFunction / BuiltIn_GetMethod after the static tables miss.
// On a hit, *pName is repointed at the registry's own copy of the name, which is
// stable for the life of the registration, and *type receives the developer flag.
ScrFunction_t __cdecl Scr_FindDynamicFunction(const char **pName, int32_t *type);
ScrMethod_t __cdecl Scr_FindDynamicMethod(const char **pName, int32_t *type);

// Registration points for the subsystems ported from CoD4x. Called from
// GScr_LoadScripts before any script is compiled.
void __cdecl Scr_AddBotsMovement();
void __cdecl Scr_AddScriptFileFunctions();
void __cdecl Scr_AddUtilityFunctions();
