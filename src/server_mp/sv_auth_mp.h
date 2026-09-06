#pragma once
#ifndef KISAK_MP
#error This File is MultiPlayer Only
#endif

#include <universal/q_shared.h>
#include <universal/sha256.h>

struct client_t;

// Admin authentication for the in-game admin system, modelled on CoD4x
// (AGPLv3) sv_auth.c but NOT a transcription of it, because CoD4x's shipped
// behaviour does not match what its own header promises.
//
// In CoD4x, Auth_GetClPower falls through to Auth_GetClPowerBySteamID, which
// hands out an admin's power to anyone presenting that steamid. The password
// path -- Auth_Login_f -- is commented out in the released source and does not
// even compile (it references an undefined `id`). So on a stock CoD4x server,
// identity IS authority: spoof the id, inherit the power.
//
// That is exactly the weakness this port exists to avoid. Here:
//
//   - Power is granted ONLY by a successful password login, and only for the
//     duration of that connection. Auth_GetClPower reads a session field set by
//     Auth_Login; there is no lookup by identity anywhere in the grant path.
//   - The stored guid is a BINDING, not a credential. If a record carries one,
//     the login must come from that guid as well as present the password. It
//     can only ever narrow who may log in, never widen it. cdkeyHash is what
//     this tree calls that identity, and it is what SV_IsBannedGuid matches on.
//   - Logging in from a second slot does not clone power to the first.
//
// The file is server-local and holds only salted, iterated hashes, so it does
// not need to be secret to be safe -- but it should still not be world readable,
// because an offline attack on a weak password is the remaining risk.

#define MAX_AUTH_ADMINS 512

// Long enough for a CoD4 player name plus a terminator; the username is a login
// handle, not a display name, and is matched case-insensitively.
#define AUTH_USERNAME_SIZE 32

// Matches SV_BANLIST_GUID_SIZE: cdkeyHash is a char[33] throughout the tree.
#define AUTH_GUID_SIZE 33

// Power levels. 1 is what an unauthenticated player has; nothing is gated at or
// below it. 100 is the server owner, who can always run everything.
#define AUTH_POWER_DEFAULT 1
#define AUTH_POWER_MAX     100

struct authAdmin_t
{
    char     username[AUTH_USERNAME_SIZE];
    char     salt[SHA256_HEX_SIZE];
    char     hash[SHA256_HEX_SIZE];
    int32_t  power;
    char     guid[AUTH_GUID_SIZE];   // empty = not bound to an identity
    bool     inUse;
};

void __cdecl Auth_Init();
void __cdecl Auth_Shutdown();

// Reload from disk, discarding the in-memory list. Live sessions keep the power
// they were granted; a reload must not silently promote or demote a player who
// is already logged in.
void __cdecl Auth_Load();
bool __cdecl Auth_Save();

// The only way power is ever granted. Returns the power on success, or 0 on
// failure -- failure is deliberately indistinguishable between "no such user"
// and "wrong password" so the caller cannot enumerate usernames.
int32_t __cdecl Auth_Login(client_t *cl, const char *username, const char *password);

// Drops the session's power. Called on disconnect, and by the logout command.
void __cdecl Auth_Logout(client_t *cl);

// The authenticated power of a connected client. Never consults identity.
int32_t __cdecl Auth_GetClPower(const client_t *cl);

// The login handle a client authenticated as, or NULL if they have not.
const char *__cdecl Auth_GetClName(const client_t *cl);

bool __cdecl Auth_AddAdmin(const char *username, const char *password, int32_t power, const char *guid);
bool __cdecl Auth_RemoveAdmin(const char *username);
bool __cdecl Auth_ChangePassword(const char *username, const char *oldPassword, const char *newPassword);

// Per-command minimum power, stored beside the command table. A command with no
// entry is unrestricted, which matters: this system gates the admin surface, it
// is not an allowlist for the whole console.
int32_t __cdecl Auth_GetCommandPower(const char *cmd);
void    __cdecl Auth_SetCommandPower(const char *cmd, int32_t power);
bool    __cdecl Auth_CanUseCommand(const client_t *cl, const char *cmd);
void    __cdecl Auth_ListCommandPowers();

void __cdecl Auth_RegisterCommands();
