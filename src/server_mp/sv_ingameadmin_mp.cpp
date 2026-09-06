#ifndef KISAK_MP
#error This File is MultiPlayer Only
#endif

#include "sv_ingameadmin_mp.h"
#include "sv_auth_mp.h"
#include "server_mp.h"

#include <qcommon/qcommon.h>
#include <qcommon/cmd.h>

#include <string.h>
#include <stdarg.h>
#include <time.h>

// Console output is redirected into this while an admin command runs, then
// chunked out to the client that invoked it.
#define SV_ADMIN_OUTPUT_SIZE 1024

// The chunker's payload limit. A server command carries more than this, but the
// text is wrapped in quotes and goes out reliably alongside everything else the
// snapshot needs, so CoD4x kept it well short of the real ceiling and so does
// this. Raising it risks overflowing the reliable command buffer on a busy
// server, which drops the client rather than truncating the message.
#define SV_ADMIN_CHUNK_SIZE 240

// Longest command word we will consider. Anything longer is not a command we
// have, and refusing early keeps the rest of the parsing bounded.
#define SV_ADMIN_MAX_CMD_LEN 30

// The whole command line, after separator stripping.
#define SV_ADMIN_MAX_LINE 256

static client_t *g_adminRedirectClient;

static int32_t g_invokerClientNum = -1;
static int32_t g_invokerPower;
static char    g_invokerName[36];
static char    g_invokerGuid[33];

// ---------------------------------------------------------------------------
// Invoker context
// ---------------------------------------------------------------------------

int32_t __cdecl Cmd_GetInvokerClnum()
{
    return g_invokerClientNum;
}

int32_t __cdecl Cmd_GetInvokerPower()
{
    // The console is not a player and is not power limited. Anything reading
    // this to decide whether to allow something must treat -1 as full authority,
    // which is what AUTH_POWER_MAX gives it.
    if (g_invokerClientNum < 0 || g_invokerClientNum >= MAX_CLIENTS)
        return AUTH_POWER_MAX;

    // LIVE, not the value captured at dispatch. The captured one is stale the
    // moment a command changes the invoker's own power, and login is exactly
    // that command: the first live test logged "admin kisaktest (power 1):
    // logged in" for a login that granted 100, because the snapshot predated
    // the grant. An audit line that under-reports the power an action ran with
    // is worse than no number at all.
    //
    // g_invokerPower is still kept, as the power the dispatcher authorised
    // against, so a later de-escalation cannot retroactively justify a command
    // that was already allowed.
    return Auth_GetClPower(&svs.clients[g_invokerClientNum]);
}

const char *__cdecl Cmd_GetInvokerName()
{
    if (g_invokerClientNum < 0)
        return "console";

    return g_invokerName;
}

const char *__cdecl Cmd_GetInvokerGuid()
{
    return g_invokerGuid;
}

void __cdecl Cmd_SetCurrentInvokerInfo(int32_t clientNum, int32_t power, const char *name, const char *guid)
{
    g_invokerClientNum = clientNum;
    g_invokerPower = power;
    I_strncpyz(g_invokerName, name ? name : "", sizeof(g_invokerName));
    I_strncpyz(g_invokerGuid, guid ? guid : "", sizeof(g_invokerGuid));
}

void __cdecl Cmd_ClearCurrentInvokerInfo()
{
    g_invokerClientNum = -1;
    g_invokerPower = 0;
    g_invokerName[0] = 0;
    g_invokerGuid[0] = 0;
}

// ---------------------------------------------------------------------------
// Output redirection
// ---------------------------------------------------------------------------

static void SV_ReliableSendRedirect(char *sendbuf)
{
    if (!g_adminRedirectClient || !sendbuf)
        return;

    // CS_PRIMED. Below this the client has no command path to receive on yet.
    if (g_adminRedirectClient->header.state < 4)
        return;

    char outputbuf[SV_ADMIN_CHUNK_SIZE + 4];
    int remaining = (int)strlen(sendbuf);

    while (remaining > 0)
    {
        int maxlength = remaining;
        if (maxlength > SV_ADMIN_CHUNK_SIZE)
            maxlength = SV_ADMIN_CHUNK_SIZE;

        int lastlinebreak = 0;
        int i = 0;

        for (; i < maxlength; ++i)
        {
            outputbuf[i] = sendbuf[i];

            // The text travels inside a quoted server command, so an embedded
            // quote would end the argument early and the rest would be parsed
            // as further arguments.
            if (outputbuf[i] == '"')
                outputbuf[i] = '\'';

            if (outputbuf[i] == '\n')
                lastlinebreak = i;
        }

        if (lastlinebreak > 0)
        {
            // Break on the last newline that fits, so wrapped output stays
            // readable instead of splitting mid-line.
            i = lastlinebreak;
            remaining -= i + 1;
            sendbuf += i + 1;
            outputbuf[i] = 0;
        }
        else
        {
            remaining -= i;
            sendbuf += i;
            outputbuf[i] = 0;
        }

        SV_SendServerCommand(g_adminRedirectClient, SV_CMD_RELIABLE, "%c \"%s\"", 101, outputbuf);
    }
}

// ---------------------------------------------------------------------------
// The dispatcher
// ---------------------------------------------------------------------------

bool __cdecl SV_ExecuteRemoteCmd(int32_t clientNum, const char *msg)
{
    if (clientNum < 0 || clientNum >= MAX_CLIENTS || !msg)
        return false;

    client_t *cl = &svs.clients[clientNum];
    if (cl->header.state < 4)
        return false;

    // Isolate the command word. Anything absurdly long or absurdly short is not
    // one of ours; fall through so it is treated as chat rather than eaten.
    int len = 0;
    while (msg[len] && msg[len] != ' ' && msg[len] != '\n' && len < SV_ADMIN_MAX_CMD_LEN + 2)
        ++len;

    if (len < 2 || len > SV_ADMIN_MAX_CMD_LEN - 1)
        return false;

    char cmd[SV_ADMIN_MAX_CMD_LEN];
    I_strncpyz(cmd, msg, len + 1);

    // Strip command separators BEFORE anything executes. Without this, a player
    // allowed to run one command can append a second after a ';' and run
    // anything at all. This is the single most important line in the file.
    char buffer[SV_ADMIN_MAX_LINE];
    I_strncpyz(buffer, msg, sizeof(buffer));

    for (char *c = buffer; *c; ++c)
    {
        if (*c == ';' || *c == '\n' || *c == '\r')
            *c = 0;
    }

    const int32_t power = Auth_GetClPower(cl);
    const int32_t needed = Auth_GetCommandPower(cmd);

    // Never echo a line that carries a secret. login and every *password command
    // put the password in argv, and this string is printed to the server console
    // and sent back to the client.
    const bool secret = I_stristr(cmd, "password") != NULL || !I_stricmp(cmd, "login");
    const char *printable = secret ? "(hidden)" : buffer;

    if (!Cmd_FindCommand(cmd))
    {
        SV_SendServerCommand(cl, SV_CMD_RELIABLE,
            "%c \"^5Command^2: %s\n^3Unknown command. Type ^2$AdminListCommands ^3for the restricted ones.\"",
            101, printable);
        return true;
    }

    if (power < needed)
    {
        SV_SendServerCommand(cl, SV_CMD_RELIABLE,
            "%c \"^5Command^2: %s\n^3You need power %i to run this; you have %i.\"",
            101, printable, needed, power);
        return true;
    }

    Com_Printf(15, "Admin command: %s  by: %s (slot %i, guid %s, power %i)\n",
        printable, cl->name, clientNum, cl->cdkeyHash, power);

    // Save and restore rather than clear: a command can be invoked from inside
    // another one, and the outer invoker has to survive the inner call.
    const int32_t prevClientNum = g_invokerClientNum;
    const int32_t prevPower = g_invokerPower;
    char prevName[sizeof(g_invokerName)];
    char prevGuid[sizeof(g_invokerGuid)];
    I_strncpyz(prevName, g_invokerName, sizeof(prevName));
    I_strncpyz(prevGuid, g_invokerGuid, sizeof(prevGuid));

    const char *authName = Auth_GetClName(cl);
    Cmd_SetCurrentInvokerInfo(clientNum, power, authName ? authName : cl->name, cl->cdkeyHash);

    char outputbuf[SV_ADMIN_OUTPUT_SIZE];
    g_adminRedirectClient = cl;
    Com_BeginRedirect(outputbuf, sizeof(outputbuf), SV_ReliableSendRedirect);

    Cmd_ExecuteSingleCommand(0, 0, buffer);

    Com_EndRedirect();
    g_adminRedirectClient = NULL;

    Cmd_SetCurrentInvokerInfo(prevClientNum, prevPower, prevName, prevGuid);

    return true;
}

bool __cdecl SV_HandleChatCommand(int32_t clientNum, const char *chatText)
{
    if (!chatText)
        return false;

    const char *text = chatText;

    // Chat arrives with a leading 0x15 marker byte in some paths.
    if (*text == 0x15)
        ++text;

    if (*text != '$' && *text != '!' && *text != '/')
        return false;

    ++text;

    if (!*text)
        return false;

    return SV_ExecuteRemoteCmd(clientNum, text);
}

// ---------------------------------------------------------------------------
// Audit log
// ---------------------------------------------------------------------------

void __cdecl SV_PrintAdministrativeLog(const char *fmt, ...)
{
    char message[1024];
    va_list argptr;

    va_start(argptr, fmt);
    _vsnprintf(message, sizeof(message) - 1, fmt, argptr);
    message[sizeof(message) - 1] = 0;
    va_end(argptr);

    const time_t now = time(NULL);
    char stamp[64];

    struct tm local;
    if (!localtime_s(&local, &now) && strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local))
    {
        // ok
    }
    else
    {
        I_strncpyz(stamp, "unknown time", sizeof(stamp));
    }

    // Step around SV_ExecuteRemoteCmd's redirect. Without this the audit line is
    // delivered to the player who triggered it and to nobody else, which is
    // exactly backwards -- they already saw the result; the SERVER needs the
    // record. Caught by the first live login leaving no trace in the console log
    // despite succeeding.
    ComRedirectState saved;
    Com_SuspendRedirect(&saved);

    Com_Printf(15, "%s - admin %s (guid %s, power %i): %s\n",
        stamp, Cmd_GetInvokerName(), Cmd_GetInvokerGuid()[0] ? Cmd_GetInvokerGuid() : "-",
        Cmd_GetInvokerPower(), message);

    Com_ResumeRedirect(&saved);
}

// ---------------------------------------------------------------------------
// Player-facing commands
// ---------------------------------------------------------------------------

static void SV_AdminLogin_f()
{
    const int32_t clientNum = Cmd_GetInvokerClnum();

    if (clientNum < 0)
    {
        Com_Printf(15, "login is for players; the console already has full power.\n");
        return;
    }
    if (Cmd_Argc() != 3)
    {
        Com_Printf(15, "Usage: login <name> <password>\n");
        return;
    }

    client_t *cl = &svs.clients[clientNum];

    if (Auth_GetClPower(cl) > AUTH_POWER_DEFAULT)
    {
        Com_Printf(15, "You are already logged in as '%s'.\n", Auth_GetClName(cl));
        return;
    }

    const int32_t power = Auth_Login(cl, Cmd_Argv(1), Cmd_Argv(2));

    if (power <= 0)
    {
        // One message for both failures. Saying which of the name or the
        // password was wrong turns this into a way to enumerate admin names.
        Com_Printf(15, "^1Login failed.\n");
        return;
    }

    Com_Printf(15, "^2Logged in as '%s' with power %i.\n", Auth_GetClName(cl), power);
    SV_PrintAdministrativeLog("logged in from slot %i", clientNum);
}

static void SV_AdminLogout_f()
{
    const int32_t clientNum = Cmd_GetInvokerClnum();

    if (clientNum < 0)
    {
        Com_Printf(15, "logout is for players.\n");
        return;
    }

    client_t *cl = &svs.clients[clientNum];

    if (Auth_GetClPower(cl) <= AUTH_POWER_DEFAULT)
    {
        Com_Printf(15, "You are not logged in.\n");
        return;
    }

    SV_PrintAdministrativeLog("logged out");
    Auth_Logout(cl);
    Com_Printf(15, "Logged out.\n");
}

static void SV_AdminChangePassword_f()
{
    const int32_t clientNum = Cmd_GetInvokerClnum();

    if (Cmd_Argc() != 3)
    {
        Com_Printf(15, "Usage: changePassword <oldPassword> <newPassword>\n");
        return;
    }
    if (clientNum < 0)
    {
        Com_Printf(15, "changePassword changes the password of the logged-in admin who runs it.\n");
        Com_Printf(15, "From the console, use AdminAddAdmin <name> <newpassword> <power> instead.\n");
        return;
    }

    client_t *cl = &svs.clients[clientNum];
    const char *name = Auth_GetClName(cl);

    if (!name)
    {
        Com_Printf(15, "You must be logged in to change your password.\n");
        return;
    }

    if (!Auth_ChangePassword(name, Cmd_Argv(1), Cmd_Argv(2)))
    {
        Com_Printf(15, "^1Password not changed: the old password did not match.\n");
        return;
    }

    Com_Printf(15, "^2Password changed.\n");
    SV_PrintAdministrativeLog("changed their own password");
}

static void SV_AdminWhoAmI_f()
{
    const int32_t clientNum = Cmd_GetInvokerClnum();

    if (clientNum < 0)
    {
        Com_Printf(15, "console, power %i\n", AUTH_POWER_MAX);
        return;
    }

    const client_t *cl = &svs.clients[clientNum];
    const char *name = Auth_GetClName(cl);

    Com_Printf(15, "slot %i, guid %s, power %i, logged in as %s\n",
        clientNum, cl->cdkeyHash, Auth_GetClPower(cl), name ? name : "(nobody)");
}

void __cdecl SV_InGameAdmin_ClientDisconnect(client_t *cl)
{
    // Power must not outlive the connection: slots are reused, and a new player
    // landing in a slot an admin just left must not inherit anything.
    Auth_Logout(cl);
}

static cmd_function_s SV_AdminLogin_f_VAR;
static cmd_function_s SV_AdminLogin_f_VAR_SERVER;
static cmd_function_s SV_AdminLogout_f_VAR;
static cmd_function_s SV_AdminLogout_f_VAR_SERVER;
static cmd_function_s SV_AdminChangePassword_f_VAR;
static cmd_function_s SV_AdminChangePassword_f_VAR_SERVER;
static cmd_function_s SV_AdminWhoAmI_f_VAR;
static cmd_function_s SV_AdminWhoAmI_f_VAR_SERVER;

void __cdecl SV_InGameAdmin_AddCommands()
{
    Cmd_AddCommandInternal("login", Cbuf_AddServerText_f, &SV_AdminLogin_f_VAR);
    Cmd_AddServerCommandInternal("login", SV_AdminLogin_f, &SV_AdminLogin_f_VAR_SERVER);

    Cmd_AddCommandInternal("logout", Cbuf_AddServerText_f, &SV_AdminLogout_f_VAR);
    Cmd_AddServerCommandInternal("logout", SV_AdminLogout_f, &SV_AdminLogout_f_VAR_SERVER);

    Cmd_AddCommandInternal("changePassword", Cbuf_AddServerText_f, &SV_AdminChangePassword_f_VAR);
    Cmd_AddServerCommandInternal("changePassword", SV_AdminChangePassword_f, &SV_AdminChangePassword_f_VAR_SERVER);

    Cmd_AddCommandInternal("whoami", Cbuf_AddServerText_f, &SV_AdminWhoAmI_f_VAR);
    Cmd_AddServerCommandInternal("whoami", SV_AdminWhoAmI_f, &SV_AdminWhoAmI_f_VAR_SERVER);

    // Sensible defaults for the commands that can actually hurt. Everything else
    // stays unrestricted until an admin says otherwise with AdminSetCommandPower.
    Auth_SetCommandPower("kick", 50);
    Auth_SetCommandPower("clientkick", 50);
    Auth_SetCommandPower("banUser", 80);
    Auth_SetCommandPower("banClient", 80);
    Auth_SetCommandPower("onlykick", 50);
    Auth_SetCommandPower("mute", 40);
    Auth_SetCommandPower("unmute", 40);
    Auth_SetCommandPower("map", 60);
    Auth_SetCommandPower("map_rotate", 60);
    Auth_SetCommandPower("map_restart", 40);
    Auth_SetCommandPower("fast_restart", 40);
    Auth_SetCommandPower("quit", 100);
    Auth_SetCommandPower("killserver", 100);

    // The admin list itself is the crown jewels: anyone who can add an admin can
    // grant themselves anything, so this is owner-only.
    Auth_SetCommandPower("AdminAddAdmin", 100);
    Auth_SetCommandPower("AdminRemoveAdmin", 100);
    Auth_SetCommandPower("AdminSetCommandPower", 100);
    Auth_SetCommandPower("AdminReload", 100);
    Auth_SetCommandPower("AdminListAdmins", 80);

    // rcon_password and friends would hand out the server outright.
    Auth_SetCommandPower("rcon", 100);
    Auth_SetCommandPower("set", 100);
    Auth_SetCommandPower("seta", 100);
    Auth_SetCommandPower("sets", 100);
    Auth_SetCommandPower("setu", 100);
    Auth_SetCommandPower("exec", 100);
}
