/* psx_savestate_menu.c - full-screen user save-state slot browser. */

#include "psx_savestate_menu.h"

#include "host_keymap.h"
#include "psx_video_menu.h"
#include "savestate.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define SSM_W 640
#define SSM_H 480
#define SSM_ROW_H 108
#define SSM_ROW_GAP 6
#define SSM_ROWS_Y 58
#define SSM_ROWS_X 28
#define SSM_ROWS_W 584
#define SSM_VISIBLE_ROWS 3
#define SSM_THUMB_W 136
#define SSM_THUMB_H 102

/* Y of the title, and the gap the header wants above it. */
#define SSM_TITLE_Y 14
#define SSM_TITLE_CLEARANCE 4
/* Last row's bottom edge, and the top of the footer band it must not reach. */
#define SSM_ROWS_BOTTOM (SSM_ROWS_Y + SSM_VISIBLE_ROWS * (SSM_ROW_H + SSM_ROW_GAP) \
                         - SSM_ROW_GAP)
#define SSM_FOOTER_Y 410

/* Rows this panel's header must drop to clear the F10 menu bar.
 *
 * The bar is composited ON TOP of this panel (see gl_swap_with_osd — the bar is
 * deliberately the last, topmost layer), and this panel is drawn from y=0, so
 * the bar lands squarely across the title. Insetting the panel's DESTINATION
 * rect instead would fix the overlap but resample a 640x480 canvas onto a
 * non-multiple height, and the whole UI is 8x8 bitmap text blitted GL_NEAREST —
 * the stems go uneven. So the shift happens here, in layout, where the
 * canvas-to-window scale stays an integer.
 *
 * Derived rather than a magic 12: the title already carries SSM_TITLE_Y of
 * padding, so only the shortfall against the bar needs making up, and the
 * answer tracks VM_BAR_H automatically. Clamped so a taller bar can never push
 * the last slot row into the footer band — the panel only has
 * SSM_FOOTER_Y - SSM_ROWS_BOTTOM px of slack to give.
 *
 * Unconditional on the bar being visible: with it hidden this is 12 extra px of
 * top padding that nobody reads as wrong, and paying that costs far less than
 * re-rasterising the panel whenever the bar is toggled. */
static int header_inset(void)
{
    int need = psx_video_menu_bar_height() + SSM_TITLE_CLEARANCE - SSM_TITLE_Y;
    const int slack = SSM_FOOTER_Y - SSM_ROWS_BOTTOM;
    if (need < 0) need = 0;
    if (need > slack) need = slack;
    return need;
}

/* Close button, top-right of the header band.
 *
 * Sits BELOW the F10 menu bar rather than beside it: the bar spans the full
 * width of the window and is composited on top of this panel, so anything drawn
 * in the top VM_BAR_H rows would be buried under it. header_inset() is already
 * exactly that clearance, which is why the button hangs off it. */
#define SSM_CLOSE_W 22
#define SSM_CLOSE_H 22
#define SSM_CLOSE_MARGIN 14

static void close_rect(int *x, int *y, int *w, int *h)
{
    *x = SSM_W - SSM_CLOSE_W - SSM_CLOSE_MARGIN;
    *y = header_inset() + 12;
    *w = SSM_CLOSE_W;
    *h = SSM_CLOSE_H;
}

/* Footer legend hit boxes, wide enough to cover the glyph AND its word: the
 * player aims at "LOAD", not at the 16px button sprite beside it. Kept beside
 * the draw calls' x positions (132 / 230 / 328) so the two cannot drift. */
#define SSM_FOOT_Y 418
#define SSM_FOOT_H 28
#define SSM_FOOT_W 76

static int action_rect(int action, int *x, int *y, int *w, int *h)
{
    switch (action) {
        case PSX_SSM_ACTION_LOAD: *x = 128; break;
        case PSX_SSM_ACTION_SAVE: *x = 226; break;
        case PSX_SSM_ACTION_BACK: *x = 324; break;
        default: return 0;
    }
    *y = SSM_FOOT_Y;
    *w = SSM_FOOT_W;
    *h = SSM_FOOT_H;
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

/* Canvas coords for a window position. The panel is a fixed 640x480 canvas
 * stretched across the whole drawable, so it maps back by simple ratio. */
static int to_canvas(int x, int y, int surface_w, int surface_h,
                     int *px, int *py)
{
    if (surface_w <= 0 || surface_h <= 0) return 0;
    *px = (int)((long)x * SSM_W / surface_w);
    *py = (int)((long)y * SSM_H / surface_h);
    return 1;
}

/* Public-domain 8x8 ASCII 32..90 subset from font8x8_basic. */
static const uint8_t FONT8[59][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00},
    {0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00}, {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00},
    {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00}, {0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00},
    {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00}, {0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00},
    {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00}, {0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00},
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, {0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06}, {0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00}, {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00},
    {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00}, {0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00},
    {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00}, {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00},
    {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00}, {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00},
    {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00}, {0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00},
    {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00}, {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00},
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00}, {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06},
    {0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00}, {0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00},
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, {0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00},
    {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00}, {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00},
    {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00}, {0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00},
    {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00}, {0x7F,0x06,0x06,0x3E,0x06,0x06,0x7F,0x00},
    {0x7F,0x06,0x06,0x3E,0x06,0x06,0x06,0x00}, {0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00},
    {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00}, {0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00},
    {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00}, {0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00},
    {0x06,0x06,0x06,0x06,0x06,0x06,0x7F,0x00}, {0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00},
    {0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00}, {0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00},
    {0x3F,0x66,0x66,0x3E,0x06,0x06,0x06,0x00}, {0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00},
    {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00}, {0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00},
    {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, {0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00},
    {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00}, {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00},
    {0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00}, {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00},
    {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00},
};

static int s_open;
static int s_selected;
static int s_dirty = 1;
static uint32_t s_panel[SSM_W * SSM_H];
static uint32_t s_thumbs[SAVESTATE_SLOTS][SAVESTATE_THUMB_W * SAVESTATE_THUMB_H];
static int s_have_thumb[SAVESTATE_SLOTS];
static int s_close_hover;
static int s_hover_row = -1;      /* absolute slot under the cursor, or -1 */
static int s_hover_action;        /* PSX_SSM_ACTION_* under the cursor */

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
    if (!s_open) return -1;
    if (!to_canvas(x, y, surface_w, surface_h, &px, &py)) return -1;
    if (px < SSM_ROWS_X || px >= SSM_ROWS_X + SSM_ROWS_W) return -1;
    for (visible = 0; visible < SSM_VISIBLE_ROWS; visible++) {
        const int ry = SSM_ROWS_Y + top + visible * (SSM_ROW_H + SSM_ROW_GAP);
        if (py >= ry && py < ry + SSM_ROW_H) {
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
     * the whole 640x480 panel every frame. */
    if (hit == s_close_hover && row == s_hover_row && act == s_hover_action)
        return;
    s_close_hover = hit;
    s_hover_row = row;
    s_hover_action = act;
    s_dirty = 1;
}

static void fill_rect(uint32_t *dst, int x0, int y0, int w, int h, uint32_t col)
{
    int x, y;
    if (x0 < 0) { w += x0; x0 = 0; }
    if (y0 < 0) { h += y0; y0 = 0; }
    if (x0 + w > SSM_W) w = SSM_W - x0;
    if (y0 + h > SSM_H) h = SSM_H - y0;
    if (w <= 0 || h <= 0) return;
    for (y = y0; y < y0 + h; y++)
        for (x = x0; x < x0 + w; x++)
            dst[y * SSM_W + x] = col;
}

static void stroke_rect(uint32_t *dst, int x, int y, int w, int h, uint32_t col)
{
    fill_rect(dst, x, y, w, 1, col);
    fill_rect(dst, x, y + h - 1, w, 1, col);
    fill_rect(dst, x, y, 1, h, col);
    fill_rect(dst, x + w - 1, y, 1, h, col);
}

static void fill_disc(uint32_t *dst, int cx, int cy, int r, uint32_t col)
{
    int x, y;
    const int rr = r * r;
    for (y = -r; y <= r; y++) {
        for (x = -r; x <= r; x++) {
            if (x * x + y * y > rr) continue;
            fill_rect(dst, cx + x, cy + y, 1, 1, col);
        }
    }
}

static void draw_line(uint32_t *dst, int x0, int y0, int x1, int y1,
                      int thickness, uint32_t col)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    int inset = thickness / 2;

    for (;;) {
        fill_rect(dst, x0 - inset, y0 - inset, thickness, thickness, col);
        if (x0 == x1 && y0 == y1)
            break;
        {
            int e2 = err * 2;
            if (e2 >= dy) {
                err += dy;
                x0 += sx;
            }
            if (e2 <= dx) {
                err += dx;
                y0 += sy;
            }
        }
    }
}

static void draw_psx_button(uint32_t *dst, int x, int y, char kind)
{
    switch (kind) {
    case 'x':
        draw_line(dst, x + 4, y + 4, x + 14, y + 14, 2, 0xFF5FA8FFu);
        draw_line(dst, x + 14, y + 4, x + 4, y + 14, 2, 0xFF5FA8FFu);
        break;
    case 's':
        stroke_rect(dst, x + 3, y + 3, 13, 13, 0xFFFF7EB6u);
        stroke_rect(dst, x + 4, y + 4, 11, 11, 0xFFFF7EB6u);
        break;
    case 'o':
        fill_disc(dst, x + 9, y + 9, 8, 0xFFFF6B6Bu);
        fill_disc(dst, x + 9, y + 9, 5, 0xFF171B25u);
        break;
    default:
        fill_rect(dst, x + 7, y + 2, 5, 15, 0xFFD7DCE6u);
        fill_rect(dst, x + 2, y + 7, 15, 5, 0xFFD7DCE6u);
        break;
    }
}

static void draw_char(uint32_t *dst, int x0, int y0, char c,
                      uint32_t col, int scale)
{
    int x, y, sx, sy;
    const uint8_t *g;
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    if (c < 32 || c > 90) c = '?';
    g = FONT8[(int)c - 32];
    for (y = 0; y < 8; y++) {
        uint8_t row = g[y];
        for (x = 0; x < 8; x++) {
            if ((row & (1u << x)) == 0) continue;
            for (sy = 0; sy < scale; sy++)
                for (sx = 0; sx < scale; sx++) {
                    int dx = x0 + x * scale + sx;
                    int dy = y0 + y * scale + sy;
                    if ((unsigned)dx < SSM_W && (unsigned)dy < SSM_H)
                        dst[dy * SSM_W + dx] = col;
                }
        }
    }
}

static void draw_text(uint32_t *dst, int x, int y, const char *s,
                      uint32_t col, int scale)
{
    if (!s) return;
    while (*s) {
        draw_char(dst, x, y, *s++, col, scale);
        x += 8 * scale;
    }
}

static void blit_thumb(uint32_t *dst, int x0, int y0, int w, int h,
                       const uint32_t *src)
{
    int x, y;
    for (y = 0; y < h; y++) {
        int sy = y * SAVESTATE_THUMB_H / h;
        for (x = 0; x < w; x++) {
            int sx = x * SAVESTATE_THUMB_W / w;
            dst[(y0 + y) * SSM_W + (x0 + x)] =
                src[sy * SAVESTATE_THUMB_W + sx] | 0xFF000000u;
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
        snprintf(out, cap, "NEW SLOT");
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

#define ACTION_FG(a) ((s_hover_action == (a)) ? 0xFFFFD24Du : 0xFFE2E5EBu)

static void rasterize_panel(void)
{
    int i, first;
    char buf[96];
    char key[32];
    const int top = header_inset();

    for (i = 0; i < SSM_W * SSM_H; i++)
        s_panel[i] = 0xFF0F1118u;

    fill_rect(s_panel, 0, 0, SSM_W, 46 + top, 0xFF171B25u);
    draw_text(s_panel, 24, SSM_TITLE_Y + top, "SAVE STATES", 0xFFFFD24Du, 2);
    host_keymap_label(HOST_KEYMAP_SAVE_STATE_MENU, key, sizeof(key));
    snprintf(buf, sizeof(buf), "%s MENU",
             key[0] ? key : "F7");
    draw_text(s_panel, 432, 18 + top, buf, 0xFFB8BDC8u, 1);

    {
        int cx, cy, cw, ch;
        close_rect(&cx, &cy, &cw, &ch);
        if (s_close_hover)
            fill_rect(s_panel, cx, cy, cw, ch, 0xFF3A2530u);
        stroke_rect(s_panel, cx, cy, cw, ch,
                    s_close_hover ? 0xFFFFD24Du : 0xFF3A4352u);
        /* Glyph is 8x8 at scale 2; centre it in the box rather than hard-coding
         * an offset, so resizing the button keeps the X centred. */
        draw_text(s_panel, cx + (cw - 16) / 2, cy + (ch - 16) / 2, "X",
                  s_close_hover ? 0xFFFFD24Du : 0xFFB8BDC8u, 2);
    }

    refresh_thumbs();
    first = first_visible(s_selected);
    snprintf(buf, sizeof(buf), "%02d-%02d / %02d",
             first + 1, first + SSM_VISIBLE_ROWS, SAVESTATE_SLOTS);
    draw_text(s_panel, 24, 42 + top, buf, 0xFF7F8796u, 1);

    for (i = first; i < first + SSM_VISIBLE_ROWS; i++) {
        int visible = i - first;
        int y = SSM_ROWS_Y + top + visible * (SSM_ROW_H + SSM_ROW_GAP);
        int sel = (i == s_selected);
        int hov = (i == s_hover_row);
        uint32_t bg = sel ? 0xFF2B2830u : (hov ? 0xFF222735u : 0xFF191D27u);
        uint32_t fg = sel ? 0xFFFFD24Du : 0xFFE2E5EBu;
        uint32_t sub = sel ? 0xFFFFFFFFu : 0xFFB2B8C2u;
        fill_rect(s_panel, SSM_ROWS_X, y, SSM_ROWS_W, SSM_ROW_H, bg);
        stroke_rect(s_panel, SSM_ROWS_X, y, SSM_ROWS_W, SSM_ROW_H,
                    sel ? 0xFFFFD24Du : (hov ? 0xFF4A5468u : 0xFF303746u));
        snprintf(buf, sizeof(buf), "SLOT %02d", i + 1);
        draw_text(s_panel, SSM_ROWS_X + 18, y + 18, buf, fg, 1);
        if (s_have_thumb[i]) {
            blit_thumb(s_panel, SSM_ROWS_X + 118, y + 3,
                       SSM_THUMB_W, SSM_THUMB_H, s_thumbs[i]);
        } else {
            fill_rect(s_panel, SSM_ROWS_X + 118, y + 3,
                      SSM_THUMB_W, SSM_THUMB_H, 0xFF242A35u);
            stroke_rect(s_panel, SSM_ROWS_X + 118, y + 3,
                        SSM_THUMB_W, SSM_THUMB_H, 0xFF3A4352u);
            draw_text(s_panel, SSM_ROWS_X + 169, y + 46, "NEW",
                      0xFF707887u, 1);
        }
        format_slot_status(i, buf, sizeof(buf));
        draw_text(s_panel, SSM_ROWS_X + 278, y + 42, buf, sub, 1);
        if (sel)
            draw_text(s_panel, SSM_ROWS_X + SSM_ROWS_W - 88, y + 82,
                      "SELECT", 0xFFFFD24Du, 1);
    }

    fill_rect(s_panel, 0, 410, SSM_W, 70, 0xFF171B25u);
    draw_psx_button(s_panel, 32, 424, 'd');
    draw_text(s_panel, 56, 428, "SLOT", 0xFFE2E5EBu, 1);
    draw_psx_button(s_panel, 132, 424, 'x');
    draw_text(s_panel, 156, 428, "LOAD", ACTION_FG(PSX_SSM_ACTION_LOAD), 1);
    draw_psx_button(s_panel, 230, 424, 's');
    draw_text(s_panel, 254, 428, "SAVE", ACTION_FG(PSX_SSM_ACTION_SAVE), 1);
    draw_psx_button(s_panel, 328, 424, 'o');
    draw_text(s_panel, 352, 428, "BACK", ACTION_FG(PSX_SSM_ACTION_BACK), 1);
    draw_text(s_panel, 32, 454, "KEYS: ARROWS SLOT  ENTER/L LOAD  SHIFT+ENTER/S SAVE  ESC BACK",
              0xFFB8BDC8u, 1);
    s_dirty = 0;
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
    if (w) *w = SSM_W;
    if (h) *h = SSM_H;
    return 1;
}
