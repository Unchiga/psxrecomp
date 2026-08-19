/* psx_guest_overlay.h — host-drawn overlays that live in the GAME's coordinate
 * space rather than the window's.
 *
 * Every other host overlay (toasts, the menu bar, the rewind filmstrip) is
 * authored against the window and scales with it. A guest-space overlay is
 * authored in guest pixels instead, because it has to sit beside something the
 * game itself draws — a HUD box at a fixed guest coordinate. It therefore has
 * to be mapped through the same letterbox rect the picture uses, so it tracks
 * the image under scaling, aspect and the menu-bar inset instead of drifting
 * away from the thing it labels.
 *
 * That mapping is framework work and is identical for every such overlay, but
 * the overlays themselves belong to a title: this game has three (the duel-rank
 * meter, the CARD DROPS "New!" tags and the fusion assistant's line) and
 * another title will have none. So the renderer owns the mapping and a title
 * registers what to draw, from a PSX_MOD_CONSTRUCTOR in its own sources.
 *
 * Registration order is draw order, back to front. Everything registered here
 * still draws UNDER the menu bar, which is the topmost layer.
 */
#ifndef PSX_GUEST_OVERLAY_H
#define PSX_GUEST_OVERLAY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Required. Returns non-zero and fills px/w/h with an ARGB image sized in
     * guest pixels while the overlay is visible; 0 while it is hidden. Called
     * once per composite, on the render thread. */
    int  (*image)(const uint32_t **px, int *w, int *h);

    /* Required. Top-left corner, in guest pixels. */
    void (*origin)(int *x, int *y);

    /* Optional (NULL = none). An extra vertical nudge in HALF guest pixels,
     * for art that has to line up with a guest row it cannot sit exactly on. */
    int  (*subpixel_y)(void);

    /* Optional (NULL = never). Non-zero while the overlay has changed and the
     * frame must be presented even though the guest's picture has not moved.
     * Without it, an overlay that appears over a static screen is not drawn
     * until something else forces a present. */
    int  (*needs_present)(void);

    /* Optional (< 0 = none). Sprite-watch group whose occlusion hides this
     * overlay. Tested at composite time, not when the overlay's own state is
     * decided: that decision runs in the frame loop and can only see occluders
     * from the frame BEFORE, so the frame where something first covers the
     * overlay would still be drawn with it on — a one-frame flash. */
    int  occlusion_group;

    /* Optional (NULL = not wanted). Receives the placement actually used, so a
     * title can report it from its own debug command and check alignment
     * against the game's own art exactly instead of eyeballing a screenshot.
     * Ten ints: letterbox box[4], native[2], dest[4]. */
    void (*placed)(const int *o);
} PsxGuestOverlay;

/* Returns 1 on success, 0 if the descriptor is incomplete or the table is
 * full. The descriptor is COPIED, so a caller may pass a compound literal. */
int psx_guest_overlay_register(const PsxGuestOverlay *ov);

/* ---- Called by the runtime. Not for titles. ---- */
int                    psx_guest_overlay_count(void);
const PsxGuestOverlay *psx_guest_overlay_at(int i);
/* 1 if ANY registered overlay wants the frame presented. */
int                    psx_guest_overlay_any_needs_present(void);

#ifdef __cplusplus
}
#endif

#endif /* PSX_GUEST_OVERLAY_H */
