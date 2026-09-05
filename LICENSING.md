# Licensing

This fork combines code under two different copyleft licences. Both apply; neither
replaces the other.

## KisakCOD itself — GPLv3

Everything originating in [SwagSoftware/KisakCOD](https://github.com/SwagSoftware/KisakCOD)
and from its contributors remains under the GNU General Public License v3. See
[`LICENSE`](./LICENSE). This fork does not and cannot relicense that code — its copyright
belongs to the upstream authors.

## Code derived from CoD4x_Server — AGPLv3

[CoD4x_Server](https://github.com/callofduty4x/CoD4x_Server) is licensed under the GNU
**Affero** General Public License v3, not the plain GPLv3 (see its `README.md` and
`License.md`). Any file here that is ported, derived, or adapted from CoD4x_Server carries
AGPLv3 and is marked with a per-file header saying so. See [`LICENSE.AGPLv3`](./LICENSE.AGPLv3).

## The combination

GPLv3 section 13 expressly permits combining a GPLv3 work with an AGPLv3 work and conveying
the result. The GPLv3 portions stay GPLv3; AGPLv3 section 13's network-interaction
requirement applies to the combination as a whole.

**What that means in practice for anyone running a server built from this repository:** if
you modify it and let users interact with it over a network, you must offer those users the
corresponding source of your modified version. Publishing your fork satisfies this.

## Header to use on ported files

Every file containing CoD4x-derived code gets this at the top, with the original authors
credited:

```
// Portions of this file are derived from CoD4x_Server
//   https://github.com/callofduty4x/CoD4x_Server
//   Copyright (C) the CoD4x authors
// Licensed under the GNU Affero General Public License v3 (see LICENSE.AGPLv3).
//
// Adapted to KisakCOD's engine APIs. The combined work is conveyed under
// GPLv3 section 13; see LICENSING.md.
```

## Files containing CoD4x-derived code

Kept current as ports land.

| File | Derived from | Notes |
|---|---|---|
| `src/server_mp/sv_bots_mp.h` | `src/sv_bots.h` | Bot input struct and declarations. |
| `src/server_mp/sv_bots_mp.cpp` | `src/sv_bots.cpp` | Six GSC bot methods, action table, usercmd translation. |
| `src/server_mp/sv_script_fs_mp.h` | `src/scr_vm_fs.c` | Script file I/O declarations. |
| `src/server_mp/sv_script_fs_mp.cpp` | `src/scr_vm_fs.c`, `src/scr_vm_functions.c` | Five fs_* GSC builtins. Re-implemented on KisakCOD's filesystem; sandbox is new. |
| `src/game/g_client_fields.cpp` | `src/g_client_fields.cpp` | The `isbot` client field and its getter only; the rest of the file is KisakCOD's own GPLv3 code. |

The bot changes inside `src/server_mp/sv_main_mp.cpp` (`SV_BotUserMove`) and the six
rows added to `methods_2[]` in `src/game_mp/g_scr_main_mp.cpp` are edits to existing
GPLv3 files that call into the AGPLv3 code above; the files themselves stay GPLv3.

## Deliberately excluded

- **`CoD4x_Client_pub`** — carries Activision's licence terms rather than GPL/AGPL. No code
  from it enters this repository. Anything wanted from it is reimplemented from scratch.
- **CoD4x's `src/botlib/` and `src/sv_bot.c`** — vendored Quake 3 / Return to Castle
  Wolfenstein code under id Software's GPL releases, which carry additional terms whose text
  is not distributed with CoD4x. Not used: `sv_bot.c` is glue to the Q3 area-awareness
  system, which the GSC bot API does not touch.
- **CoD4x's reconstructions of stock engine files** — `cscr_*`, `filesystem.c`, `q_math.c`,
  `qshared.c`, `cvar.c`, `msg.c`, `huffman.c`, `netchan.c`, `db_load.cpp` and similar.
  KisakCOD's own versions are decompiled further and correctly named. Taking these would be
  both a downgrade and needless licence mixing.
