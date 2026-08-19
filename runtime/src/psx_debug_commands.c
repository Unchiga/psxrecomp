/* psx_debug_commands.c — see psx_debug_commands.h. */

#include <string.h>

#include "psx_debug_commands.h"

/* Fixed capacity, no allocation: registration runs during static
 * initialisation, before anything could report a failure usefully. This title
 * registers 18; a title needing more than this many distinct commands is
 * likely describing one subsystem's state through a dozen verbs rather than
 * one command with fields. */
#define PSX_DEBUG_CMD_MAX 64

typedef struct { const char *name; PsxDebugCmdHandler fn; } PsxDebugCmdEntry;

static PsxDebugCmdEntry s_cmds[PSX_DEBUG_CMD_MAX];
static int              s_count;

int psx_debug_add_command(const char *name, PsxDebugCmdHandler fn)
{
    if (!name || !*name || !fn) return 0;
    if (s_count >= PSX_DEBUG_CMD_MAX) return 0;
    for (int i = 0; i < s_count; i++)
        if (strcmp(s_cmds[i].name, name) == 0) return 0;
    s_cmds[s_count].name = name;
    s_cmds[s_count].fn   = fn;
    s_count++;
    return 1;
}

int psx_debug_run_command(const char *name, int id, const char *json)
{
    if (!name) return 0;
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_cmds[i].name, name) == 0) {
            s_cmds[i].fn(id, json);
            return 1;
        }
    }
    return 0;
}
