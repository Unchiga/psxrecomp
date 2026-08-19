/* psx_game_hooks.c — see psx_game_hooks.h. */

#include "psx_game_hooks.h"

/* Fixed capacity, no allocation: registration happens during static
 * initialisation, before anything could report a failure usefully, and a title
 * needing more hooks than this has a structure problem rather than a capacity
 * one. Registration order is call order, which is what makes a title's own
 * ordering predictable. */
#define PSX_GAME_HOOK_MAX 16

static PsxGameHook s_start[PSX_GAME_HOOK_MAX];
static int         s_start_count;
static PsxGameHook s_frame[PSX_GAME_HOOK_MAX];
static int         s_frame_count;

int psx_game_add_start_hook(PsxGameHook fn) {
    if (!fn || s_start_count >= PSX_GAME_HOOK_MAX) return 0;
    s_start[s_start_count++] = fn;
    return 1;
}

int psx_game_add_frame_hook(PsxGameHook fn) {
    if (!fn || s_frame_count >= PSX_GAME_HOOK_MAX) return 0;
    s_frame[s_frame_count++] = fn;
    return 1;
}

void psx_game_run_start_hooks(void) {
    for (int i = 0; i < s_start_count; i++) s_start[i]();
}

void psx_game_run_frame_hooks(void) {
    for (int i = 0; i < s_frame_count; i++) s_frame[i]();
}
