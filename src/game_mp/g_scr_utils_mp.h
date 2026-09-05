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
#include <script/scr_vm.h>

// The utility half of CoD4x's scr_vm_functions.c: string helpers, type
// predicates, a few maths and time functions, base64, and a console bridge.
// Stock CoD4 has none of these, so every name here is new script surface
// rather than a behaviour change to an existing builtin.
//
// Registered through Scr_AddFunction / Scr_AddMethod at GScr_LoadScripts, so
// the decompiled functions[] and methods_2[] tables stay at their stock sizes.
//
// Three deliberate departures from CoD4x, all noted at the implementation:
//   - the string builtins are bounded and NUL terminated; CoD4x's write past
//     fixed stack buffers on long input and one of them never terminates,
//   - isarray/isentity test the pointer type rather than the value type, which
//     is what this VM actually reports for objects,
//   - exec/execex are gated by sv_scriptExec; CoD4x lets any script run any
//     console command, including quit and set rcon_password.
//
// Not ported from that file, and why:
//   strpixlen, strtokbypixlen  a hand-guessed pixel width table for one font
//   sha256                     wants a crypto primitive this tree has no home
//                              for yet; it belongs with the auth block
//   addscriptcommand, usercall  built on Cmd_GetInvokerClnum and the plugin
//                              callback path, both in the deferred auth work
//   arraytest, printstar        developer-only test and debug-draw helpers
void __cdecl Scr_AddUtilityFunctions();

// Registered by Scr_AddUtilityFunctions; declared here so the registration
// list and the implementations can be read against each other.
void __cdecl GScr_ToUpper();
void __cdecl GScr_StrColorStrip();
void __cdecl GScr_StrCtrlStrip();
void __cdecl GScr_StrRepl();
void __cdecl GScr_StrReplace();
void __cdecl GScr_StrTokByLen();
void __cdecl GScr_Pow();
void __cdecl GScr_Float();
void __cdecl GScr_VectorAdd();
void __cdecl GScr_IsArrayValue();
void __cdecl GScr_IsEntityValue();
void __cdecl GScr_IsVectorValue();
void __cdecl GScr_IsFloatValue();
void __cdecl GScr_IsIntValue();
void __cdecl GScr_IsDvarDefined();
void __cdecl GScr_GetRealTime();
void __cdecl GScr_TimeToString();
void __cdecl GScr_Base64Encode();
void __cdecl GScr_Base64Decode();
void __cdecl GScr_Exec();
void __cdecl GScr_ExecEx();

void __cdecl PlayerCmd_GetUserinfo(scr_entref_t arg);
void __cdecl PlayerCmd_GetPing(scr_entref_t arg);
