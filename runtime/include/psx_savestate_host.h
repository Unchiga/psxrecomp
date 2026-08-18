/* Host-side save-state glue: the slot menu's state machine and the input guard
 * that keeps a confirm press from reaching the guest after a restore.
 *
 * Drawing lives in psx_savestate_menu.h; serialisation in savestate.h. */
#ifndef PSX_SAVESTATE_HOST_H
#define PSX_SAVESTATE_HOST_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Slot menu ---------------------------------------------------------- */

/* Menu state. Read directly by the host event loop, which has to know whether
 * a key belongs to the menu before it reaches the game. */
extern int savestate_menu_open;
extern int savestate_menu_ignore_toggle_release;
/* Keycodes cross this boundary as plain ints: SDL_Keycode is an integer type
 * and keeping SDL out of the header is what lets C modules include it freely
 * (psx_savestate_menu.h does the same). */
extern int savestate_menu_open_key;

void savestate_menu_close(void);
/* opened_by_key is the key that opened it, so its own release does not
 * immediately close it again; pass 0 when opened by pad or mouse. */
void savestate_menu_toggle(int opened_by_key);
void savestate_menu_move(int delta);
/* Select a slot outright (the mouse hit-test picks a row rather than stepping);
 * redraws the overlay, same as move(). */
void savestate_menu_set_slot(int slot);
void savestate_menu_submit(int save);
void savestate_menu_handle_key(int key, int mod, int repeat);

/* --- Input guard -------------------------------------------------------- */

/* Arm the release latch alone (the SAVE case), or the latch plus the settle
 * window (LOAD, which also has to neutralise the sticks). */
void savestate_hold_guard_arm(void);
void savestate_input_guard_arm(void);

/* Apply the latch to one slot's finished button word. The PSX pad word is
 * active-low, so this can only ever turn bits back on. */
uint16_t savestate_hold_guard_apply(int slot, uint16_t buttons);

/* Is the post-load settle window still muting input? */
int savestate_input_guard_active(void);

/* Per-slot latch masks; the pad sampler reads these while building its word. */
extern uint16_t g_savestate_hold_mask[];

/* --- Trace ring (savestate_input_trace) --------------------------------- */

void savestate_diag_arm(const char *what);
void savestate_diag_note(const char *tag, int slot, unsigned raw,
                         unsigned out_btn, int windowed);
int  psx_savestate_trace_json(char *buf, unsigned size, int count);

/* --- Provided BY the host ----------------------------------------------- */

/* Nonzero while the player still holds an input that would count as "resume".
 * The guard cannot answer this itself: it is a question about SDL keyboard and
 * controller state, which stays in the frontend. */
int psx_savestate_host_resume_inputs_held(void);

/* Menu state for the debug server (savestate_menu_state). */
void psx_savestate_menu_debug(int *open, int *slot);

#ifdef __cplusplus
}
#endif

#endif /* PSX_SAVESTATE_HOST_H */
