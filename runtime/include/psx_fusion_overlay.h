#ifndef PSX_FUSION_OVERLAY_H
#define PSX_FUSION_OVERLAY_H

/* The in-duel fusion assistant's one line of text.
 *
 * Sits just above the hand and answers the question the game never does: what
 * do these cards make? With nothing picked it names the best fusion the hand
 * can reach; as cards are picked it shows the card that would stand if they
 * were summoned now, which IS the running fusion, because the game folds a
 * multi-card summon two at a time in pick order.
 *
 * Same shape as the rank meter and the CARD DROPS tags: a small ARGB canvas
 * authored in GUEST pixels that the renderer maps into the letterboxed game
 * rect. Pure state and rasterisation — no GL, and no guest reads of its own;
 * psx_fusion_assist supplies the content.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Once a frame: re-reads the duel through psx_fusion_assist and re-rasterises
 * only when the line actually changes. Cheap to call always. */
void psx_fusion_overlay_tick(void);

/* 1 + fills the ARGB canvas when there is something to draw. Guest pixels. */
int  psx_fusion_overlay_image(const uint32_t **pixels, int *w, int *h);

/* Top-left corner in guest pixels. */
void psx_fusion_overlay_origin(int *x, int *y);

/* 1 while the line is on screen, so a present can be forced. */
int  psx_fusion_overlay_needs_present(void);

/* Live layout tuning over the debug server — placing text against the game's
 * own art is a by-eye job and a rebuild per nudge costs the player their duel.
 * ABSOLUTE values; PSX_FUSION_OVERLAY_KEEP leaves a field alone. */
#define PSX_FUSION_OVERLAY_KEEP (-100000)
void psx_fusion_overlay_tune(int x, int y, int text_x, int enabled);
void psx_fusion_overlay_tune_get(int *x, int *y, int *text_x, int *enabled);

/* What the line currently says, for `fusion_overlay`. Returns the text. */
const char *psx_fusion_overlay_text(void);

#ifdef __cplusplus
}
#endif

#endif /* PSX_FUSION_OVERLAY_H */
