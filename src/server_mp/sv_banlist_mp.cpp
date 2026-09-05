#ifndef KISAK_MP
#error This File is MultiPlayer Only
#endif

#include "sv_banlist_mp.h"
#include "server_mp.h"

#include <qcommon/qcommon.h>
#include <qcommon/cmd.h>
#include <universal/com_files.h>
#include <universal/com_memory.h>
#include <universal/q_parse.h>

#include <string.h>

// A guid is at most 32 characters -- both cdkeyHash and the Steam id are
// carried in char[33] buffers -- so a longer token in the file can never match
// one and is dropped at load rather than stored and compared.
#define SV_BANLIST_GUID_SIZE 33

// How long a cache entry is trusted before the next lookup re-reads the file.
// This is what picks up an edit made outside the server: an admin appending to
// ban.txt by hand, or another tool writing it. Bans made through this server
// do not wait for it -- those invalidate the cache directly.
#define SV_BANLIST_RECHECK_MSEC 10000

// The allocation tag other server-side Z_Malloc callers in this tree use.
#define SV_BANLIST_MEMORY_TYPE 9

static char    *g_svBanGuids;      // count * SV_BANLIST_GUID_SIZE, or null
static uint32_t g_svBanCount;
static const char **g_svBanHash;   // open addressed, power of two, null = free
static uint32_t g_svBanHashSize;
static uint32_t g_svBanLoadTime;
static bool     g_svBanLoaded;

static uint32_t SV_BanList_Hash(const char *guid)
{
    uint32_t hash = 2166136261u;

    for (; *guid; ++guid)
    {
        hash ^= (uint8_t)*guid;
        hash *= 16777619u;
    }

    return hash;
}

static void SV_BanList_Free()
{
    if (g_svBanGuids)
        Z_Free(g_svBanGuids, SV_BANLIST_MEMORY_TYPE);

    if (g_svBanHash)
        Z_Free(g_svBanHash, SV_BANLIST_MEMORY_TYPE);

    g_svBanGuids = nullptr;
    g_svBanHash = nullptr;
    g_svBanCount = 0;
    g_svBanHashSize = 0;
    g_svBanLoaded = false;
}

void __cdecl SV_BanList_Invalidate()
{
    SV_BanList_Free();
}

static void SV_BanList_Insert(const char *guid)
{
    uint32_t slot = SV_BanList_Hash(guid) & (g_svBanHashSize - 1);

    // The table is sized to at least twice the entry count, so a free slot
    // always exists and this cannot spin.
    while (g_svBanHash[slot])
    {
        if (!strcmp(g_svBanHash[slot], guid))
            return;

        slot = (slot + 1) & (g_svBanHashSize - 1);
    }

    g_svBanHash[slot] = guid;
}

// Walks the file the way the function this replaces did -- Com_Parse for the
// token, then skip to the next line -- so comment and quoting behaviour is
// unchanged. `store` null means count only.
static uint32_t SV_BanList_Walk(const char *text, char *store)
{
    uint32_t count = 0;

    while (1)
    {
        const char *token = (const char *)Com_Parse(&text);

        if (!*token)
            break;

        if (strlen(token) < SV_BANLIST_GUID_SIZE)
        {
            if (store)
                I_strncpyz(&store[count * SV_BANLIST_GUID_SIZE], token, SV_BANLIST_GUID_SIZE);

            ++count;
        }

        Com_SkipRestOfLine(&text);
    }

    return count;
}

static void SV_BanList_Load()
{
    char *file = nullptr;

    SV_BanList_Free();

    // Mark loaded before any early return: a missing or empty ban.txt is a
    // valid answer of "nobody is banned", and re-reading it per lookup is
    // exactly what this cache exists to stop.
    g_svBanLoaded = true;
    g_svBanLoadTime = Sys_Milliseconds();

    if (FS_ReadFile("ban.txt", (void **)&file) < 0)
        return;

    const uint32_t count = SV_BanList_Walk(file, nullptr);

    if (count == 0)
    {
        FS_FreeFile(file);
        return;
    }

    uint32_t hashSize = 1;
    while (hashSize < count * 2)
        hashSize *= 2;

    g_svBanGuids = (char *)Z_Malloc(count * SV_BANLIST_GUID_SIZE, "SV_BanList_Load", SV_BANLIST_MEMORY_TYPE);
    g_svBanHash = (const char **)Z_Malloc(hashSize * sizeof(*g_svBanHash), "SV_BanList_Load", SV_BANLIST_MEMORY_TYPE);

    if (!g_svBanGuids || !g_svBanHash)
    {
        // Out of zone memory. Leave the cache empty and say so rather than
        // answering "not banned" from a half built table, which would quietly
        // let every banned player back in.
        Com_PrintError(15, "SV_BanList: out of memory for %i bans, ban.txt is not being enforced\n", count);
        SV_BanList_Free();
        g_svBanLoaded = true;
        FS_FreeFile(file);
        return;
    }

    Com_Memset(g_svBanHash, 0, hashSize * sizeof(*g_svBanHash));
    g_svBanHashSize = hashSize;
    g_svBanCount = SV_BanList_Walk(file, g_svBanGuids);

    for (uint32_t i = 0; i < g_svBanCount; ++i)
        SV_BanList_Insert(&g_svBanGuids[i * SV_BANLIST_GUID_SIZE]);

    FS_FreeFile(file);

    Com_DPrintf(15, "SV_BanList: %i bans loaded from ban.txt\n", g_svBanCount);
}

int __cdecl SV_IsBannedGuid(const char *guid)
{
    if (!*guid)
        return 0;

    // Sys_Milliseconds is unsigned and wraps; the signed difference keeps the
    // comparison right across the wrap.
    if (!g_svBanLoaded || (int32_t)(Sys_Milliseconds() - g_svBanLoadTime) > SV_BANLIST_RECHECK_MSEC)
        SV_BanList_Load();

    if (!g_svBanHash)
        return 0;

    uint32_t slot = SV_BanList_Hash(guid) & (g_svBanHashSize - 1);

    while (g_svBanHash[slot])
    {
        if (!strcmp(g_svBanHash[slot], guid))
            return 1;

        slot = (slot + 1) & (g_svBanHashSize - 1);
    }

    return 0;
}

static void __cdecl SV_BanListReload_f()
{
    SV_BanList_Invalidate();
    SV_BanList_Load();

    Com_Printf(15, "ban.txt reloaded: %i bans\n", g_svBanCount);
}

static cmd_function_s SV_BanListReload_f_VAR;
static cmd_function_s SV_BanListReload_f_VAR_SERVER;

void __cdecl SV_BanList_AddCommands()
{
    // For an admin who edited ban.txt and does not want to wait out
    // SV_BANLIST_RECHECK_MSEC.
    Cmd_AddCommandInternal("banlist_reload", Cbuf_AddServerText_f, &SV_BanListReload_f_VAR);
    Cmd_AddServerCommandInternal("banlist_reload", SV_BanListReload_f, &SV_BanListReload_f_VAR_SERVER);
}
