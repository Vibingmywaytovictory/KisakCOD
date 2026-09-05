// Portions of this file are derived from CoD4x_Server
//   https://github.com/callofduty4x/CoD4x_Server
//   Copyright (C) 2010-2013 Ninja and TheKelm
//   Copyright (C) the CoD4x authors
// which in turn carries the leaky bucket limiter from ioquake3
//   Copyright (C) 1999-2005 Id Software, Inc.
// Licensed under the GNU Affero General Public License v3 (see LICENSE.AGPLv3).
//
// Adapted to KisakCOD's engine APIs. The combined work is conveyed under
// GPLv3 section 13; see LICENSING.md.

#pragma once

#include <universal/q_shared.h>
#include <qcommon/net_chan_mp.h>

// Rate limiting for connectionless packets, which are the only thing a
// stranger can make this server do work for. Two problems it addresses:
//
//   Amplification. A getstatus reply is far larger than the request, and the
//   request is a single spoofable UDP datagram. Point enough servers at one
//   victim address and the replies are the attack. Every Quake 3 lineage
//   server shipped with this; ioquake3 fixed it with the leaky bucket below
//   and CoD4x carried the fix across.
//
//   Brute force. Nothing else limits how fast rcon passwords can be guessed.
//
// Two kinds of limit, both from CoD4x. The per-address one holds a single
// source to a slow rate. The global one caps what the server will emit in
// total, so a flood from thousands of spoofed addresses -- which defeats any
// per-address scheme -- still cannot turn this server into an amplifier.
//
// Both return true when the caller should drop the packet.

enum SvRateLimitBucket
{
    SV_RATELIMIT_STATUS,
    SV_RATELIMIT_INFO,
    SV_RATELIMIT_RCON,

    SV_RATELIMIT_BUCKET_COUNT
};

// Registers the dvars and clears the pool. Safe to call more than once.
void __cdecl SV_RateLimitInit();

// True if `from` has already had `burst` packets inside `periodMsec`. LAN and
// loopback addresses are never limited, so a local admin cannot lock themself
// out and a LAN server browser stays responsive.
bool __cdecl SV_RateLimitAddress(netadr_t from, int32_t burst, int32_t periodMsec);

// True if the server as a whole has already emitted `burst` of this reply
// inside `periodMsec`, regardless of who asked.
bool __cdecl SV_RateLimitGlobal(SvRateLimitBucket bucket, int32_t burst, int32_t periodMsec);

// sv_queryIgnoreTime in milliseconds, for callers building a per-address
// period out of it. Same name and meaning as CoD4x's dvar.
int32_t __cdecl SV_QueryIgnoreTime();
