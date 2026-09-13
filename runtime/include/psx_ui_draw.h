#ifndef PSX_UI_DRAW_H
#define PSX_UI_DRAW_H

/* Antialiased shapes and text on a caller-owned ARGB canvas.
 *
 * The F10 menu (psx_video_menu.c) grew a set of coverage-based drawing
 * helpers when it moved off the 8x8 bitmap sheet -- rounded rects that compute
 * a partial alpha at their edges rather than a hard mask, and text through
 * psx_ui_font. The toast strip and the save-state browser needed the same
 * helpers to stop looking a decade older than the bar sitting on top of them,
 * and three private copies of an antialiased round_rect is two too many. This
 * is that set, with the canvas passed in instead of being a file static.
 *
 * psx_video_menu.c still carries its own copies. It also owns primitives
 * nobody else needs (carets, slider tracks) and it is the module every other
 * overlay is being matched AGAINST, so rewriting it was not part of moving the
 * other two across. Folding it in here is the obvious next step and nothing in
 * this header is shaped to prevent it.
 *
 * COLOUR is straight (NOT premultiplied) ARGB8888, because that is what the
 * GL and Vulkan present paths blend with SRC_ALPHA / ONE_MINUS_SRC_ALPHA. A
 * shape drawn over a transparent region must therefore carry its own alpha
 * out, which is exactly why the edges below compute coverage instead of
 * testing inside/outside.
 *
 * NOT thread-safe in the sense that matters: nothing here has global state,
 * but psx_ui_font's face cache does, so a canvas must be drawn from the thread
 * that owns the state being drawn. */

#include <stdint.h>

#include "psx_ui_font.h"

#ifdef __cplusplus
extern "C" {
#endif

/* An ARGB8888 canvas plus the bounding box of everything drawn into it since
 * the box was last reset.
 *
 * The dirty box exists so a redraw can clear only what the previous one
 * touched. A caller that repaints its whole canvas every time (because the
 * canvas IS the thing it draws, or because it is opaque edge to edge) can
 * ignore dx/dy/dw/dh entirely -- they are maintained either way, and cost a
 * few comparisons per shape. */
typedef struct {
    uint32_t *px;                 /* w * h, row-major, top row first */
    int       w, h;
    int       dx, dy, dw, dh;     /* dirty box; dw == 0 means "nothing" */
} PsxUiCanvas;

/* Forget the dirty box without touching pixels. */
void psx_ui_dirty_reset(PsxUiCanvas *c);

/* Zero the dirty box's pixels and forget it -- the "clear only what was drawn
 * last time" half of an incremental redraw. */
void psx_ui_dirty_clear(PsxUiCanvas *c);

/* Grow the dirty box over a rect. Only needed by callers that write pixels
 * without going through the helpers below (a raw image blit, say). */
void psx_ui_mark(PsxUiCanvas *c, int x, int y, int w, int h);

/* One pixel, `argb` scaled by `cov` (0..1) and composited over what is there.
 * Out-of-range coordinates and zero coverage are no-ops. */
void psx_ui_blend(PsxUiCanvas *c, int x, int y, uint32_t argb, float cov);

/* Axis-aligned rect, hard edges. Clipped to the canvas. */
void psx_ui_fill(PsxUiCanvas *c, int x, int y, int w, int h, uint32_t col);

/* Filled rounded rect. `r` is clamped to half the shorter side, so passing a
 * huge radius on a square gives a circle -- which is how the round button
 * glyphs are drawn rather than with a separate disc routine. */
void psx_ui_round_rect(PsxUiCanvas *c, int x, int y, int w, int h,
                       float r, uint32_t col);

/* Outline of a rounded rect, `width` px wide, centred on the edge. */
void psx_ui_round_rect_line(PsxUiCanvas *c, int x, int y, int w, int h,
                            float r, uint32_t col, float width);

/* Soft drop shadow OUTSIDE a rounded rect, falling off as the square of the
 * distance over `spread` px. What lifts a floating panel off the game. */
void psx_ui_round_rect_shadow(PsxUiCanvas *c, int x, int y, int w, int h,
                              float r, uint32_t col, int spread);

/* Antialiased line segment of the given width, round-capped. Float endpoints:
 * these come out of a layout scaled by a fractional factor, and rounding them
 * to pixels first is what makes a diagonal look hand-drawn. */
void psx_ui_line(PsxUiCanvas *c, float x0, float y0, float x1, float y1,
                 float width, uint32_t col);

/* Bilinear-sampled image blit into an arbitrary destination rect, with the
 * corners rounded off to radius `r` (0 for a hard rect).
 *
 * For the save-state thumbnails: a fixed small source landing in a box that
 * grows with the window, so point sampling turns them to mush at 3x -- and
 * sitting in a rounded well beside rounded rows, so square corners on the one
 * element that happens to be an image read as a mistake. The source is treated
 * as OPAQUE; only the rounded edge contributes partial alpha. */
void psx_ui_blit_scaled(PsxUiCanvas *c, int x, int y, int w, int h,
                        float r, const uint32_t *src, int sw, int sh);

/* Text, baseline-positioned, in `col` (its alpha scales glyph coverage).
 * Returns the x the caller would continue at. NULL text or face draws
 * nothing and returns `x`, so a failed face bake costs the label, not the
 * layout. */
int psx_ui_text(PsxUiCanvas *c, int x, int baseline, const char *s,
                uint32_t col, const PsxUiFace *f);

/* Text that must not exceed `max_w`, ending in an ellipsis when it would. */
void psx_ui_text_clip(PsxUiCanvas *c, int x, int baseline, const char *s,
                      uint32_t col, const PsxUiFace *f, int max_w);

/* Baseline that centres a face's INK inside a band [y, y+h). Uses ascent and
 * descent rather than the line height: a font's line box carries leading that
 * is not part of any glyph and would push short text visibly low. */
int psx_ui_baseline_in(int y, int h, const PsxUiFace *f);

#ifdef __cplusplus
}
#endif

#endif /* PSX_UI_DRAW_H */
