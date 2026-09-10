/* psx_savestate_menu.c - full-screen user save-state slot browser.
 *
 * WHAT CHANGED, AND WHY IT MATTERS TO ANYONE EDITING THIS
 *
 * This panel used to author ONE fixed 640x480 image, whatever the window was,
 * and let the renderer stretch it across the whole drawable. That is why its
 * text was always soft: an 8x8 bitmap sheet blown up by three and a bit. It
 * now works the way psx_video_menu.c does --
 *
 *  - LAYOUT IS IN DESIGN UNITS, one unit being 1/480 of the canvas HEIGHT.
 *    S() converts. Every vertical number below is the number this file always
 *    used, because the old canvas was exactly 480 tall and the two coordinate
 *    spaces therefore coincide. Never write a raw pixel count into layout code
 *    here -- it will be right on one monitor.
 *  - THE CANVAS IS THE WINDOW'S OWN SIZE (psx_savestate_menu_set_layout), so
 *    text is rasterised at the resolution it will be seen at, through
 *    psx_ui_font: antialiased, proportional, and mixed case.
 *
 * Width is the part that could not simply be scaled. The old canvas was 4:3
 * and the stretch distorted horizontally to whatever the window was; the
 * content is now laid out in a COLUMN of fixed design width, centred, with the
 * header and footer bands running full bleed behind it. On a 4:3 window that
 * reproduces the old margins exactly; on a wider one the rows stay a readable
 * width instead of growing to the full span of somebody's ultrawide.
 *
 * STILL FULLY OPAQUE, edge to edge, and that is load-bearing rather than
 * inherited: Vulkan composites this through a plain buffer-to-image copy that
 * does not blend, so a translucent scrim would read as glass on GL and as flat
 * paint there. It is a modal screen with the guest frozen behind it, so there
 * is nothing to see through anyway.
 *
 * No dirty-box bookkeeping, unlike the menu: an opaque panel repaints every
 * pixel it owns on every pass, so there is nothing a previous pass could have
 * left behind to clear. Those passes only happen when the selection, the hover
 * or the slot contents change -- never merely because a frame went by. */

#include "psx_savestate_menu.h"

#include "host_keymap.h"
#include "psx_ui_draw.h"
#include "psx_video_menu.h"
#include "savestate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- canvas --------------------------------------------------------------
 *
 * Allocated once at the cap and never moved, for the same reason the menu's
 * is: the pointer goes out to whichever thread is presenting, so reallocating
 * under it is a use-after-free rather than a resize. Taken lazily, on the
 * first layout that needs it, because most sessions never open this screen and
 * 33 MB is a lot to hold for a browser nobody visited.
 *
 * The static fallback is what a failed allocation degrades to: a smaller
 * canvas the renderer stretches, which is exactly the UI this replaced rather
 * than no UI at all. */
#define SSM_MAX_W 3840
#define SSM_MAX_H 2160
#define SSM_FB_W  1280
#define SSM_FB_H   720

/* ---- layout, in DESIGN UNITS (1 unit = 1/480 of canvas height) ----------- */

#define SSM_HEAD_H       46.0f   /* header band, below the inset */
#define SSM_TITLE_Y      14.0f
#define SSM_TITLE_CLEARANCE 4.0f
#define SSM_COUNT_GAP    14.0f   /* title -> the "01-03 / 12" counter beside it */

#define SSM_CONTENT_W   584.0f   /* the column everything is laid out in */
#define SSM_EDGE_PAD     24.0f   /* minimum gutter when the window is narrow */

#define SSM_ROWS_Y       58.0f
#define SSM_ROW_H       108.0f
#define SSM_ROW_GAP       6.0f
#define SSM_ROW_R        10.0f
#define SSM_ROW_PAD      20.0f
#define SSM_VISIBLE_ROWS  3
#define SSM_THUMB_X     118.0f   /* from the row's left edge */
#define SSM_THUMB_Y       3.0f
#define SSM_THUMB_W     136.0f
#define SSM_THUMB_H     102.0f
#define SSM_THUMB_R       6.0f
#define SSM_META_X      278.0f   /* date column, right of the thumb */

/* Last row's bottom edge, and the top of the footer band it must not reach. */
#define SSM_ROWS_BOTTOM (SSM_ROWS_Y + SSM_VISIBLE_ROWS * (SSM_ROW_H + SSM_ROW_GAP) \
                         - SSM_ROW_GAP)
#define SSM_FOOTER_Y    410.0f
#define SSM_FOOT_GLYPH_Y 424.0f
#define SSM_FOOT_GLYPH   18.0f   /* button-glyph box, square */
#define SSM_FOOT_GAP      7.0f   /* glyph -> its word */
#define SSM_FOOT_STEP    28.0f   /* word -> the next glyph */
#define SSM_FOOT_HIT_PAD  5.0f   /* slop around a legend's own extent */
#define SSM_KEYS_Y      452.0f

#define SSM_CLOSE        22.0f
#define SSM_CLOSE_R       6.0f
#define SSM_CLOSE_Y       12.0f  /* below the header inset */

/* ---- type ----------------------------------------------------------------
 *
 * The two smallest sizes are deliberately the menu's own (VM_FS_ROW and
 * VM_FS_HINT): an identical pixel size shares a baked face rather than adding
 * one, and psx_ui_font's cache is sized against the total across every
 * overlay, not against any one of them. */
#define SSM_FS_TITLE     17.0f
#define SSM_FS_ROW       11.0f
#define SSM_FS_META       9.5f
#define SSM_FS_FOOT      10.0f
#define SSM_FS_HINT       8.8f

/* ---- palette -------------------------------------------------------------
 *
 * The panel keeps its amber accent rather than borrowing the F10 bar's blue:
 * the bar is composited ON TOP of this and highlights its own open menu, so
 * two different things being "active" at once want two different colours. */
#define SSM_COL_BACK     0xFF0F1118u
#define SSM_COL_BAND     0xFF171B25u
#define SSM_COL_ROW      0xFF191D27u
#define SSM_COL_ROW_HOV  0xFF222735u
#define SSM_COL_ROW_SEL  0xFF2B2830u
#define SSM_COL_EDGE     0xFF303746u
#define SSM_COL_EDGE_HOV 0xFF4A5468u
#define SSM_COL_WELL_ED  0xFF3A4352u
#define SSM_COL_ACCENT   0xFFFFD24Du
#define SSM_COL_TEXT     0xFFE2E5EBu
#define SSM_COL_SUB      0xFFB2B8C2u
#define SSM_COL_DIM      0xFFB8BDC8u
#define SSM_COL_FAINT    0xFF7F8796u
#define SSM_COL_WELL     0xFF242A35u   /* empty thumbnail well */
#define SSM_COL_WELL_TX  0xFF707887u
#define SSM_COL_CLOSE_BG 0xFF3A2530u

/* PlayStation face-button tints, unchanged: they are the console's, not this
 * panel's, and a player matches them against the pad in their hands. */
#define SSM_COL_CROSS    0xFF5FA8FFu
#define SSM_COL_SQUARE   0xFFFF7EB6u
#define SSM_COL_CIRCLE   0xFFFF6B6Bu
#define SSM_COL_DPAD     0xFFD7DCE6u

static uint32_t  s_panel_fb[SSM_FB_W * SSM_FB_H];
static uint32_t *s_panel = s_panel_fb;
static int       s_cap_w = SSM_FB_W;
static int       s_cap_h = SSM_FB_H;
static int       s_lw = 640, s_lh = 480;
static float     s_unit = 1.0f;

static int s_open;
static int s_selected;
static int s_dirty = 1;
static uint32_t s_thumbs[SAVESTATE_SLOTS][SAVESTATE_THUMB_W * SAVESTATE_THUMB_H];
static int s_have_thumb[SAVESTATE_SLOTS];
static int s_close_hover;
static int s_hover_row = -1;      /* absolute slot under the cursor, or -1 */
static int s_hover_action;        /* PSX_SSM_ACTION_* under the cursor */

/* Rounded to whole pixels. Layout that lands on half-pixels puts a row's
 * highlight one pixel off from the row it highlights. */
static int S(float design)
{
    return (int)(design * s_unit + 0.5f);
}

/* Hairline width for outlines: one pixel until the canvas is big enough that
 * one pixel disappears, then a fraction of the scale. */
static float hairline(void)
{
    return s_unit < 2.0f ? 1.0f : s_unit * 0.7f;
}

static float unit_for(int canvas_h)
{
    float u = (float)canvas_h / 480.0f;
    if (u < 1.0f) u = 1.0f;
    if (u > 8.0f) u = 8.0f;
    return u;
}

static const PsxUiFace *face_title(void)
{
    return psx_ui_font_face(SSM_FS_TITLE * s_unit, PSX_UI_FONT_SEMIBOLD);
}
static const PsxUiFace *face_row(void)
{
    return psx_ui_font_face(SSM_FS_ROW * s_unit, PSX_UI_FONT_SEMIBOLD);
}
static const PsxUiFace *face_meta(void)
{
    return psx_ui_font_face(SSM_FS_META * s_unit, PSX_UI_FONT_REGULAR);
}
static const PsxUiFace *face_foot(void)
{
    return psx_ui_font_face(SSM_FS_FOOT * s_unit, PSX_UI_FONT_REGULAR);
}
static const PsxUiFace *face_hint(void)
{
    return psx_ui_font_face(SSM_FS_HINT * s_unit, PSX_UI_FONT_REGULAR);
}

/* Rows this panel's header must drop to clear the F10 menu bar.
 *
 * The bar is composited ON TOP of this panel (see gl_swap_with_osd — the bar is
 * deliberately the last, topmost layer), and this panel is drawn from y=0, so
 * the bar lands squarely across the title. Insetting the panel's DESTINATION
 * rect instead would fix the overlap but move the canvas-to-window mapping
 * that the hit-testing below reads back; the shift therefore happens here, in
 * layout, where it is one number that both halves already share.
 *
 * Derived rather than a magic 12: the title already carries SSM_TITLE_Y of
 * padding, so only the shortfall against the bar needs making up, and the
 * answer tracks the bar's own height automatically. Clamped so a taller bar
 * can never push the last slot row into the footer band — the panel only has
 * SSM_FOOTER_Y - SSM_ROWS_BOTTOM of slack to give.
 *
 * psx_video_menu_bar_height() reports DESIGN UNITS against a 480-tall screen,
 * which is the space every constant in this file is in, so the arithmetic
 * stays unit-clean and only its result is scaled. That was true when this
 * canvas was a literal 640x480 and it is still true now the canvas tracks the
 * window, because a design unit is defined off the canvas HEIGHT either way.
 *
 * Unconditional on the bar being visible: with it hidden this is 12 extra
 * units of top padding that nobody reads as wrong, and paying that costs far
 * less than re-rasterising the panel whenever the bar is toggled. */
static float header_inset_u(void)
{
    float need = (float)psx_video_menu_bar_height() + SSM_TITLE_CLEARANCE
                 - SSM_TITLE_Y;
    const float slack = SSM_FOOTER_Y - SSM_ROWS_BOTTOM;
    if (need < 0.0f) need = 0.0f;
    if (need > slack) need = slack;
    return need;
}

static int header_inset(void)
{
    return S(header_inset_u());
}

/* Left edge and width of the column everything is laid out in. Centred, and
 * narrowed to fit when the canvas is too slim to hold it with a gutter. */
static int content_w(void)
{
    int w = S(SSM_CONTENT_W);
    const int lim = s_lw - S(SSM_EDGE_PAD) * 2;
    if (w > lim) w = lim;
    if (w < 1) w = 1;
    return w;
}

static int content_x(void)
{
    int x = (s_lw - content_w()) / 2;
    return x < 0 ? 0 : x;
}

/* Close button, top-right of the header band.
 *
 * Sits BELOW the F10 menu bar rather than beside it: the bar spans the full
 * width of the window and is composited on top of this panel, so anything
 * drawn in the top rows would be buried under it. header_inset() is exactly
 * that clearance, which is why the button hangs off it. Anchored to the
 * content column rather than to the canvas edge, so on an ultrawide it stays
 * beside the thing it closes instead of stranded in the corner. */
static void close_rect(int *x, int *y, int *w, int *h)
{
    *w = S(SSM_CLOSE);
    *h = S(SSM_CLOSE);
    *x = content_x() + content_w() - *w;
    *y = header_inset() + S(SSM_CLOSE_Y);
}

/* ---- footer legends ------------------------------------------------------
 *
 * A button glyph, then its word, laid out left to right from the content
 * column. ONE function drives both the drawing and the hit-testing: they used
 * to be two lists of x positions with a comment asking the next person to keep
 * them in step, which is a bug with a waiting period — and they cannot be
 * constants at all now, because a proportional word is not a fixed number of
 * pixels wide.
 *
 * The hit box covers the glyph AND the word: the player aims at "Load", not at
 * the 18-unit sprite beside it. */
static const char *foot_label(int i)
{
    switch (i) {
        case 0:  return "Slot";
        case 1:  return "Load";
        case 2:  return "Save";
        default: return "Back";
    }
}

/* Legend i's box, glyph included. Returns 0 past the last one. */
static int foot_rect(int i, int *x, int *y, int *w, int *h)
{
    const PsxUiFace *f = face_foot();
    int cx = content_x(), k;
    if (i < 0 || i > 3) return 0;
    for (k = 0; k < i; k++)
        cx += S(SSM_FOOT_GLYPH) + S(SSM_FOOT_GAP)
              + psx_ui_font_text_w(f, foot_label(k)) + S(SSM_FOOT_STEP);
    *x = cx;
    *y = S(SSM_FOOT_GLYPH_Y);
    *w = S(SSM_FOOT_GLYPH) + S(SSM_FOOT_GAP)
         + psx_ui_font_text_w(f, foot_label(i));
    *h = S(SSM_FOOT_GLYPH);
    return 1;
}

/* Legend index for a PSX_SSM_ACTION_*, or -1 for "not a legend". Slot (index
 * 0) is a hint, not a button: there is nothing to click it for. */
static int foot_index_for_action(int action)
{
    switch (action) {
        case PSX_SSM_ACTION_LOAD: return 1;
        case PSX_SSM_ACTION_SAVE: return 2;
        case PSX_SSM_ACTION_BACK: return 3;
        default: return -1;
    }
}

static int action_rect(int action, int *x, int *y, int *w, int *h)
{
    const int i = foot_index_for_action(action);
    int pad;
    if (i < 0 || !foot_rect(i, x, y, w, h)) return 0;
    pad = S(SSM_FOOT_HIT_PAD);
    *x -= pad;
    *y -= pad;
    *w += pad * 2;
    *h += pad * 2;
    return 1;
}

/* Index of the top visible row. The list scrolls to keep the selection in the
 * middle of the three rows, clamped at both ends. Shared by the rasterizer and
 * the hit-test so a click always lands on the row the player can see. */
static int first_visible(int selected)
{
    int first = selected - 1;
    if (first < 0) first = 0;
    if (first > SAVESTATE_SLOTS - SSM_VISIBLE_ROWS)
        first = SAVESTATE_SLOTS - SSM_VISIBLE_ROWS;
    return first;
}

/* Canvas coords for a window position. The panel is one canvas stretched
 * across the whole drawable, so it maps back by simple ratio — which holds
 * whether the canvas matches the surface (the usual case now) or is the
 * smaller fallback. */
static int to_canvas(int x, int y, int surface_w, int surface_h,
                     int *px, int *py)
{
    if (surface_w <= 0 || surface_h <= 0) return 0;
    *px = (int)((long)x * s_lw / surface_w);
    *py = (int)((long)y * s_lh / surface_h);
    return 1;
}

int psx_savestate_menu_hit_close(int x, int y, int surface_w, int surface_h)
{
    int cx, cy, cw, ch, px, py;
    if (!s_open) return 0;
    if (!to_canvas(x, y, surface_w, surface_h, &px, &py)) return 0;
    close_rect(&cx, &cy, &cw, &ch);
    return px >= cx && px < cx + cw && py >= cy && py < cy + ch;
}

int psx_savestate_menu_hit_slot(int x, int y, int surface_w, int surface_h)
{
    int px, py, visible;
    const int top = header_inset();
    const int first = first_visible(s_selected);
    const int rx = content_x(), rw = content_w();
    if (!s_open) return -1;
    if (!to_canvas(x, y, surface_w, surface_h, &px, &py)) return -1;
    if (px < rx || px >= rx + rw) return -1;
    for (visible = 0; visible < SSM_VISIBLE_ROWS; visible++) {
        const int ry = S(SSM_ROWS_Y) + top
                       + visible * (S(SSM_ROW_H) + S(SSM_ROW_GAP));
        if (py >= ry && py < ry + S(SSM_ROW_H)) {
            const int slot = first + visible;
            return (slot >= 0 && slot < SAVESTATE_SLOTS) ? slot : -1;
        }
    }
    return -1;
}

int psx_savestate_menu_hit_action(int x, int y, int surface_w, int surface_h)
{
    int px, py, a;
    if (!s_open) return PSX_SSM_ACTION_NONE;
    if (!to_canvas(x, y, surface_w, surface_h, &px, &py))
        return PSX_SSM_ACTION_NONE;
    for (a = PSX_SSM_ACTION_LOAD; a <= PSX_SSM_ACTION_BACK; a++) {
        int ax, ay, aw, ah;
        if (!action_rect(a, &ax, &ay, &aw, &ah)) continue;
        if (px >= ax && px < ax + aw && py >= ay && py < ay + ah)
            return a;
    }
    return PSX_SSM_ACTION_NONE;
}

void psx_savestate_menu_hover(int x, int y, int surface_w, int surface_h)
{
    const int hit = psx_savestate_menu_hit_close(x, y, surface_w, surface_h);
    const int row = psx_savestate_menu_hit_slot(x, y, surface_w, surface_h);
    const int act = psx_savestate_menu_hit_action(x, y, surface_w, surface_h);
    /* Only redraw on a real change: a moving cursor would otherwise re-raster
     * the whole panel every frame, and the panel is now the size of the window
     * rather than a fixed 640x480. */
    if (hit == s_close_hover && row == s_hover_row && act == s_hover_action)
        return;
    s_close_hover = hit;
    s_hover_row = row;
    s_hover_action = act;
    s_dirty = 1;
}

/* ---- drawing ------------------------------------------------------------- */

/* One PlayStation face button, filling a square box of side `d`.
 *
 * Drawn as strokes rather than as glyphs: the embedded icon face carries no
 * controller symbols, and these want the console's own colours, which a single
 * text glyph cannot give. Every measurement is a fraction of `d` so the shapes
 * hold their proportions at whatever size the box comes out. */
static void draw_psx_button(PsxUiCanvas *c, int x, int y, int d, char kind)
{
    const float f = (float)d;
    float stroke = f * 0.13f;
    if (stroke < 1.5f) stroke = 1.5f;
    switch (kind) {
    case 'x':
        psx_ui_line(c, (float)x + f * 0.27f, (float)y + f * 0.27f,
                       (float)x + f * 0.73f, (float)y + f * 0.73f,
                    stroke, SSM_COL_CROSS);
        psx_ui_line(c, (float)x + f * 0.73f, (float)y + f * 0.27f,
                       (float)x + f * 0.27f, (float)y + f * 0.73f,
                    stroke, SSM_COL_CROSS);
        break;
    case 's': {
        const int in = (int)(f * 0.22f + 0.5f);
        psx_ui_round_rect_line(c, x + in, y + in, d - in * 2, d - in * 2,
                               f * 0.10f, SSM_COL_SQUARE, stroke);
        break;
    }
    case 'o': {
        const int in = (int)(f * 0.16f + 0.5f);
        /* Radius past half the side is clamped to a circle by the primitive. */
        psx_ui_round_rect_line(c, x + in, y + in, d - in * 2, d - in * 2,
                               f, SSM_COL_CIRCLE, stroke);
        break;
    }
    default: {
        /* D-pad: a plus, arms about as thick as the strokes above. */
        int arm = (int)(f * 0.24f + 0.5f);
        int len = (int)(f * 0.92f + 0.5f);
        int off, mid;
        if (arm < 2) arm = 2;
        if (len < arm) len = arm;
        off = (d - len) / 2;
        mid = (d - arm) / 2;
        psx_ui_round_rect(c, x + mid, y + off, arm, len, (float)arm * 0.35f,
                          SSM_COL_DPAD);
        psx_ui_round_rect(c, x + off, y + mid, len, arm, (float)arm * 0.35f,
                          SSM_COL_DPAD);
        break;
    }
    }
}

static void format_slot_status(int slot, char *out, size_t cap)
{
    int64_t mt64 = 0;
    time_t mt;
    struct tm tmv;
    if (!out || cap == 0) return;
    if (!savestate_slot_mtime(slot, &mt64)) {
        snprintf(out, cap, "New slot");
        return;
    }
    mt = (time_t)mt64;
#ifdef _WIN32
    localtime_s(&tmv, &mt);
#else
    localtime_r(&mt, &tmv);
#endif
    strftime(out, cap, "%Y-%m-%d %H:%M", &tmv);
}

static void refresh_thumbs(void)
{
    int i;
    for (i = 0; i < SAVESTATE_SLOTS; i++) {
        s_have_thumb[i] =
            savestate_read_thumb(i, s_thumbs[i], SAVESTATE_THUMB_W,
                                 SAVESTATE_THUMB_H);
    }
}

#define ACTION_FG(a) ((s_hover_action == (a)) ? SSM_COL_ACCENT : SSM_COL_TEXT)

static void draw_close_button(PsxUiCanvas *c, const PsxUiFace *fh,
                              const char *keyhint)
{
    int x, y, w, h;
    const uint32_t fg = s_close_hover ? SSM_COL_ACCENT : SSM_COL_DIM;
    float pad, stroke;

    close_rect(&x, &y, &w, &h);

    /* Keybind reminder, right-aligned into the gap before the button, so it
     * can never collide with the title however long the bound key's name is. */
    if (keyhint && keyhint[0]) {
        const int tw = psx_ui_font_text_w(fh, keyhint);
        psx_ui_text(c, x - S(10.0f) - tw, psx_ui_baseline_in(y, h, fh),
                    keyhint, SSM_COL_FAINT, fh);
    }

    if (s_close_hover)
        psx_ui_round_rect(c, x, y, w, h, SSM_CLOSE_R * s_unit,
                          SSM_COL_CLOSE_BG);
    psx_ui_round_rect_line(c, x, y, w, h, SSM_CLOSE_R * s_unit,
                           s_close_hover ? SSM_COL_ACCENT : SSM_COL_WELL_ED,
                           hairline());
    /* Two strokes rather than the letter X: at this size a glyph reads as a
     * character in a word, not as a control. */
    pad = (float)w * 0.32f;
    stroke = (float)w * 0.09f;
    if (stroke < 1.4f) stroke = 1.4f;
    psx_ui_line(c, (float)x + pad, (float)y + pad,
                   (float)(x + w) - pad, (float)(y + h) - pad, stroke, fg);
    psx_ui_line(c, (float)(x + w) - pad, (float)y + pad,
                   (float)x + pad, (float)(y + h) - pad, stroke, fg);
}

static void draw_row(PsxUiCanvas *c, int slot, int y,
                     const PsxUiFace *fr, const PsxUiFace *fm,
                     const PsxUiFace *fh)
{
    const int cx = content_x(), cw = content_w();
    const int rowh = S(SSM_ROW_H);
    const int sel = (slot == s_selected);
    const int hov = (slot == s_hover_row);
    const uint32_t bg = sel ? SSM_COL_ROW_SEL
                            : (hov ? SSM_COL_ROW_HOV : SSM_COL_ROW);
    const uint32_t fg = sel ? SSM_COL_ACCENT : SSM_COL_TEXT;
    const uint32_t sub = sel ? 0xFFFFFFFFu : SSM_COL_SUB;
    const int tx = cx + S(SSM_THUMB_X), ty = y + S(SSM_THUMB_Y);
    const int tw = S(SSM_THUMB_W), th = S(SSM_THUMB_H);
    char buf[96];

    psx_ui_round_rect(c, cx, y, cw, rowh, SSM_ROW_R * s_unit, bg);
    psx_ui_round_rect_line(c, cx, y, cw, rowh, SSM_ROW_R * s_unit,
                           sel ? SSM_COL_ACCENT
                               : (hov ? SSM_COL_EDGE_HOV : SSM_COL_EDGE),
                           hairline());

    snprintf(buf, sizeof(buf), "Slot %02d", slot + 1);
    psx_ui_text(c, cx + S(SSM_ROW_PAD), y + S(18.0f) + psx_ui_font_ascent(fr),
                buf, fg, fr);

    if (s_have_thumb[slot]) {
        psx_ui_blit_scaled(c, tx, ty, tw, th, SSM_THUMB_R * s_unit,
                           s_thumbs[slot],
                           SAVESTATE_THUMB_W, SAVESTATE_THUMB_H);
    } else {
        const int w = psx_ui_font_text_w(fm, "New");
        psx_ui_round_rect(c, tx, ty, tw, th, SSM_THUMB_R * s_unit,
                          SSM_COL_WELL);
        psx_ui_text(c, tx + (tw - w) / 2, psx_ui_baseline_in(ty, th, fm),
                    "New", SSM_COL_WELL_TX, fm);
    }
    /* Outlined either way, so a written slot and an empty one are the same
     * shape and only their CONTENTS differ. */
    psx_ui_round_rect_line(c, tx, ty, tw, th, SSM_THUMB_R * s_unit,
                           SSM_COL_WELL_ED, hairline());

    format_slot_status(slot, buf, sizeof(buf));
    psx_ui_text(c, cx + S(SSM_META_X), y + S(42.0f) + psx_ui_font_ascent(fm),
                buf, sub, fm);

    if (sel) {
        const int w = psx_ui_font_text_w(fh, "Selected");
        psx_ui_text(c, cx + cw - S(SSM_ROW_PAD) - w, y + rowh - S(14.0f),
                    "Selected", SSM_COL_ACCENT, fh);
    }
}

static void draw_footer(PsxUiCanvas *c, const PsxUiFace *ff,
                        const PsxUiFace *fh)
{
    /* Index order matches foot_label / foot_rect. */
    static const char KIND[4] = { 'd', 'x', 's', 'o' };
    static const int  ACT[4]  = { PSX_SSM_ACTION_NONE, PSX_SSM_ACTION_LOAD,
                                  PSX_SSM_ACTION_SAVE, PSX_SSM_ACTION_BACK };
    const int gd = S(SSM_FOOT_GLYPH);
    const int cx = content_x();
    int i;

    psx_ui_fill(c, 0, S(SSM_FOOTER_Y), s_lw, s_lh - S(SSM_FOOTER_Y),
                SSM_COL_BAND);
    for (i = 0; i < 4; i++) {
        int fx, fy, fw, fhh;
        if (!foot_rect(i, &fx, &fy, &fw, &fhh)) continue;
        draw_psx_button(c, fx, fy, gd, KIND[i]);
        psx_ui_text(c, fx + gd + S(SSM_FOOT_GAP),
                    psx_ui_baseline_in(fy, gd, ff), foot_label(i),
                    i == 0 ? SSM_COL_TEXT : ACTION_FG(ACT[i]), ff);
    }
    /* U+2022 bullets as separators — in the font's embedded subset, unlike the
     * middot, and the one punctuation that survives being this small. */
    psx_ui_text_clip(c, cx, S(SSM_KEYS_Y) + psx_ui_font_ascent(fh),
                     "Arrows: slot \xE2\x80\xA2 Enter or L: load \xE2\x80\xA2 "
                     "Shift+Enter or S: save \xE2\x80\xA2 Esc: back",
                     SSM_COL_DIM, fh, s_lw - cx * 2);
}

static void rasterize_panel(void)
{
    PsxUiCanvas c;
    const PsxUiFace *ft = face_title(), *fr = face_row(), *fm = face_meta();
    const PsxUiFace *ff = face_foot(),  *fh = face_hint();
    const int top = header_inset();
    const int cx = content_x();
    int i, first;
    char buf[128];
    char key[32];

    c.px = s_panel;
    c.w  = s_lw;
    c.h  = s_lh;
    psx_ui_dirty_reset(&c);

    psx_ui_fill(&c, 0, 0, s_lw, s_lh, SSM_COL_BACK);
    psx_ui_fill(&c, 0, 0, s_lw, S(SSM_HEAD_H) + top, SSM_COL_BAND);

    refresh_thumbs();
    first = first_visible(s_selected);

    /* Title, and the scroll counter on its BASELINE rather than on a line of
     * its own below it. The old layout put the counter at design y 42 in a
     * band that ended at 46, so it hung half out of the band onto the panel
     * behind -- invisible in 8x8 blocks, obvious the moment the text got
     * edges. Beside the title it also stops being a stray third line in a
     * header that only has two things to say. */
    {
        const int base = S(SSM_TITLE_Y) + top + psx_ui_font_ascent(ft);
        const int end = psx_ui_text(&c, cx, base, "Save states",
                                    SSM_COL_ACCENT, ft);
        snprintf(buf, sizeof(buf), "%02d-%02d / %02d",
                 first + 1, first + SSM_VISIBLE_ROWS, SAVESTATE_SLOTS);
        psx_ui_text(&c, end + S(SSM_COUNT_GAP), base, buf, SSM_COL_FAINT, fh);
    }

    host_keymap_label(HOST_KEYMAP_SAVE_STATE_MENU, key, sizeof(key));
    snprintf(buf, sizeof(buf), "%s Menu", key[0] ? key : "F7");
    draw_close_button(&c, fh, buf);

    for (i = first; i < first + SSM_VISIBLE_ROWS; i++)
        draw_row(&c, i,
                 S(SSM_ROWS_Y) + top
                     + (i - first) * (S(SSM_ROW_H) + S(SSM_ROW_GAP)),
                 fr, fm, fh);

    draw_footer(&c, ff, fh);
    s_dirty = 0;
}

/* ---- public API ---------------------------------------------------------- */

void psx_savestate_menu_set_layout(int surface_w, int surface_h)
{
    if (surface_w < 160) surface_w = 160;
    if (surface_h < 120) surface_h = 120;

    /* Take the big canvas on first use rather than at startup: most sessions
     * never open this screen, and this is the thread that will draw into it. */
    if (s_panel == s_panel_fb &&
        (surface_w > SSM_FB_W || surface_h > SSM_FB_H)) {
        uint32_t *p = (uint32_t *)malloc((size_t)SSM_MAX_W * (size_t)SSM_MAX_H
                                         * sizeof(uint32_t));
        if (p) {
            s_panel = p;
            s_cap_w = SSM_MAX_W;
            s_cap_h = SSM_MAX_H;
        }
    }
    /* Clamp to what was actually ALLOCATED, not to SSM_MAX_*: if the big
     * canvas could not be had, writing past the fallback is the one bug in
     * this module that would be a crash rather than a cosmetic fault. Every
     * backend stretches whatever it is handed, so a clamped canvas degrades to
     * the soft-but-present UI this replaced rather than to a missing one. */
    if (surface_w > s_cap_w) surface_w = s_cap_w;
    if (surface_h > s_cap_h) surface_h = s_cap_h;
    if (s_lw == surface_w && s_lh == surface_h) return;
    s_lw = surface_w;
    s_lh = surface_h;
    s_unit = unit_for(surface_h);
    s_dirty = 1;
}

void psx_savestate_menu_set_state(int open, int selected_slot)
{
    if (selected_slot < 0) selected_slot = 0;
    if (selected_slot >= SAVESTATE_SLOTS) selected_slot = SAVESTATE_SLOTS - 1;
    if (s_open != (open ? 1 : 0) || s_selected != selected_slot)
        s_dirty = 1;
    if (!open && s_open) {
        /* Stale highlights would otherwise be baked into the first frame of
         * the next open, before any mouse motion arrives to correct them. */
        s_hover_row = -1;
        s_hover_action = PSX_SSM_ACTION_NONE;
        s_close_hover = 0;
    }
    s_open = open ? 1 : 0;
    s_selected = selected_slot;
}

void psx_savestate_menu_note_slots_changed(void)
{
    s_dirty = 1;
}

int psx_savestate_menu_needs_present(void)
{
    return s_open;
}

int psx_savestate_menu_overlay_image(const uint32_t **pixels, int *w, int *h)
{
    if (!s_open) {
        if (pixels) *pixels = NULL;
        if (w) *w = 0;
        if (h) *h = 0;
        return 0;
    }
    if (s_dirty)
        rasterize_panel();
    if (pixels) *pixels = s_panel;
    if (w) *w = s_lw;
    if (h) *h = s_lh;
    return 1;
}
