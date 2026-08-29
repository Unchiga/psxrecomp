#ifndef PSX_UI_FONT_H
#define PSX_UI_FONT_H

/* Antialiased proportional text for host overlays, rasterised on the CPU into
 * an ARGB canvas.
 *
 * This exists because the overlays used to share one 8x8 bitmap sheet covering
 * ASCII 32..90 -- uppercase only, one fixed cell, magnified by a whole number.
 * That is most of why they looked like 1997. Here the glyphs come from a real
 * typeface (Inter, subset and embedded; see tools/make_ui_font.py) rasterised
 * by stb_truetype at whatever pixel size the caller asks for, so a bigger
 * window gets more detail rather than bigger blocks.
 *
 * A "face" is one (pixel size, weight) pair. Faces are baked lazily into an
 * 8-bit coverage atlas and cached, because rasterising is far too slow to do
 * per frame -- and the callers redraw only when their state changes anyway.
 *
 * NOT thread-safe: the cache is shared mutable state. Every caller today
 * rasterises from the thread that owns its UI state (for the F10 menu that is
 * the emu thread, which is why the presentation thread reads a prepared canvas
 * instead of drawing one). Keep it that way. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Faces. ICONS is Material Symbols Outlined rather than a text weight: it is
 * reached through the same rasteriser and the same cache, so an icon costs no
 * second code path -- draw it by passing its codepoint (PSX_UI_ICON_*) as a
 * one-character UTF-8 string. A codepoint the icon face does not carry draws
 * nothing at all, rather than a tofu box. */
enum { PSX_UI_FONT_REGULAR = 0, PSX_UI_FONT_SEMIBOLD = 1, PSX_UI_FONT_ICONS = 2 };

/* Every icon in the embedded subset. Adding one means adding it to ICONS in
 * tools/make_ui_font.py and to UF_ICON_CP in psx_ui_font.c as well -- present
 * in only one of the three, it silently does not draw. */
#define PSX_UI_ICON_FOLDER      0xE2C7   /* folder */
#define PSX_UI_ICON_EYE         0xE417   /* visibility */
#define PSX_UI_ICON_MONITOR     0xEF5B   /* monitor */
#define PSX_UI_ICON_VOLUME      0xE050   /* volume_up */
#define PSX_UI_ICON_GAMEPAD     0xE6EC   /* sports_esports */
#define PSX_UI_ICON_BOLT        0xEA0B   /* bolt */
#define PSX_UI_ICON_EXTENSION   0xE87B   /* extension */
#define PSX_UI_ICON_TUNE        0xE429   /* tune */

/* UTF-8 encode one codepoint into `out` (at least 5 bytes) and return it, so a
 * caller can hand an icon straight to psx_ui_font_draw. */
char *psx_ui_font_utf8(unsigned cp, char *out);

typedef struct PsxUiFace PsxUiFace;

/* A face at `px` pixels per em, baking it on first use. Returns NULL when the
 * embedded font will not load or the cache is exhausted -- every caller must
 * cope, because a menu that cannot draw its own text is a menu the player
 * cannot use to undo whatever brought them there. */
const PsxUiFace *psx_ui_font_face(float px, int family);

/* Vertical metrics of a face, in whole pixels. `ascent` is baseline-to-top and
 * is what a caller adds to a box's top edge to get a baseline; `line` is the
 * font's natural line spacing. */
int psx_ui_font_ascent(const PsxUiFace *f);
int psx_ui_font_descent(const PsxUiFace *f);
int psx_ui_font_line_height(const PsxUiFace *f);

/* Advance width of a UTF-8 string, in whole pixels. */
int psx_ui_font_text_w(const PsxUiFace *f, const char *utf8);

/* Advance width of the first `bytes` bytes -- for measuring a prefix without
 * copying it out. */
int psx_ui_font_text_w_n(const PsxUiFace *f, const char *utf8, int bytes);

/* Blend `utf8` into the ARGB canvas with its baseline at `y`, left edge at
 * `x`. `argb` supplies the colour; its alpha scales the glyph coverage, so a
 * dimmed label is one call rather than a second colour. Clipped to the canvas.
 * Returns the x the caller would continue at. */
int psx_ui_font_draw(uint32_t *dst, int dst_w, int dst_h,
                     int x, int y, const char *utf8,
                     uint32_t argb, const PsxUiFace *f);

/* Drop every baked face. The caches are keyed by pixel size, so a window that
 * moves between displays otherwise keeps the sizes it no longer uses. Cheap:
 * the next draw re-bakes what it needs. */
void psx_ui_font_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* PSX_UI_FONT_H */
