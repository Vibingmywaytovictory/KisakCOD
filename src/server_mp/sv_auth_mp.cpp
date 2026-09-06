#ifndef KISAK_MP
#error This File is MultiPlayer Only
#endif

#include "sv_auth_mp.h"
#include "server_mp.h"

#include <qcommon/qcommon.h>
#include <qcommon/cmd.h>
#include <universal/com_files.h>
#include <universal/sha256.h>

#include <string.h>
#include <stdlib.h>

// Where the list lives. Read and written through FS_SV_*, so it sits beside
// ban.txt in the server's own directory rather than anywhere a client-visible
// search path can reach.
#define AUTH_ADMIN_FILE "admins.txt"

// Online guessing defence. Distinct from the offline cost in SHA256_HashPassword:
// that one raises the price of attacking a stolen file, this one stops someone
// simply typing passwords at a live server. After AUTH_MAX_ATTEMPTS failures a
// slot cannot try again until the window expires.
#define AUTH_MAX_ATTEMPTS     5
#define AUTH_LOCKOUT_MSEC     (60 * 1000)

#define AUTH_MEMORY_TYPE 9

// Per-command minimum power, held beside the command table rather than inside
// it. CoD4x widened its cmd_function_s with a power field; here that struct is
// recovered from the binary and shared with every other command in the engine,
// so a side table keyed by name is the smaller change and cannot shift a layout
// something else has frozen. Lookup is O(n) over a list that holds only the
// commands an admin has actually restricted, which is a handful.
#define MAX_COMMAND_POWERS 128

struct authCommandPower_t
{
    char    name[64];
    int32_t power;
};

static authAdmin_t         g_authAdmins[MAX_AUTH_ADMINS];
static authCommandPower_t  g_authCommandPowers[MAX_COMMAND_POWERS];
static int32_t             g_authCommandPowerCount;
static bool                g_authLoaded;

// Login throttling, indexed by client slot.
static int32_t  g_authAttempts[MAX_CLIENTS];
static uint32_t g_authLockoutUntil[MAX_CLIENTS];

static int32_t Auth_ClientNum(const client_t *cl)
{
    if (!cl)
        return -1;

    const int32_t num = (int32_t)(cl - svs.clients);
    if (num < 0 || num >= MAX_CLIENTS)
        return -1;

    return num;
}

static authAdmin_t *Auth_FindByName(const char *username)
{
    if (!username || !*username)
        return NULL;

    for (int32_t i = 0; i < MAX_AUTH_ADMINS; ++i)
    {
        if (g_authAdmins[i].inUse && !I_stricmp(g_authAdmins[i].username, username))
            return &g_authAdmins[i];
    }

    return NULL;
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

void __cdecl Auth_Load()
{
    memset(g_authAdmins, 0, sizeof(g_authAdmins));
    g_authLoaded = true;

    int handle = 0;
    const int len = FS_SV_FOpenFileRead(AUTH_ADMIN_FILE, &handle);

    if (!handle || len <= 0)
    {
        if (handle)
            FS_FCloseFile(handle);
        Com_Printf(15, "No " AUTH_ADMIN_FILE " found; no admins are configured.\n");
        return;
    }

    char *buffer = (char *)Z_Malloc(len + 1, "Auth_Load", AUTH_MEMORY_TYPE);
    if (!buffer)
    {
        FS_FCloseFile(handle);
        Com_PrintError(15, "Auth_Load: out of memory reading " AUTH_ADMIN_FILE "\n");
        return;
    }

    FS_Read((uint8_t *)buffer, len, handle);
    FS_FCloseFile(handle);
    buffer[len] = 0;

    int32_t count = 0;
    int32_t lineNum = 0;
    char *cursor = buffer;

    while (cursor && *cursor)
    {
        char *line = cursor;
        char *newline = strchr(cursor, '\n');

        if (newline)
        {
            *newline = 0;
            cursor = newline + 1;
        }
        else
        {
            cursor = NULL;
        }

        ++lineNum;

        // Tolerate CRLF files written by hand on Windows.
        const size_t lineLen = strlen(line);
        if (lineLen && line[lineLen - 1] == '\r')
            line[lineLen - 1] = 0;

        while (*line == ' ' || *line == '\t')
            ++line;

        if (!*line || *line == '#' || *line == '/')
            continue;

        if (count >= MAX_AUTH_ADMINS)
        {
            Com_PrintWarning(15, "WARNING: " AUTH_ADMIN_FILE " holds more than %i admins; the rest are ignored.\n",
                MAX_AUTH_ADMINS);
            break;
        }

        // username power salt hash [guid]
        char username[AUTH_USERNAME_SIZE];
        char salt[SHA256_HEX_SIZE];
        char hash[SHA256_HEX_SIZE];
        char guid[AUTH_GUID_SIZE];
        int32_t power = 0;

        guid[0] = 0;

        const int fields = sscanf(line, "%31s %d %64s %64s %32s",
            username, &power, salt, hash, guid);

        // The guid is optional, so four fields is already a complete record.
        if (fields < 4)
        {
            Com_PrintWarning(15, "WARNING: " AUTH_ADMIN_FILE " line %i is malformed and was skipped.\n", lineNum);
            continue;
        }
        if (fields < 5 || !strcmp(guid, "-"))
            guid[0] = 0;

        if (power < AUTH_POWER_DEFAULT || power > AUTH_POWER_MAX)
        {
            Com_PrintWarning(15, "WARNING: " AUTH_ADMIN_FILE " line %i has power %i outside [%i, %i]; skipped.\n",
                lineNum, power, AUTH_POWER_DEFAULT, AUTH_POWER_MAX);
            continue;
        }
        if (strlen(salt) != SHA256_HEX_SIZE - 1 || strlen(hash) != SHA256_HEX_SIZE - 1)
        {
            Com_PrintWarning(15, "WARNING: " AUTH_ADMIN_FILE " line %i has a malformed salt or hash; skipped.\n", lineNum);
            continue;
        }
        if (Auth_FindByName(username))
        {
            Com_PrintWarning(15, "WARNING: " AUTH_ADMIN_FILE " line %i repeats the name '%s'; skipped.\n",
                lineNum, username);
            continue;
        }

        authAdmin_t *admin = &g_authAdmins[count];
        I_strncpyz(admin->username, username, sizeof(admin->username));
        I_strncpyz(admin->salt, salt, sizeof(admin->salt));
        I_strncpyz(admin->hash, hash, sizeof(admin->hash));
        I_strncpyz(admin->guid, guid, sizeof(admin->guid));
        admin->power = power;
        admin->inUse = true;
        ++count;
    }

    Z_Free(buffer, AUTH_MEMORY_TYPE);
    Com_Printf(15, "Loaded %i admin%s from " AUTH_ADMIN_FILE ".\n", count, count == 1 ? "" : "s");
}

bool __cdecl Auth_Save()
{
    const int handle = FS_SV_FOpenFileWrite(AUTH_ADMIN_FILE);
    if (!handle)
    {
        Com_PrintError(15, "Auth_Save: could not open " AUTH_ADMIN_FILE " for writing.\n");
        return false;
    }

    static const char *header[] =
    {
        "// KisakCOD admin list.\n",
        "// username power salt hash [guid]\n",
        "// Passwords are salted and iterated, never stored. Editing a hash by hand\n",
        "// locks that admin out; use AdminAddAdmin to set a new one.\n",
        "// A guid of - means the account may be logged into from any identity.\n",
    };

    for (int32_t i = 0; i < (int32_t)ARRAY_COUNT(header); ++i)
        FS_Write(header[i], (uint32_t)strlen(header[i]), handle);

    char line[512];
    int32_t written = 0;

    for (int32_t i = 0; i < MAX_AUTH_ADMINS; ++i)
    {
        const authAdmin_t *admin = &g_authAdmins[i];
        if (!admin->inUse)
            continue;

        Com_sprintf(line, sizeof(line), "%s %i %s %s %s\n",
            admin->username, admin->power, admin->salt, admin->hash,
            admin->guid[0] ? admin->guid : "-");
        FS_Write(line, (uint32_t)strlen(line), handle);
        ++written;
    }

    FS_FCloseFile(handle);
    Com_Printf(15, "Wrote %i admin%s to " AUTH_ADMIN_FILE ".\n", written, written == 1 ? "" : "s");
    return true;
}

// ---------------------------------------------------------------------------
// The grant path
// ---------------------------------------------------------------------------

int32_t __cdecl Auth_Login(client_t *cl, const char *username, const char *password)
{
    const int32_t clientNum = Auth_ClientNum(cl);
    if (clientNum < 0 || !username || !password)
        return 0;

    if (!g_authLoaded)
        Auth_Load();

    const uint32_t now = Sys_Milliseconds();

    if (g_authLockoutUntil[clientNum])
    {
        if (now < g_authLockoutUntil[clientNum])
        {
            Com_Printf(15, "Too many failed login attempts. Try again in %u seconds.\n",
                (g_authLockoutUntil[clientNum] - now) / 1000 + 1);
            return 0;
        }

        g_authLockoutUntil[clientNum] = 0;
        g_authAttempts[clientNum] = 0;
    }

    const authAdmin_t *admin = Auth_FindByName(username);

    // Hash unconditionally, even when there is no such user, so a wrong username
    // and a wrong password cost the same. Otherwise the 60000-round hash makes
    // "this name exists" trivially measurable and turns the list into an oracle.
    static const char dummySalt[SHA256_HEX_SIZE] =
        "0000000000000000000000000000000000000000000000000000000000000000";

    char computed[SHA256_HEX_SIZE];
    SHA256_HashPassword(password, admin ? admin->salt : dummySalt, computed);

    bool ok = admin != NULL && SHA256_HexEquals(computed, admin->hash);

    // A record carrying a guid is BOUND to it: the password alone is not enough
    // from another identity. This can only ever narrow who may log in, so a
    // spoofed guid gains nothing -- it still needs the password, and a real
    // admin whose guid is being impersonated is refused rather than impersonated.
    if (ok && admin->guid[0] && I_stricmp(admin->guid, cl->cdkeyHash))
    {
        Com_Printf(15, "Login for '%s' refused: bound to a different guid.\n", admin->username);
        ok = false;
    }

    memset(computed, 0, sizeof(computed));

    if (!ok)
    {
        if (++g_authAttempts[clientNum] >= AUTH_MAX_ATTEMPTS)
        {
            g_authLockoutUntil[clientNum] = now + AUTH_LOCKOUT_MSEC;
            Com_PrintWarning(15, "Client %i locked out after %i failed admin logins.\n",
                clientNum, g_authAttempts[clientNum]);
        }
        return 0;
    }

    g_authAttempts[clientNum] = 0;
    g_authLockoutUntil[clientNum] = 0;

    cl->authPower = admin->power;
    I_strncpyz(cl->authName, admin->username, sizeof(cl->authName));

    Com_Printf(15, "Client %i authenticated as '%s' with power %i.\n",
        clientNum, admin->username, admin->power);

    return admin->power;
}

void __cdecl Auth_Logout(client_t *cl)
{
    const int32_t clientNum = Auth_ClientNum(cl);
    if (clientNum < 0)
        return;

    cl->authPower = 0;
    cl->authName[0] = 0;
    g_authAttempts[clientNum] = 0;
    g_authLockoutUntil[clientNum] = 0;
}

int32_t __cdecl Auth_GetClPower(const client_t *cl)
{
    // No identity lookup here, and deliberately so -- see the header. The only
    // thing that can raise this above the default is Auth_Login.
    if (!cl || cl->authPower <= 0)
        return AUTH_POWER_DEFAULT;

    if (cl->authPower > AUTH_POWER_MAX)
        return AUTH_POWER_MAX;

    return cl->authPower;
}

const char *__cdecl Auth_GetClName(const client_t *cl)
{
    if (!cl || cl->authPower <= 0 || !cl->authName[0])
        return NULL;

    return cl->authName;
}

// ---------------------------------------------------------------------------
// List management
// ---------------------------------------------------------------------------

bool __cdecl Auth_AddAdmin(const char *username, const char *password, int32_t power, const char *guid)
{
    if (!username || !*username || !password || !*password)
        return false;

    if (power < AUTH_POWER_DEFAULT || power > AUTH_POWER_MAX)
    {
        Com_PrintError(15, "Power must be between %i and %i.\n", AUTH_POWER_DEFAULT, AUTH_POWER_MAX);
        return false;
    }
    if (strlen(username) >= AUTH_USERNAME_SIZE)
    {
        Com_PrintError(15, "Admin names are limited to %i characters.\n", AUTH_USERNAME_SIZE - 1);
        return false;
    }

    // Whitespace would break the one-record-per-line format on read-back, and a
    // leading comment character would make the record vanish on the next load.
    for (const char *c = username; *c; ++c)
    {
        if (*c == ' ' || *c == '\t' || *c == '\r' || *c == '\n')
        {
            Com_PrintError(15, "Admin names may not contain whitespace.\n");
            return false;
        }
    }
    if (username[0] == '#' || username[0] == '/')
    {
        Com_PrintError(15, "Admin names may not start with # or /.\n");
        return false;
    }

    if (!g_authLoaded)
        Auth_Load();

    authAdmin_t *admin = Auth_FindByName(username);

    if (!admin)
    {
        for (int32_t i = 0; i < MAX_AUTH_ADMINS; ++i)
        {
            if (!g_authAdmins[i].inUse)
            {
                admin = &g_authAdmins[i];
                break;
            }
        }
        if (!admin)
        {
            Com_PrintError(15, "The admin list is full (%i).\n", MAX_AUTH_ADMINS);
            return false;
        }
    }

    memset(admin, 0, sizeof(*admin));
    I_strncpyz(admin->username, username, sizeof(admin->username));
    SHA256_GenerateSalt(admin->salt);
    SHA256_HashPassword(password, admin->salt, admin->hash);
    admin->power = power;

    if (guid && *guid && strcmp(guid, "-"))
        I_strncpyz(admin->guid, guid, sizeof(admin->guid));

    admin->inUse = true;

    return Auth_Save();
}

bool __cdecl Auth_RemoveAdmin(const char *username)
{
    if (!g_authLoaded)
        Auth_Load();

    authAdmin_t *admin = Auth_FindByName(username);
    if (!admin)
        return false;

    memset(admin, 0, sizeof(*admin));
    return Auth_Save();
}

bool __cdecl Auth_ChangePassword(const char *username, const char *oldPassword, const char *newPassword)
{
    if (!newPassword || !*newPassword)
        return false;

    if (!g_authLoaded)
        Auth_Load();

    authAdmin_t *admin = Auth_FindByName(username);
    if (!admin)
        return false;

    // Changing a password still costs the old one. Without this, any path that
    // reaches here holding only a username is an account takeover.
    char computed[SHA256_HEX_SIZE];
    SHA256_HashPassword(oldPassword ? oldPassword : "", admin->salt, computed);

    const bool ok = SHA256_HexEquals(computed, admin->hash);
    memset(computed, 0, sizeof(computed));

    if (!ok)
        return false;

    // New salt as well as new hash, so the two passwords share nothing.
    SHA256_GenerateSalt(admin->salt);
    SHA256_HashPassword(newPassword, admin->salt, admin->hash);

    return Auth_Save();
}

// ---------------------------------------------------------------------------
// Per-command power
// ---------------------------------------------------------------------------

int32_t __cdecl Auth_GetCommandPower(const char *cmd)
{
    if (!cmd || !*cmd)
        return AUTH_POWER_MAX;

    for (int32_t i = 0; i < g_authCommandPowerCount; ++i)
    {
        if (!I_stricmp(g_authCommandPowers[i].name, cmd))
            return g_authCommandPowers[i].power;
    }

    // Unlisted means unrestricted. This gates the admin surface; it is not an
    // allowlist for the whole console, and treating an unknown command as
    // forbidden would silently break every command nobody thought to list.
    return AUTH_POWER_DEFAULT;
}

void __cdecl Auth_SetCommandPower(const char *cmd, int32_t power)
{
    if (!cmd || !*cmd)
        return;

    if (power < AUTH_POWER_DEFAULT)
        power = AUTH_POWER_DEFAULT;
    if (power > AUTH_POWER_MAX)
        power = AUTH_POWER_MAX;

    for (int32_t i = 0; i < g_authCommandPowerCount; ++i)
    {
        if (!I_stricmp(g_authCommandPowers[i].name, cmd))
        {
            g_authCommandPowers[i].power = power;
            return;
        }
    }

    if (g_authCommandPowerCount >= MAX_COMMAND_POWERS)
    {
        Com_PrintError(15, "Cannot restrict more than %i commands.\n", MAX_COMMAND_POWERS);
        return;
    }

    I_strncpyz(g_authCommandPowers[g_authCommandPowerCount].name, cmd,
        sizeof(g_authCommandPowers[0].name));
    g_authCommandPowers[g_authCommandPowerCount].power = power;
    ++g_authCommandPowerCount;
}

bool __cdecl Auth_CanUseCommand(const client_t *cl, const char *cmd)
{
    return Auth_GetClPower(cl) >= Auth_GetCommandPower(cmd);
}

void __cdecl Auth_ListCommandPowers()
{
    Com_Printf(15, "%-32s %s\n", "command", "min power");

    for (int32_t i = 0; i < g_authCommandPowerCount; ++i)
        Com_Printf(15, "%-32s %i\n", g_authCommandPowers[i].name, g_authCommandPowers[i].power);

    Com_Printf(15, "%i restricted command%s; anything not listed is unrestricted.\n",
        g_authCommandPowerCount, g_authCommandPowerCount == 1 ? "" : "s");
}

// ---------------------------------------------------------------------------
// Console commands
// ---------------------------------------------------------------------------

static void Auth_AddAdmin_f()
{
    if (Cmd_Argc() < 4)
    {
        Com_Printf(15, "Usage: AdminAddAdmin <name> <password> <power> [guid]\n");
        Com_Printf(15, "  power is %i..%i. A guid binds the account to that identity as well as\n",
            AUTH_POWER_DEFAULT, AUTH_POWER_MAX);
        Com_Printf(15, "  to the password; omit it to allow login from any identity.\n");
        Com_Printf(15, "  Re-running this for an existing name resets that admin's password.\n");
        return;
    }

    const char *guid = (Cmd_Argc() > 4) ? Cmd_Argv(4) : "";

    if (Auth_AddAdmin(Cmd_Argv(1), Cmd_Argv(2), atoi(Cmd_Argv(3)), guid))
        Com_Printf(15, "Admin '%s' saved.\n", Cmd_Argv(1));
    else
        Com_PrintError(15, "Could not add admin '%s'.\n", Cmd_Argv(1));
}

static void Auth_RemoveAdmin_f()
{
    if (Cmd_Argc() != 2)
    {
        Com_Printf(15, "Usage: AdminRemoveAdmin <name>\n");
        return;
    }

    if (Auth_RemoveAdmin(Cmd_Argv(1)))
        Com_Printf(15, "Admin '%s' removed.\n", Cmd_Argv(1));
    else
        Com_PrintError(15, "No admin named '%s'.\n", Cmd_Argv(1));
}

static void Auth_ListAdmins_f()
{
    if (!g_authLoaded)
        Auth_Load();

    Com_Printf(15, "%-32s %-6s %s\n", "name", "power", "bound guid");

    int32_t count = 0;
    for (int32_t i = 0; i < MAX_AUTH_ADMINS; ++i)
    {
        if (!g_authAdmins[i].inUse)
            continue;

        Com_Printf(15, "%-32s %-6i %s\n", g_authAdmins[i].username, g_authAdmins[i].power,
            g_authAdmins[i].guid[0] ? g_authAdmins[i].guid : "(any)");
        ++count;
    }

    Com_Printf(15, "%i admin%s.\n", count, count == 1 ? "" : "s");
}

static void Auth_SetCommandPower_f()
{
    if (Cmd_Argc() != 3)
    {
        Com_Printf(15, "Usage: AdminSetCommandPower <command> <power>\n");
        return;
    }

    Auth_SetCommandPower(Cmd_Argv(1), atoi(Cmd_Argv(2)));
    Com_Printf(15, "'%s' now needs power %i.\n", Cmd_Argv(1), Auth_GetCommandPower(Cmd_Argv(1)));
}

static void Auth_ListCommandPowers_f()
{
    Auth_ListCommandPowers();
}

static void Auth_Reload_f()
{
    Auth_Load();
}

static cmd_function_s Auth_AddAdmin_f_VAR;
static cmd_function_s Auth_AddAdmin_f_VAR_SERVER;
static cmd_function_s Auth_RemoveAdmin_f_VAR;
static cmd_function_s Auth_RemoveAdmin_f_VAR_SERVER;
static cmd_function_s Auth_ListAdmins_f_VAR;
static cmd_function_s Auth_ListAdmins_f_VAR_SERVER;
static cmd_function_s Auth_SetCommandPower_f_VAR;
static cmd_function_s Auth_SetCommandPower_f_VAR_SERVER;
static cmd_function_s Auth_ListCommandPowers_f_VAR;
static cmd_function_s Auth_ListCommandPowers_f_VAR_SERVER;
static cmd_function_s Auth_Reload_f_VAR;
static cmd_function_s Auth_Reload_f_VAR_SERVER;

void __cdecl Auth_RegisterCommands()
{
    Cmd_AddCommandInternal("AdminAddAdmin", Cbuf_AddServerText_f, &Auth_AddAdmin_f_VAR);
    Cmd_AddServerCommandInternal("AdminAddAdmin", Auth_AddAdmin_f, &Auth_AddAdmin_f_VAR_SERVER);

    Cmd_AddCommandInternal("AdminRemoveAdmin", Cbuf_AddServerText_f, &Auth_RemoveAdmin_f_VAR);
    Cmd_AddServerCommandInternal("AdminRemoveAdmin", Auth_RemoveAdmin_f, &Auth_RemoveAdmin_f_VAR_SERVER);

    Cmd_AddCommandInternal("AdminListAdmins", Cbuf_AddServerText_f, &Auth_ListAdmins_f_VAR);
    Cmd_AddServerCommandInternal("AdminListAdmins", Auth_ListAdmins_f, &Auth_ListAdmins_f_VAR_SERVER);

    Cmd_AddCommandInternal("AdminSetCommandPower", Cbuf_AddServerText_f, &Auth_SetCommandPower_f_VAR);
    Cmd_AddServerCommandInternal("AdminSetCommandPower", Auth_SetCommandPower_f, &Auth_SetCommandPower_f_VAR_SERVER);

    Cmd_AddCommandInternal("AdminListCommands", Cbuf_AddServerText_f, &Auth_ListCommandPowers_f_VAR);
    Cmd_AddServerCommandInternal("AdminListCommands", Auth_ListCommandPowers_f, &Auth_ListCommandPowers_f_VAR_SERVER);

    Cmd_AddCommandInternal("AdminReload", Cbuf_AddServerText_f, &Auth_Reload_f_VAR);
    Cmd_AddServerCommandInternal("AdminReload", Auth_Reload_f, &Auth_Reload_f_VAR_SERVER);
}

void __cdecl Auth_Init()
{
    memset(g_authAdmins, 0, sizeof(g_authAdmins));
    memset(g_authCommandPowers, 0, sizeof(g_authCommandPowers));
    memset(g_authAttempts, 0, sizeof(g_authAttempts));
    memset(g_authLockoutUntil, 0, sizeof(g_authLockoutUntil));
    g_authCommandPowerCount = 0;
    g_authLoaded = false;

    Auth_RegisterCommands();
    Auth_Load();
}

void __cdecl Auth_Shutdown()
{
    // The hashes are not secrets, but the list is still not something to leave
    // sitting in freed memory for the next allocation to inherit.
    memset(g_authAdmins, 0, sizeof(g_authAdmins));
    g_authLoaded = false;
}
