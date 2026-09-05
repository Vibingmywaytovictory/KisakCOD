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

// sv_randomMapRotation, from CoD4x's sv_cmds.c. Stock plays sv_mapRotation in
// the order it is written, forever; with this on, the order is reshuffled every
// time the rotation runs out and is refilled, so a server does not present the
// same sequence night after night.
//
// Off by default: a rotation that was written in a deliberate order should keep
// it unless an admin says otherwise.
void __cdecl SV_MapRotation_Init();

// Copies sv_mapRotation into sv_mapRotationCurrent, shuffled when
// sv_randomMapRotation is set. Called wherever the rotation is refilled.
//
// Shuffling is done on gametype/map pairs rather than on tokens, so a map keeps
// the gametype it was written with; a map with no gametype of its own inherits
// the one most recently named, which is what playing the rotation in order
// would have given it.
void __cdecl SV_MapRotation_Refill();
