/* psx_debug_commands.h — a title's own TCP debug commands.
 *
 * Rule 3 says every observable is a debug command, which means a title's
 * features need commands as much as the framework's do. The framework must
 * still not name them: a debug_server.c that calls a game's functions directly
 * fails to LINK for any other title, which is exactly what happened here — 34
 * Yu-Gi-Oh entry points sat below the PSX_NO_DEBUG_TOOLS guard and so were
 * compiled unconditionally into every build.
 *
 * A title registers its commands from a PSX_MOD_CONSTRUCTOR in its own sources
 * (see mod_plugins.h), the same way it registers overlay-menu rows and game
 * hooks. Registration happens during static initialisation, so every command
 * is in place before the server accepts its first connection.
 *
 * Lookup order is the built-in table first, then this one. A title therefore
 * cannot shadow a framework command by accident; registering a name that
 * already exists is refused at registration rather than silently ignored at
 * dispatch, because a command that answers as something else is worse than one
 * that is missing.
 *
 * Handlers run at the same safe point and under the same lockstep-record
 * suppression as the built-ins, so a registered command may read guest RAM for
 * diagnostics without leaking observer traffic into a recorded trace.
 */
#ifndef PSX_DEBUG_COMMANDS_H
#define PSX_DEBUG_COMMANDS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* `id` is the request's id — echo it in the reply. `json` is the raw request
 * line; read fields out of it with the parse helpers below. A handler MUST
 * send exactly one reply. */
typedef void (*PsxDebugCmdHandler)(int id, const char *json);

/* Returns 1 on success, 0 if the name is empty, already taken, or the table is
 * full. `name` is stored by pointer, so pass a string literal or something
 * else with static storage duration. */
int psx_debug_add_command(const char *name, PsxDebugCmdHandler fn);

/* Called by the runtime after the built-in table misses. Returns 1 if a
 * registered command handled the line. Not for titles. */
int psx_debug_run_command(const char *name, int id, const char *json);

/* ---- Reply helpers ----
 *
 * Defined in debug_server.c and declared here so a title's sources can answer
 * without reaching into the server's internals. `send_fmt` is the general one;
 * the other two are the common shapes. */
void send_ok(int id);
void send_err(int id, const char *msg);
void debug_server_send_fmt(const char *fmt, ...);
#ifndef send_fmt
#define send_fmt debug_server_send_fmt
#endif

/* ---- Request parse helpers ----
 *
 * json_get_int keeps bare-decimal semantics (base 0, so "0x7B" works too);
 * json_get_str returns NULL when the key is absent, and normalises a bare
 * decimal to 0x-hex for keys that read as addresses. hex_to_u32 parses what
 * json_get_str produced. */
int         json_get_int(const char *json, const char *key, int def);
const char *json_get_str(const char *json, const char *key,
                         char *out, int out_sz);
uint32_t    hex_to_u32(const char *s);

#ifdef __cplusplus
}
#endif

#endif /* PSX_DEBUG_COMMANDS_H */
