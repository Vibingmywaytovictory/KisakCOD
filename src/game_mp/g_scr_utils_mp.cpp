// Portions of this file are derived from CoD4x_Server
//   https://github.com/callofduty4x/CoD4x_Server
//   Copyright (C) 2010-2013 Ninja and TheKelm
//   Copyright (C) 1999-2005 Id Software, Inc.
//   Copyright (C) the CoD4x authors
// Licensed under the GNU Affero General Public License v3 (see LICENSE.AGPLv3).
//
// Adapted to KisakCOD's engine APIs. The combined work is conveyed under
// GPLv3 section 13; see LICENSING.md.

#ifndef KISAK_MP
#error This File is MultiPlayer Only
#endif

#include "g_scr_utils_mp.h"

#include "g_public_mp.h"
#include "g_scr_builtins_mp.h"

#include <qcommon/qcommon.h>
#include <qcommon/cmd.h>
#include <script/scr_variable.h>
#include <script/scr_main.h>
#include <server_mp/server_mp.h>
#include <universal/base64.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Every builtin below writes its result through a buffer of this size. CoD4x
// sizes the equivalents at 1024 or 2048 and, in several cases, neither bounds
// the write nor terminates the buffer -- StrColorStrip in particular hands
// Scr_AddString a stack buffer with no NUL on it. One cap, applied everywhere,
// and every path terminates.
//
// Script strings arrive from the VM, which is fed by mod authors and, through
// dvars and userinfo, by players. Truncation is the right failure here: a
// script error would abort a thread over a long name.
#define SCR_UTIL_STRLEN 2048

// ---------------------------------------------------------------------------
// String helpers
// ---------------------------------------------------------------------------

// string = toupper( <string> )
void __cdecl GScr_ToUpper()
{
    char buffer[SCR_UTIL_STRLEN];

    if (Scr_GetNumParam() != 1)
    {
        Scr_Error("Usage: string = toUpper( <string> );");
        return;
    }

    I_strncpyz(buffer, Scr_GetString(0), sizeof(buffer));
    I_strupr(buffer);

    Scr_AddString(buffer);
}

// string = strcolorstrip( <string> )
//
// Removes ^N colour escapes. CoD4x drops the '^' and the digit but never
// terminates the output, so what reaches the string list is whatever happened
// to follow on the stack.
void __cdecl GScr_StrColorStrip()
{
    char buffer[SCR_UTIL_STRLEN];

    if (Scr_GetNumParam() != 1)
    {
        Scr_Error("Usage: string = strColorStrip( <string> );");
        return;
    }

    const char *string = Scr_GetString(0);
    uint32_t out = 0;

    for (uint32_t in = 0; string[in] && out < sizeof(buffer) - 1; ++in)
    {
        if (string[in] == '^' && string[in + 1] >= '0' && string[in + 1] <= '9')
        {
            ++in;
            continue;
        }

        buffer[out++] = string[in];
    }

    buffer[out] = 0;

    Scr_AddString(buffer);
}

// string = strctrlstrip( <string> )
//
// Drops every control character, which is how a name or a chat line smuggles a
// newline into the log or a carriage return into a configstring.
void __cdecl GScr_StrCtrlStrip()
{
    char buffer[SCR_UTIL_STRLEN];

    if (Scr_GetNumParam() != 1)
    {
        Scr_Error("Usage: string = strCtrlStrip( <string> );");
        return;
    }

    const char *string = Scr_GetString(0);
    uint32_t out = 0;

    for (uint32_t in = 0; string[in] && out < sizeof(buffer) - 1; ++in)
    {
        if ((uint8_t)string[in] >= 0x20)
            buffer[out++] = string[in];
    }

    buffer[out] = 0;

    Scr_AddString(buffer);
}

// string = strrepl( <string>, <find>, <replacement> )
//
// CoD4x calls Q_strnrepl, which this tree does not have; the substitution is
// written out here rather than adding a string primitive for one caller.
void __cdecl GScr_StrRepl()
{
    char buffer[SCR_UTIL_STRLEN];

    if (Scr_GetNumParam() != 3)
    {
        Scr_Error("Usage: string = strRepl( <string>, <find>, <replacement> );");
        return;
    }

    const char *string = Scr_GetString(0);
    const char *find = Scr_GetString(1);
    const char *replacement = Scr_GetString(2);

    const size_t findLen = strlen(find);
    const size_t replLen = strlen(replacement);
    uint32_t out = 0;

    // An empty needle would match at every position and never advance.
    if (findLen == 0)
    {
        I_strncpyz(buffer, string, sizeof(buffer));
        Scr_AddString(buffer);
        return;
    }

    while (*string && out < sizeof(buffer) - 1)
    {
        if (!strncmp(string, find, findLen))
        {
            const size_t room = sizeof(buffer) - 1 - out;
            const size_t copy = replLen < room ? replLen : room;

            memcpy(buffer + out, replacement, copy);
            out += (uint32_t)copy;
            string += findLen;
            continue;
        }

        buffer[out++] = *string++;
    }

    buffer[out] = 0;

    Scr_AddString(buffer);
}

// Substitutes one && argument reference for strreplace below.
static uint32_t G_ScrUtil_FetchArg(char digit, char *out, uint32_t room)
{
    if (digit < '1' || digit > '9')
    {
        Scr_ParamError(0, "strReplace expects a digit 1-9 after &&");
        return 0;
    }

    const uint32_t arg = (uint32_t)(digit - '0');

    if (Scr_GetNumParam() < arg + 1)
    {
        Scr_ParamError(0, "strReplace expects as many extra arguments as the highest && reference");
        return 0;
    }

    const char *value = Scr_GetString(arg);
    const size_t len = strlen(value);
    const size_t copy = len < room ? len : room;

    memcpy(out, value, copy);
    return (uint32_t)copy;
}

// string = strreplace( <format>, <arg1>, ... )
//
// && followed by 1-9 is replaced with that argument; a backslash before an &
// escapes it.
//
// CoD4x computes the room left as sizeof(buf) - (pos + 1) without first
// checking that pos is still inside the buffer, so once the assembly buffer
// fills, that subtraction wraps and the next argument is copied with a length
// near SIZE_MAX. The loop condition here keeps out below the cap, so the
// subtraction cannot go negative.
void __cdecl GScr_StrReplace()
{
    char buffer[SCR_UTIL_STRLEN];

    if (Scr_GetNumParam() < 1)
    {
        Scr_Error("Usage: string = strReplace( <string>, <string>, ... );");
        return;
    }

    const char *string = Scr_GetString(0);
    uint32_t out = 0;

    while (*string && out < sizeof(buffer) - 1)
    {
        if (string[0] == '&' && string[1] == '&' && string[2] != 0)
        {
            out += G_ScrUtil_FetchArg(string[2], buffer + out, sizeof(buffer) - 1 - out);
            string += 3;
            continue;
        }

        if (string[0] == '\\' && string[1] == '&')
            ++string;

        buffer[out++] = *string++;
    }

    buffer[out] = 0;

    Scr_AddString(buffer);
}

// Colour code in effect at the end of a run of text, for carrying across a
// line break.
static char G_ScrUtil_TrailingColor(const char *string, uint32_t len, char current)
{
    for (uint32_t i = 0; i + 1 < len; ++i)
    {
        if (string[i] == '^' && string[i + 1] >= '0' && string[i + 1] <= '9')
        {
            current = string[i + 1];
            ++i;
        }
    }

    return current;
}

// array = strtokbylen( <string>, <maxlen> )
//
// Splits into lines of at most maxlen visible characters, preferring to break
// on a space, and re-opens each line with the colour that was in effect where
// the previous one ended.
//
// CoD4x's version indexes its output buffer with the *input* cursor while
// tracking capacity with a separate counter, so the two disagree the moment a
// colour code or a line break appears. Same script-visible contract, written
// against one cursor.
void __cdecl GScr_StrTokByLen()
{
    // Enough for any chat or hud line. A script asking for more pieces than
    // this is looping on bad input rather than formatting text.
    const uint32_t maxLines = 128;

    if (Scr_GetNumParam() != 2)
    {
        Scr_Error("Usage: array = strTokByLen( <string>, <maxlen> );");
        return;
    }

    const char *string = Scr_GetString(0);
    const int32_t limit = Scr_GetInt(1);

    if (limit < 1)
    {
        Scr_ParamError(1, "strTokByLen: maxlen must be at least 1");
        return;
    }

    char color = '7';
    uint32_t lines = 0;
    uint32_t pos = 0;

    Scr_MakeArray();

    while (string[pos] && lines < maxLines)
    {
        char line[SCR_UTIL_STRLEN];
        uint32_t out = 0;
        int32_t visible = 0;
        uint32_t breakPos = 0;   // input offset just past the last space
        uint32_t breakOut = 0;   // matching output length

        // Every line after the first re-states the colour, otherwise a break
        // resets the client back to white mid-sentence.
        if (lines != 0)
        {
            line[out++] = '^';
            line[out++] = color;
        }

        while (string[pos] && visible < limit && out < sizeof(line) - 3)
        {
            if (string[pos] == '^' && string[pos + 1] >= '0' && string[pos + 1] <= '9')
            {
                line[out++] = string[pos++];
                line[out++] = string[pos++];
                continue;
            }

            if (string[pos] == ' ')
            {
                breakPos = pos + 1;
                breakOut = out + 1;
            }

            line[out++] = string[pos++];
            ++visible;
        }

        // Rewind to the last space, but only when there is more text to place:
        // breaking on the final line would just drop the tail.
        if (string[pos] && breakOut != 0)
        {
            out = breakOut;
            pos = breakPos;
        }

        line[out] = 0;
        color = G_ScrUtil_TrailingColor(line, out, color);

        Scr_AddString(line);
        Scr_AddArray();
        ++lines;
    }
}

// ---------------------------------------------------------------------------
// Maths
// ---------------------------------------------------------------------------

// float = pow( <base>, <exponent> )
void __cdecl GScr_Pow()
{
    if (Scr_GetNumParam() != 2)
    {
        Scr_Error("Usage: float = pow( <base>, <exponent> );");
        return;
    }

    Scr_AddFloat(powf(Scr_GetFloat(0), Scr_GetFloat(1)));
}

// float = float( <value> )
//
// The counterpart to the stock int(). An unparseable string yields 0, which is
// CoD4x's behaviour; unlike CoD4x this also accepts a leading sign, a leading
// dot and leading whitespace, since strtod already handles them.
void __cdecl GScr_Float()
{
    if (Scr_GetNumParam() != 1)
    {
        Scr_Error("Usage: float = float( <float, int or string> );");
        return;
    }

    const int32_t type = Scr_GetType(0);

    switch (type)
    {
    case VAR_FLOAT:
        Scr_AddFloat(Scr_GetFloat(0));
        break;

    case VAR_INTEGER:
        Scr_AddFloat((float)Scr_GetInt(0));
        break;

    case VAR_STRING:
    {
        const char *string = Scr_GetString(0);
        char *end = nullptr;
        const double value = strtod(string, &end);

        Scr_AddFloat(end == string ? 0.0f : (float)value);
        break;
    }

    default:
        Scr_ParamError(0, va("cannot cast %s to float", var_typename[type]));
        break;
    }
}

// vector = vectoradd( <vector>, <x>, <y>, <z> )
void __cdecl GScr_VectorAdd()
{
    float vec[3];

    if (Scr_GetNumParam() != 4)
    {
        Scr_Error("Usage: vector = vectorAdd( <vector>, <x>, <y>, <z> );");
        return;
    }

    Scr_GetVector(0, vec);

    vec[0] += Scr_GetFloat(1);
    vec[1] += Scr_GetFloat(2);
    vec[2] += Scr_GetFloat(3);

    Scr_AddVector(vec);
}

// ---------------------------------------------------------------------------
// Type predicates
// ---------------------------------------------------------------------------

// An entity, an array and a struct all report VAR_POINTER from Scr_GetType;
// what separates them is the pointer type. CoD4x compares Scr_GetType against
// VAR_ENTITY and VAR_ARRAY directly, which in this VM is never true -- its
// isentity() would answer false for every entity. GScr_IsArray in
// game/g_scr_main.cpp already does this correctly and is registered as it is.
static bool G_ScrUtil_IsPointerTo(uint32_t index, int32_t pointerType)
{
    if (Scr_GetType(index) != VAR_POINTER)
        return false;

    return (int32_t)Scr_GetPointerType(index) == pointerType;
}

// bool = isarray( <value> )
//
// game/g_scr_main.cpp has an identical check, but that file is only compiled
// into the single-player target, so multiplayer needs its own.
void __cdecl GScr_IsArrayValue()
{
    if (Scr_GetNumParam() != 1)
    {
        Scr_Error("Usage: bool = isArray( <value> );");
        return;
    }

    Scr_AddBool(G_ScrUtil_IsPointerTo(0, VAR_ARRAY));
}

// bool = isentity( <value> )
void __cdecl GScr_IsEntityValue()
{
    if (Scr_GetNumParam() != 1)
    {
        Scr_Error("Usage: bool = isEntity( <value> );");
        return;
    }

    Scr_AddBool(G_ScrUtil_IsPointerTo(0, VAR_ENTITY));
}

// bool = isvector( <value> )
void __cdecl GScr_IsVectorValue()
{
    if (Scr_GetNumParam() != 1)
    {
        Scr_Error("Usage: bool = isVector( <value> );");
        return;
    }

    Scr_AddBool(Scr_GetType(0) == VAR_VECTOR);
}

// bool = isfloat( <value> )
void __cdecl GScr_IsFloatValue()
{
    if (Scr_GetNumParam() != 1)
    {
        Scr_Error("Usage: bool = isFloat( <value> );");
        return;
    }

    Scr_AddBool(Scr_GetType(0) == VAR_FLOAT);
}

// bool = isint( <value> )
void __cdecl GScr_IsIntValue()
{
    if (Scr_GetNumParam() != 1)
    {
        Scr_Error("Usage: bool = isInt( <value> );");
        return;
    }

    Scr_AddBool(Scr_GetType(0) == VAR_INTEGER);
}

// bool = isdvardefined( <name> )
//
// getdvar() on an unregistered dvar returns "", which a script cannot tell
// apart from a dvar that is registered and empty.
void __cdecl GScr_IsDvarDefined()
{
    if (Scr_GetNumParam() != 1)
    {
        Scr_Error("Usage: bool = isDvarDefined( <name> );");
        return;
    }

    Scr_AddBool(Dvar_FindVar(Scr_GetString(0)) != nullptr);
}

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------

// CoD4x reports seconds since 2012-01-01 rather than since the epoch, to keep
// the value clear of the signed 32 bit ceiling a GSC int carries. Scripts
// written against CoD4x do arithmetic on that number, so the offset is kept
// exactly as it is; timetostring below adds it back.
#define SCR_UTIL_TIME_EPOCH 1325376000

// int = getrealtime()
void __cdecl GScr_GetRealTime()
{
    if (Scr_GetNumParam() != 0)
    {
        Scr_Error("Usage: int = getRealTime();");
        return;
    }

    Scr_AddInt((int32_t)(_time64(nullptr) - SCR_UTIL_TIME_EPOCH));
}

// string = timetostring( <realtime>, <utc>, <format> )
void __cdecl GScr_TimeToString()
{
    char timestring[128];

    if (Scr_GetNumParam() != 3)
    {
        Scr_Error("Usage: string = timeToString( <realtime>, <utc>, <format> );");
        return;
    }

    __time64_t when = (__time64_t)Scr_GetInt(0) + SCR_UTIL_TIME_EPOCH;
    const int32_t utc = Scr_GetInt(1);
    const char *format = Scr_GetString(2);

    // Either conversion returns null for a timestamp the CRT cannot represent,
    // which a script reaches by passing a large negative realtime.
    struct tm *broken = utc ? _gmtime64(&when) : _localtime64(&when);

    if (!broken || strftime(timestring, sizeof(timestring), format, broken) == 0)
        timestring[0] = 0;

    Scr_AddString(timestring);
}

// ---------------------------------------------------------------------------
// Base64
// ---------------------------------------------------------------------------

// string = base64encode( <string> )
void __cdecl GScr_Base64Encode()
{
    char encoded[SCR_UTIL_STRLEN];

    if (Scr_GetNumParam() != 1)
    {
        Scr_Error("Usage: string = base64Encode( <string> );");
        return;
    }

    const char *input = Scr_GetString(0);
    const uint32_t inputLen = (uint32_t)strlen(input);

    // b64_encode writes b64e_size(n) bytes plus a NUL and takes no capacity of
    // its own, so the length has to be checked before the call rather than
    // clipped after it.
    if (b64e_size(inputLen) + 1 > sizeof(encoded))
    {
        Scr_ParamError(0, "base64Encode: input too long");
        return;
    }

    b64_encode((const unsigned char *)input, inputLen, (unsigned char *)encoded);
    encoded[sizeof(encoded) - 1] = 0;

    Scr_AddString(encoded);
}

// string = base64decode( <string> )
//
// A payload that decodes to bytes containing a NUL comes back cut at that
// byte: the script string list stores C strings, so this cannot carry binary.
void __cdecl GScr_Base64Decode()
{
    char decoded[SCR_UTIL_STRLEN];

    if (Scr_GetNumParam() != 1)
    {
        Scr_Error("Usage: string = base64Decode( <string> );");
        return;
    }

    const char *input = Scr_GetString(0);
    const uint32_t inputLen = (uint32_t)strlen(input);

    if (b64d_size(inputLen) + 1 > sizeof(decoded))
    {
        Scr_ParamError(0, "base64Decode: input too long");
        return;
    }

    const uint32_t decodedLen = b64_decode((const unsigned char *)input, inputLen, (unsigned char *)decoded);

    decoded[decodedLen < sizeof(decoded) ? decodedLen : sizeof(decoded) - 1] = 0;

    Scr_AddString(decoded);
}

// ---------------------------------------------------------------------------
// Console bridge
// ---------------------------------------------------------------------------

static const dvar_t *sv_scriptExec;

// Commands a script may not reach even when exec is otherwise allowed: they
// end the server or hand the caller the keys to it. CoD4x applies no filter at
// all, so on a CoD4x server any gametype script -- including one uploaded to a
// rented box by someone who is not the owner -- can run quit, or read back
// rcon_password through execEx.
static const char *const g_scrExecDeniedCommands[] =
{
    "quit",
    "killserver",
    "exec",
    "writeconfig",
};

// Dvars the set family may not touch from script, for the same reason.
static const char *const g_scrExecProtectedDvars[] =
{
    "rcon_password",
    "sv_privatePassword",
    "g_password",
    "fs_game",
    "fs_basepath",
    "fs_homepath",
    "fs_basegame",
};

// Pulls one whitespace-delimited token, honouring quotes the way the command
// tokenizer does. Written out rather than calling Cmd_TokenizeString, which
// would overwrite the arguments of the command currently executing -- and
// script frequently runs from inside one.
static const char *G_ScrUtil_NextToken(const char **data, char *token, uint32_t size)
{
    const char *in = *data;
    uint32_t out = 0;

    while (*in == ' ' || *in == '\t' || *in == '\n' || *in == '\r')
        ++in;

    if (!*in)
    {
        *data = in;
        token[0] = 0;
        return nullptr;
    }

    if (*in == '"')
    {
        ++in;
        while (*in && *in != '"')
        {
            if (out < size - 1)
                token[out++] = *in;
            ++in;
        }
        if (*in == '"')
            ++in;
    }
    else
    {
        while (*in && *in != ' ' && *in != '\t' && *in != '\n' && *in != '\r')
        {
            if (out < size - 1)
                token[out++] = *in;
            ++in;
        }
    }

    token[out] = 0;
    *data = in;
    return token;
}

// True if the command line may run. `command` is the whole line.
static bool G_ScrUtil_ExecAllowed(const char *command)
{
    char verb[64];
    char target[64];
    const char *data = command;

    const int32_t mode = sv_scriptExec ? sv_scriptExec->current.integer : 1;

    if (mode == 0)
    {
        Scr_Error("exec: disabled by sv_scriptExec 0");
        return false;
    }

    if (mode >= 2)
        return true;

    if (!G_ScrUtil_NextToken(&data, verb, sizeof(verb)))
        return false;

    for (uint32_t i = 0; i < ARRAY_COUNT(g_scrExecDeniedCommands); ++i)
    {
        if (!I_stricmp(verb, g_scrExecDeniedCommands[i]))
        {
            Scr_Error(va("exec: '%s' is not allowed from script; sv_scriptExec 2 permits it", verb));
            return false;
        }
    }

    // set, seta, sets, setu and friends all take the dvar name as argument one.
    if (!I_strnicmp(verb, "set", 3) || !I_stricmp(verb, "toggle"))
    {
        if (!G_ScrUtil_NextToken(&data, target, sizeof(target)))
            return true;

        for (uint32_t i = 0; i < ARRAY_COUNT(g_scrExecProtectedDvars); ++i)
        {
            if (!I_stricmp(target, g_scrExecProtectedDvars[i]))
            {
                Scr_Error(va("exec: '%s' is protected from script; sv_scriptExec 2 permits it", target));
                return false;
            }
        }
    }

    return true;
}

// Console output captured for execEx, filled by the redirect below.
static char g_scrExecOutput[1024];
static uint32_t g_scrExecOutputLen;

static void G_ScrUtil_ExecFlush(char *data)
{
    if (!data)
        return;

    const size_t len = strlen(data);
    const size_t room = sizeof(g_scrExecOutput) - 1 - g_scrExecOutputLen;
    const size_t copy = len < room ? len : room;

    // CoD4x keeps only whatever arrived in the first flush and drops the rest,
    // so a command that prints more than one buffer's worth loses its tail
    // silently. Append instead, and stop at capacity.
    memcpy(g_scrExecOutput + g_scrExecOutputLen, data, copy);
    g_scrExecOutputLen += (uint32_t)copy;
    g_scrExecOutput[g_scrExecOutputLen] = 0;
}

// exec( <command> )
//
// Queued rather than executed inline: a script that execs `map mp_crash` would
// otherwise tear the VM down underneath the thread that called it.
void __cdecl GScr_Exec()
{
    char command[1024];

    if (Scr_GetNumParam() != 1)
    {
        Scr_Error("Usage: exec( <command> );");
        return;
    }

    I_strncpyz(command, Scr_GetString(0), sizeof(command) - 1);

    if (!G_ScrUtil_ExecAllowed(command))
        return;

    I_strncat(command, sizeof(command), "\n");
    Cbuf_AddText(0, command);
}

// string = execex( <command> )
//
// Runs the command immediately and returns whatever it printed. Commands that
// load or restart a map still go through the buffer, since running one here
// would free the script the caller is executing from; those return "".
void __cdecl GScr_ExecEx()
{
    char command[1024];
    char outputbuf[1024];
    char verb[64];

    if (Scr_GetNumParam() != 1)
    {
        Scr_Error("Usage: string = execEx( <command> );");
        return;
    }

    I_strncpyz(command, Scr_GetString(0), sizeof(command) - 1);

    if (!G_ScrUtil_ExecAllowed(command))
        return;

    g_scrExecOutput[0] = 0;
    g_scrExecOutputLen = 0;

    const char *data = command;
    G_ScrUtil_NextToken(&data, verb, sizeof(verb));

    // CoD4x tests the first three characters against "map", which also catches
    // every other command starting with those letters. Match the whole verb.
    const bool deferred =
        !I_stricmp(verb, "map") ||
        !I_stricmp(verb, "devmap") ||
        !I_stricmp(verb, "map_restart") ||
        !I_stricmp(verb, "map_rotate") ||
        !I_stricmp(verb, "fast_restart");

    if (deferred)
    {
        I_strncat(command, sizeof(command), "\n");
        Cbuf_AddText(0, command);
    }
    else
    {
        Com_BeginRedirect(outputbuf, sizeof(outputbuf), G_ScrUtil_ExecFlush);
        Cmd_ExecuteSingleCommand(0, 0, command);
        Com_EndRedirect();
    }

    Scr_AddString(g_scrExecOutput);
}

// ---------------------------------------------------------------------------
// Player methods
// ---------------------------------------------------------------------------

static client_t *G_ScrUtil_ClientForEntRef(scr_entref_t entref)
{
    gentity_s *ent = GetPlayerEntity(entref);

    if (!ent || !ent->client || entref.entnum >= (uint32_t)sv_maxclients->current.integer)
    {
        Scr_ObjectError("not a client entity");
        return nullptr;
    }

    return &svs.clients[entref.entnum];
}

// string = self getuserinfo( <key> )
//
// Reads the client's own userinfo, which is how a mod gets at settings the
// game does not otherwise expose -- rate, snaps, cl_anonymous.
void __cdecl PlayerCmd_GetUserinfo(scr_entref_t arg)
{
    client_t *cl = G_ScrUtil_ClientForEntRef(arg);

    if (!cl)
        return;

    if (Scr_GetNumParam() != 1)
    {
        Scr_Error("Usage: string = <player> getUserinfo( <key> );");
        return;
    }

    Scr_AddString(Info_ValueForKey(cl->userinfo, Scr_GetString(0)));
}

// int = self getping()
//
// The scoreboard ping, the same number the status command prints.
void __cdecl PlayerCmd_GetPing(scr_entref_t arg)
{
    client_t *cl = G_ScrUtil_ClientForEntRef(arg);

    if (!cl)
        return;

    if (Scr_GetNumParam() != 0)
    {
        Scr_Error("Usage: int = <player> getPing();");
        return;
    }

    Scr_AddInt(cl->ping);
}

// ---------------------------------------------------------------------------

void __cdecl Scr_AddUtilityFunctions()
{
    sv_scriptExec = Dvar_RegisterInt(
        "sv_scriptExec", 1, 0, 2, DVAR_ARCHIVE,
        "Console access for the exec and execEx script functions: "
        "0 disabled, 1 allowed except server control and passwords, 2 unrestricted");

    Scr_AddFunction("toupper", GScr_ToUpper, 0);
    Scr_AddFunction("strcolorstrip", GScr_StrColorStrip, 0);
    Scr_AddFunction("strctrlstrip", GScr_StrCtrlStrip, 0);
    Scr_AddFunction("strrepl", GScr_StrRepl, 0);
    Scr_AddFunction("strreplace", GScr_StrReplace, 0);
    Scr_AddFunction("strtokbylen", GScr_StrTokByLen, 0);

    Scr_AddFunction("pow", GScr_Pow, 0);
    Scr_AddFunction("float", GScr_Float, 0);
    Scr_AddFunction("vectoradd", GScr_VectorAdd, 0);

    Scr_AddFunction("isarray", GScr_IsArrayValue, 0);
    Scr_AddFunction("isentity", GScr_IsEntityValue, 0);
    Scr_AddFunction("isvector", GScr_IsVectorValue, 0);
    Scr_AddFunction("isfloat", GScr_IsFloatValue, 0);
    Scr_AddFunction("isint", GScr_IsIntValue, 0);

    // CoD4x calls this iscvardefined, from the Quake 3 name for a dvar. Both
    // spellings are registered: the first is what existing scripts call, the
    // second is what the rest of this engine calls them.
    Scr_AddFunction("iscvardefined", GScr_IsDvarDefined, 0);
    Scr_AddFunction("isdvardefined", GScr_IsDvarDefined, 0);

    Scr_AddFunction("getrealtime", GScr_GetRealTime, 0);
    Scr_AddFunction("timetostring", GScr_TimeToString, 0);

    Scr_AddFunction("base64encode", GScr_Base64Encode, 0);
    Scr_AddFunction("base64decode", GScr_Base64Decode, 0);

    Scr_AddFunction("exec", GScr_Exec, 0);
    Scr_AddFunction("execex", GScr_ExecEx, 0);

    Scr_AddMethod("getuserinfo", PlayerCmd_GetUserinfo, 0);
    Scr_AddMethod("getping", PlayerCmd_GetPing, 0);
}
