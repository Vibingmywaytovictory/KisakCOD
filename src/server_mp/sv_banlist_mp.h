#pragma once

#include <universal/q_shared.h>

// ban.txt, held in memory instead of read and parsed per lookup.
//
// SV_IsBannedGuid used to FS_ReadFile and Com_Parse the whole file on every
// call, and SV_GetChallenge calls it for every connect attempt -- which is an
// unauthenticated packet. That made a file read and a full parse reachable by
// anyone who could send UDP. The connectionless rate limiter bounds how often
// that can happen; this removes the cost itself.
//
// Note that CoD4x is no help here: its sv_banlist.c delegates every ban
// question to a plugin and its own internal list is commented out, so there
// was nothing to port.
//
// The cache reloads when this server changes the file, and, for a file edited
// underneath it, on the next lookup after SV_BANLIST_RECHECK_MSEC. Reloading
// is lazy, so an idle server does no work at all.

// Loads on first use. Returns true if the guid appears in ban.txt.
// Same contract as the function it replaces, including returning false for an
// empty guid and for a missing file.
int __cdecl SV_IsBannedGuid(const char *guid);

// Drops the cache so the next lookup re-reads the file. Call after anything
// that writes ban.txt.
void __cdecl SV_BanList_Invalidate();

// Registers the banlist_reload command. Called from SV_AddOperatorCommands.
void __cdecl SV_BanList_AddCommands();
