// Portions of this file are derived from CoD4x_Server
//   https://github.com/callofduty4x/CoD4x_Server
//   Copyright (C) 2010-2013 Ninja and TheKelm
//   Copyright (C) the CoD4x authors
// Licensed under the GNU Affero General Public License v3 (see LICENSE.AGPLv3).
//
// Adapted to KisakCOD's engine APIs. The combined work is conveyed under
// GPLv3 section 13; see LICENSING.md.
//
// CoD4x layers these builtins on its own parallel FILE* handle table, built on
// raw fopen against an OS path. KisakCOD already has a handle-based filesystem,
// so this sits on that instead: one less handle table, and script files obey the
// same search-path rules as everything else.

#include <universal/q_shared.h>
#include "sv_script_fs_mp.h"
#include "server_mp.h"

#include <qcommon/qcommon.h>
#include <universal/com_files.h>
#include <script/scr_vm.h>
#include <game_mp/g_scr_builtins_mp.h>

#include <string.h>

#define MAX_SCRIPT_FILEHANDLES 8
#define SCRIPT_FS_BUFFER_SIZE  4096
#define SCRIPT_FS_MAX_LINE     8192

// Everything a script opens lives under here, with no way out. See
// SV_ScriptFS_SanitizePath.
static const char SCRIPT_FS_ROOT[] = "scriptdata/";

struct scriptFile_t
{
    int      fsHandle;                  // KisakCOD FS handle; 0 means the slot is free
    bool     writing;
    char     name[MAX_QPATH];           // sanitised path, for the duplicate-open check

    char     buf[SCRIPT_FS_BUFFER_SIZE]; // read-ahead, so fs_readline is not a syscall per byte
    uint32_t bufLen;
    uint32_t bufPos;
    bool     atEof;
};

static scriptFile_t s_scriptFiles[MAX_SCRIPT_FILEHANDLES];

// Confine a script-supplied path to scriptdata/.
//
// This is the whole security boundary for script file I/O, so it is a whitelist
// rather than a blacklist: every character must be one we allow, and every path
// component must be an ordinary name. That rejects absolute paths, drive letters
// and NTFS streams (':'), UNC paths, parent traversal (".."), and anything with
// a control character or wildcard in it.
//
// Scripts that already say "scriptdata/" are not double-prefixed -- Bot Warfare's
// CoD4x adapter prepends it before calling, and would otherwise end up asking for
// scriptdata/scriptdata/... .
static bool SV_ScriptFS_SanitizePath(const char *in, char *out, uint32_t outSize)
{
    if (!in || !*in || !out)
        return false;

    char clean[MAX_QPATH];
    uint32_t n = 0;

    for (const char *p = in; *p; ++p)
    {
        char c = *p;

        if (c == '\\')
            c = '/';

        const bool allowed =
            (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '_' || c == '-' || c == '.' || c == '/';

        if (!allowed)
            return false;

        if (n + 1 >= sizeof(clean))
            return false;

        clean[n++] = c;
    }
    clean[n] = '\0';

    if (n == 0)
        return false;
    if (clean[0] == '/')        // absolute
        return false;
    if (clean[n - 1] == '/')    // names a directory, not a file
        return false;

    // Every component must be a real name: no "", no ".", no "..".
    const char *seg = clean;
    for (;;)
    {
        const char *slash = strchr(seg, '/');
        const uint32_t len = slash ? (uint32_t)(slash - seg) : (uint32_t)strlen(seg);

        if (len == 0)
            return false;
        if (len == 1 && seg[0] == '.')
            return false;
        if (len == 2 && seg[0] == '.' && seg[1] == '.')
            return false;

        if (!slash)
            break;

        seg = slash + 1;
    }

    const uint32_t rootLen = (uint32_t)(sizeof(SCRIPT_FS_ROOT) - 1);

    if (!I_strnicmp(clean, SCRIPT_FS_ROOT, rootLen))
        I_strncpyz(out, clean, outSize);
    else
        Com_sprintf(out, outSize, "%s%s", SCRIPT_FS_ROOT, clean);

    return true;
}

// Script handles are 1-based so that 0 can mean failure, matching CoD4x.
static scriptFile_t *SV_ScriptFS_FromHandle(int handle)
{
    if (handle < 1 || handle > MAX_SCRIPT_FILEHANDLES)
        return nullptr;

    scriptFile_t *f = &s_scriptFiles[handle - 1];
    if (!f->fsHandle)
        return nullptr;

    return f;
}

static void SV_ScriptFS_Close(scriptFile_t *f)
{
    if (!f->fsHandle)
        return;

    FS_FCloseFile(f->fsHandle);
    memset(f, 0, sizeof(*f));
}

void __cdecl SV_ScriptFS_CloseAll()
{
    // Called from G_ShutdownGame. On the quit path SV_Shutdown runs before
    // FS_Shutdown so the filesystem is still up, but this is shutdown code
    // reaching into another subsystem -- do not assume that ordering holds on
    // every path.
    if (!FS_Initialized())
    {
        memset(s_scriptFiles, 0, sizeof(s_scriptFiles));
        return;
    }

    for (int32_t i = 0; i < MAX_SCRIPT_FILEHANDLES; ++i)
        SV_ScriptFS_Close(&s_scriptFiles[i]);
}

// One character at a time out of the read-ahead buffer. Returns false at EOF.
static bool SV_ScriptFS_ReadChar(scriptFile_t *f, char *out)
{
    if (f->bufPos >= f->bufLen)
    {
        if (f->atEof)
            return false;

        const uint32_t got = FS_Read((uint8_t *)f->buf, sizeof(f->buf), f->fsHandle);

        // FS_Read reports unsigned; treat anything not in range as end of file
        // rather than trusting it as a length.
        if (got == 0 || got > sizeof(f->buf))
        {
            f->atEof = true;
            f->bufLen = 0;
            f->bufPos = 0;
            return false;
        }

        f->bufLen = got;
        f->bufPos = 0;

        if (got < sizeof(f->buf))
            f->atEof = true; // short read means that was the last block
    }

    *out = f->buf[f->bufPos++];
    return true;
}

// fs_fopen( <filename>, <"read"|"write"|"append"> ) -> handle, or 0 on failure
void __cdecl GScr_FS_FOpen()
{
    if (Scr_GetNumParam() != 2)
    {
        Scr_Error("Usage: fs_fopen( <filename>, <\"read\" | \"write\" | \"append\"> );");
        return;
    }

    const char *filename = Scr_GetString(0);
    const char *mode = Scr_GetString(1);

    char path[MAX_QPATH];
    if (!SV_ScriptFS_SanitizePath(filename, path, sizeof(path)))
    {
        Scr_ParamError(0, "fs_fopen: illegal path. Script files live under scriptdata/; "
                          "'..', absolute paths and drive letters are refused.");
        return;
    }

    // Refuse a second handle to a file already open, so a script cannot read a
    // file it is part-way through writing.
    for (int32_t i = 0; i < MAX_SCRIPT_FILEHANDLES; ++i)
    {
        if (s_scriptFiles[i].fsHandle && !I_stricmp(s_scriptFiles[i].name, path))
        {
            Com_Printf(CON_CHANNEL_SCRIPT, "fs_fopen: %s is already open\n", path);
            Scr_AddInt(0);
            return;
        }
    }

    scriptFile_t *f = nullptr;
    int32_t slot = 0;
    for (int32_t i = 0; i < MAX_SCRIPT_FILEHANDLES; ++i)
    {
        if (!s_scriptFiles[i].fsHandle)
        {
            f = &s_scriptFiles[i];
            slot = i + 1;
            break;
        }
    }

    if (!f)
    {
        Com_Printf(CON_CHANNEL_SCRIPT, "fs_fopen: all %i script file handles are in use\n",
                   MAX_SCRIPT_FILEHANDLES);
        Scr_AddInt(0);
        return;
    }

    memset(f, 0, sizeof(*f));

    int fsHandle = 0;

    if (!I_stricmp(mode, "read"))
    {
        FS_FOpenFileRead(path, &fsHandle);
        f->writing = false;
    }
    else if (!I_stricmp(mode, "write"))
    {
        fsHandle = FS_FOpenFileWrite(path);
        f->writing = true;
    }
    else if (!I_stricmp(mode, "append"))
    {
        fsHandle = FS_FOpenFileAppend(path);
        f->writing = true;
    }
    else
    {
        Scr_ParamError(1, "fs_fopen: mode must be \"read\", \"write\" or \"append\".");
        return;
    }

    if (fsHandle <= 0)
    {
        Scr_AddInt(0);
        return;
    }

    f->fsHandle = fsHandle;
    I_strncpyz(f->name, path, sizeof(f->name));

    Scr_AddInt(slot);
}

// fs_fclose( <handle> )
void __cdecl GScr_FS_FClose()
{
    if (Scr_GetNumParam() != 1)
    {
        Scr_Error("Usage: fs_fclose( <filehandle> );");
        return;
    }

    scriptFile_t *f = SV_ScriptFS_FromHandle(Scr_GetInt(0));
    if (!f)
    {
        Scr_ParamError(0, "fs_fclose: not an open file handle.");
        return;
    }

    SV_ScriptFS_Close(f);
}

// fs_testfile( <filename> ) -> bool
void __cdecl GScr_FS_TestFile()
{
    if (Scr_GetNumParam() != 1)
    {
        Scr_Error("Usage: fs_testfile( <filename> );");
        return;
    }

    char path[MAX_QPATH];
    if (!SV_ScriptFS_SanitizePath(Scr_GetString(0), path, sizeof(path)))
    {
        // A path outside the sandbox is reported as "not there" rather than as an
        // error, so a script cannot use fs_testfile to probe the filesystem for
        // what does and does not exist beyond scriptdata/.
        Scr_AddBool(0);
        return;
    }

    int fsHandle = 0;
    FS_FOpenFileRead(path, &fsHandle);

    if (fsHandle <= 0)
    {
        Scr_AddBool(0);
        return;
    }

    FS_FCloseFile(fsHandle);
    Scr_AddBool(1);
}

// fs_readline( <handle> ) -> string, or undefined at end of file
void __cdecl GScr_FS_ReadLine()
{
    if (Scr_GetNumParam() != 1)
    {
        Scr_Error("Usage: fs_readline( <filehandle> );");
        return;
    }

    scriptFile_t *f = SV_ScriptFS_FromHandle(Scr_GetInt(0));
    if (!f)
    {
        Scr_ParamError(0, "fs_readline: not an open file handle.");
        return;
    }

    if (f->writing)
    {
        Scr_ParamError(0, "fs_readline: file was opened for writing.");
        return;
    }

    char line[SCRIPT_FS_MAX_LINE];
    uint32_t len = 0;
    bool readAnything = false;
    char c;

    // CoD4x opens in text mode and lets the CRT strip carriage returns. KisakCOD's
    // filesystem is binary, so drop them here; that also means a file written on
    // either line ending reads back the same.
    while (SV_ScriptFS_ReadChar(f, &c))
    {
        readAnything = true;

        if (c == '\n')
            break;
        if (c == '\r')
            continue;

        if (len + 1 < sizeof(line))
            line[len++] = c;
    }

    line[len] = '\0';

    if (!readAnything)
        Scr_AddUndefined();
    else
        Scr_AddString(line);
}

// fs_writeline( <handle>, <string> ) -> bool
void __cdecl GScr_FS_WriteLine()
{
    if (Scr_GetNumParam() != 2)
    {
        Scr_Error("Usage: fs_writeline( <filehandle>, <data> );");
        return;
    }

    scriptFile_t *f = SV_ScriptFS_FromHandle(Scr_GetInt(0));
    if (!f)
    {
        Scr_ParamError(0, "fs_writeline: not an open file handle.");
        return;
    }

    if (!f->writing)
    {
        Scr_ParamError(0, "fs_writeline: file was opened for reading.");
        return;
    }

    const char *data = Scr_GetString(1);
    if (!data)
        data = "";

    char buffer[SCRIPT_FS_MAX_LINE];
    Com_sprintf(buffer, sizeof(buffer), "%s\n", data);

    const uint32_t len = (uint32_t)strlen(buffer);
    const uint32_t written = FS_Write(buffer, len, f->fsHandle);

    Scr_AddBool(written == len);
}

// fs_fcloseall()
void __cdecl GScr_FS_FCloseAll()
{
    if (Scr_GetNumParam() != 0)
    {
        Scr_Error("Usage: fs_fcloseall();");
        return;
    }

    SV_ScriptFS_CloseAll();
}

// fs_remove( <filename> ) -> bool
void __cdecl GScr_FS_Remove()
{
    if (Scr_GetNumParam() != 1)
    {
        Scr_Error("Usage: fs_remove( <filename> );");
        return;
    }

    char path[MAX_QPATH];
    if (!SV_ScriptFS_SanitizePath(Scr_GetString(0), path, sizeof(path)))
    {
        Scr_ParamError(0, "fs_remove: illegal path. Script files live under scriptdata/; "
                          "'..', absolute paths and drive letters are refused.");
        return;
    }

    // Deleting a file that is currently open would leave a dangling handle, so
    // refuse rather than half-succeed. CoD4x does not check this.
    for (int32_t i = 0; i < MAX_SCRIPT_FILEHANDLES; ++i)
    {
        if (s_scriptFiles[i].fsHandle && !I_stricmp(s_scriptFiles[i].name, path))
        {
            Scr_ParamError(0, "fs_remove: file is currently open");
            return;
        }
    }

    Scr_AddBool(FS_Delete(path));
}

// CoD4x registers these from scr_vm_main.c the same way. Its aliases for the
// same functions (openfile/closefile/fprintln/freadln) are not carried over --
// they are older spellings of the fs_* names and nothing needs both.
void __cdecl Scr_AddScriptFileFunctions()
{
    Scr_AddFunction("fs_fopen", GScr_FS_FOpen, 0);
    Scr_AddFunction("fs_fclose", GScr_FS_FClose, 0);
    Scr_AddFunction("fs_testfile", GScr_FS_TestFile, 0);
    Scr_AddFunction("fs_readline", GScr_FS_ReadLine, 0);
    Scr_AddFunction("fs_writeline", GScr_FS_WriteLine, 0);
    Scr_AddFunction("fs_fcloseall", GScr_FS_FCloseAll, 0);
    Scr_AddFunction("fs_remove", GScr_FS_Remove, 0);
}
