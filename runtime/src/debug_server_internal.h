/* The seam between the debug server's core and its command handlers.
 *
 * Handlers do three things: read a request field, read some emulator state, and
 * send a reply. Only the first and third need the server, and this is all of
 * it -- which is what lets 8,000 lines of handlers live in files of their own
 * instead of one 15,000-line translation unit.
 *
 * Not a public interface: nothing outside the debug server should include this.
 * The public surface is debug_server.h.
 */
#ifndef DEBUG_SERVER_INTERNAL_H
#define DEBUG_SERVER_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

struct CPUState;

/* --- request ------------------------------------------------------------ */

int         json_get_int(const char *json, const char *key, int def);
double      json_get_double(const char *json, const char *key, double def);
/* Copies into `out` and returns it, or NULL when the key is absent. */
const char *json_get_str(const char *json, const char *key,
                         char *out, int out_sz);
uint32_t    hex_to_u32(const char *s);

/* Escape a string for embedding in a JSON reply (quotes, backslashes,
 * control characters). Truncates rather than overruns. */
void json_escape_string(char *dst, size_t dst_size, const char *src);

/* --- reply -------------------------------------------------------------- */

/* One response per request, one line. send_fmt is the general form; send_ok
 * and send_err are the two shapes almost every handler ends in. */
void debug_server_send_fmt(const char *fmt, ...);
void debug_server_send_line(const char *json);
#define send_fmt debug_server_send_fmt
void send_ok(int id);
void send_err(int id, const char *msg);

/* --- machine ------------------------------------------------------------ */

/* The CPU the server is attached to, or NULL before debug_server_set_cpu().
 * Handlers that dereference it must check. */
struct CPUState *debug_server_cpu(void);

#endif /* DEBUG_SERVER_INTERNAL_H */
