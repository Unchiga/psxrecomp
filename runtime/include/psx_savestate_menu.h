#ifndef PSX_SAVESTATE_MENU_H
#define PSX_SAVESTATE_MENU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void psx_savestate_menu_set_state(int open, int selected_slot);
void psx_savestate_menu_note_slots_changed(void);
int  psx_savestate_menu_needs_present(void);

/* Size the panel's canvas to the surface it will be stretched across.
 *
 * The panel used to author one fixed 640x480 image whatever the window was,
 * which is why its text was soft: a 3x stretch of an 8x8 bitmap sheet. It now
 * lays itself out in units of 1/480 of the canvas HEIGHT and rasterises at
 * whatever size it is given, so text comes out at the window's own resolution.
 * Call this before _overlay_image; a caller that does not gets the last size
 * it was told, or 640x480 if it was never told one.
 *
 * The reported canvas can still come back SMALLER than requested -- it is
 * clamped to what was actually allocated, and the fallback buffer is smaller
 * than a 4K window. Composite it stretched to the full surface either way,
 * which is what every backend already did. */
void psx_savestate_menu_set_layout(int surface_w, int surface_h);

int  psx_savestate_menu_overlay_image(const uint32_t **pixels, int *w, int *h);

/* Close button (the X in the panel's top-right corner).
 *
 * Coordinates are DRAWABLE pixels, together with the drawable size the panel
 * was stretched across. The canvas now tracks the window (see
 * psx_savestate_menu_set_layout), but it is not guaranteed to EQUAL it, so the
 * caller still supplies the surface size and the mapping still happens here,
 * next to the geometry it has to agree with.
 *
 * _hit_close reports whether a click landed on the button; _hover updates the
 * highlight (and marks the canvas dirty only when the state actually changes,
 * so a moving cursor does not force a redraw every frame). */
int  psx_savestate_menu_hit_close(int x, int y, int surface_w, int surface_h);
void psx_savestate_menu_hover(int x, int y, int surface_w, int surface_h);

/* Mouse navigation. The panel already owned a close button and hover
 * highlighting; these complete it, so every action the footer advertises is
 * reachable without touching the controller.
 *
 * _hit_slot returns the ABSOLUTE slot index under the cursor (0..SLOTS-1), or
 * -1. Only the three rows currently scrolled into view can be hit, which is
 * why this lives here: the scroll window is derived from the selection during
 * rasterization, and a caller has no way to know which rows those are.
 *
 * _hit_action returns one of PSX_SSM_ACTION_*, for the footer's LOAD / SAVE /
 * BACK legends. They were already drawn as buttons; this makes them behave
 * like buttons. */
enum {
    PSX_SSM_ACTION_NONE = 0,
    PSX_SSM_ACTION_LOAD,
    PSX_SSM_ACTION_SAVE,
    PSX_SSM_ACTION_BACK
};
int  psx_savestate_menu_hit_slot(int x, int y, int surface_w, int surface_h);
int  psx_savestate_menu_hit_action(int x, int y, int surface_w, int surface_h);

#ifdef __cplusplus
}
#endif

#endif /* PSX_SAVESTATE_MENU_H */
