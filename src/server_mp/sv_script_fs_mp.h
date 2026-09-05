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

// GSC global functions, registered in functions[] (game_mp/g_scr_main_mp.cpp).
//
// Every path these take is confined to scriptdata/ by the engine. CoD4x does
// that prefixing in its GSC adapter instead, which leaves the engine builtins
// themselves able to open anything the process can reach.
void __cdecl GScr_FS_FOpen();
void __cdecl GScr_FS_FClose();
void __cdecl GScr_FS_TestFile();
void __cdecl GScr_FS_ReadLine();
void __cdecl GScr_FS_WriteLine();

// Drop every handle a script left open. Called when the VM shuts down so
// handles do not leak across a map change.
void __cdecl SV_ScriptFS_CloseAll();
