/* psx_game_hooks.c — see psx_game_hooks.h. */

#include "psx_game_hooks.h"
#include <stdio.h>

/* Fixed capacity, no allocation: registration happens during static
 * initialisation, before anything could report a failure usefully.
 * Registration order is call order, which is what makes a title's own
 * ordering predictable -- but that order is the linker's constructor order,
 * so a full table drops a DIFFERENT set of hooks on each platform (a title
 * with 21 per-frame hooks lost its Card Manager tick on Windows only, at
 * the old limit of 16, and the window stayed blank). Overflows are counted
 * and reported when the start hooks run, when stdio is certainly up. */
#define PSX_GAME_HOOK_MAX 64
static int s_dropped;

static PsxGameHook s_start[PSX_GAME_HOOK_MAX];
static int         s_start_count;
static PsxGameHook s_stop[PSX_GAME_HOOK_MAX];
static int         s_stop_count;
static PsxGameHook s_frame[PSX_GAME_HOOK_MAX];
static int         s_frame_count;
static PsxGameHook s_vblank[PSX_GAME_HOOK_MAX];
static int         s_vblank_count;
static PsxGameEventHook s_event[PSX_GAME_HOOK_MAX];
static int              s_event_count;

int psx_game_add_start_hook(PsxGameHook fn) {
    if (!fn) return 0;
    if (s_start_count >= PSX_GAME_HOOK_MAX) { s_dropped++; return 0; }
    s_start[s_start_count++] = fn;
    return 1;
}

int psx_game_add_stop_hook(PsxGameHook fn) {
    if (!fn) return 0;
    if (s_stop_count >= PSX_GAME_HOOK_MAX) { s_dropped++; return 0; }
    s_stop[s_stop_count++] = fn;
    return 1;
}

int psx_game_add_frame_hook(PsxGameHook fn) {
    if (!fn) return 0;
    if (s_frame_count >= PSX_GAME_HOOK_MAX) { s_dropped++; return 0; }
    s_frame[s_frame_count++] = fn;
    return 1;
}

int psx_game_add_vblank_hook(PsxGameHook fn) {
    if (!fn) return 0;
    if (s_vblank_count >= PSX_GAME_HOOK_MAX) { s_dropped++; return 0; }
    s_vblank[s_vblank_count++] = fn;
    return 1;
}

int psx_game_add_event_hook(PsxGameEventHook fn) {
    if (!fn) return 0;
    if (s_event_count >= PSX_GAME_HOOK_MAX) { s_dropped++; return 0; }
    s_event[s_event_count++] = fn;
    return 1;
}

/* First hook to claim the event wins, and the rest never see it — otherwise
 * two windows could both act on one click. */
int psx_game_run_event_hooks(const void *sdl_event) {
    for (int i = 0; i < s_event_count; i++)
        if (s_event[i](sdl_event)) return 1;
    return 0;
}

void psx_game_run_start_hooks(void) {
    if (s_dropped) fprintf(stderr, "psx_game_hooks: %d hook(s) not registered: the table (%d per kind) is full\n", s_dropped, PSX_GAME_HOOK_MAX);
    for (int i = 0; i < s_start_count; i++) s_start[i]();
}

void psx_game_run_stop_hooks(void) {
    /* Reverse registration mirrors destructor order and lets coordinators
     * registered early release after the clients they own. */
    for (int i = s_stop_count - 1; i >= 0; i--) s_stop[i]();
}

void psx_game_run_frame_hooks(void) {
    for (int i = 0; i < s_frame_count; i++) s_frame[i]();
}

void psx_game_run_vblank_hooks(void) {
    for (int i = 0; i < s_vblank_count; i++) s_vblank[i]();
}
