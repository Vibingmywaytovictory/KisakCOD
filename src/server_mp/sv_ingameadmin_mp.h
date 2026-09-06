#pragma once
#ifndef KISAK_MP
#error This File is MultiPlayer Only
#endif

#include <universal/q_shared.h>

struct client_t;

// In-game admin commands, ported from CoD4x (AGPLv3) sv_ingameadmin.c.
//
// A player types a command into chat prefixed with $, ! or / and it runs on the
// server as them, with the output redirected back to their screen instead of the
// server console. What they are allowed to run is decided by sv_auth_mp.h, which
// grants power for a password and nothing else.
//
// Two things carried over from CoD4x verbatim because they are load bearing:
//
//   - The output chunker. Server commands are size limited, so console output is
//     split into <=240 byte pieces at the last linebreak that fits, and every
//     double quote is replaced with an apostrophe because the command it travels
//     in is itself quoted.
//   - Separator stripping. ';', '\n' and '\r' are cut from the command line
//     before it reaches Cmd_ExecuteSingleCommand, so a player cannot chain a
//     second command onto one they are allowed to run.

// The invoker of the command currently executing, or -1 when the console or rcon
// is the caller. Commands that need to know who ran them read these; anything
// that does not can ignore them entirely.
int32_t     __cdecl Cmd_GetInvokerClnum();
int32_t     __cdecl Cmd_GetInvokerPower();
const char *__cdecl Cmd_GetInvokerName();
const char *__cdecl Cmd_GetInvokerGuid();

void __cdecl Cmd_SetCurrentInvokerInfo(int32_t clientNum, int32_t power, const char *name, const char *guid);
void __cdecl Cmd_ClearCurrentInvokerInfo();

// Runs one chat-prefixed command as `clientNum`. `msg` is the text with the
// prefix already stripped. Returns true if the text was consumed as a command
// (including when it was refused), false if it should fall through to chat.
bool __cdecl SV_ExecuteRemoteCmd(int32_t clientNum, const char *msg);

// Called from G_Say before anything else. Returns true if the message was a
// command and must not be shown as chat.
bool __cdecl SV_HandleChatCommand(int32_t clientNum, const char *chatText);

// Timestamped audit line naming the invoker. Every state-changing admin action
// should call this -- an admin system with no record of who did what is worth
// very little after the fact.
void __cdecl SV_PrintAdministrativeLog(const char *fmt, ...);

void __cdecl SV_InGameAdmin_AddCommands();
void __cdecl SV_InGameAdmin_ClientDisconnect(client_t *cl);
