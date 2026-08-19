/* Top menu bar overlay: FILE / VIDEO / GAME.
 *
 * Rendered into an ARGB canvas sized in LOGICAL pixels by the renderer, which
 * then magnifies it by an INTEGER factor. Drawing at an integer multiple is
 * what keeps the 8x8 glyphs crisp - stretching one fixed-size canvas across an
 * arbitrary window makes the text uneven. Transparent everywhere the bar and
 * the open dropdown do not cover, so the game stays visible underneath (the
 * presenter blends this layer; see gl_draw_osd_image_ex).
 *
 * The 8x8 glyph set is the same public-domain font8x8_basic 32..90 subset the
 * save-state menu uses. It is duplicated rather than shared so this module has
 * no coupling to the save-state UI; both are small and independently drawable.
 * Consequence: labels are uppercase-only (anything outside 32..90 draws '?'). */

#include "psx_video_menu.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "psx_sdl.h"

/* Canvas ceiling. The renderer raises ui_scale until the logical size fits, so
 * this bounds memory rather than window size. */
#define VM_MAX_W 1280
#define VM_MAX_H 720

#define VM_BAR_H     22
#define VM_TITLE_X0  10
#define VM_TITLE_PAD 14
#define VM_ROW_H     20
#define VM_ROWS_Y0   (VM_BAR_H + 6)

#define COL_BAR      0xF01A1F2Bu
#define COL_BAR_EDGE 0xFF2E3648u
#define COL_PANEL    0xF81A1F2Bu
#define COL_PANEL_ED 0xFF3A4352u
#define COL_TEXT     0xFFD5DAE4u
#define COL_DIM      0xFF7F8796u
#define COL_ACCENT   0xFFFFD24Du
#define COL_SEL_BG   0xFF303746u
#define COL_HOVER_BG 0xFF262D3Bu
#define COL_TITLE_BG 0xFF303746u

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

/* ---- menu model ---------------------------------------------------------- */

/* MODS is appended rather than inserted: s_item[] is indexed by these values,
 * so renumbering the existing menus would silently move every remembered row
 * selection to a different menu. */
enum { MENU_FILE = 0, MENU_VIEW = 1, MENU_VIDEO = 2, MENU_AUDIO = 3,
       MENU_GAME = 4, MENU_CHEATS = 5, MENU_MODS = 6, MENU_COUNT = 7 };
/* AUDIO rows. MASTER scales everything; MUSIC and SOUND are the split buses. */
enum { AUD_MASTER = 0, AUD_MUSIC = 1, AUD_SOUND = 2 };
enum { IT_OPTION = 0, IT_ACTION = 1, IT_NUMBER = 2 };
enum { ACT_CLOSE = 0, ACT_QUIT = 1 };

#define VM_EDIT_MAX 5   /* digits; 32767 is the widest useful value */


static const char *const SCALING_LABELS[] = { "FILL WINDOW", "INTEGER" };
static const char *const FILTER_LABELS[]  = { "NEAREST (SHARP)", "LINEAR (SMOOTH)" };
static const char *const TEXFILTER_LABELS[] = { "NEAREST (SHARP)", "BILINEAR" };
static const char *const SCREEN_LABELS[]  = { "WINDOWED", "BORDERLESS", "EXCLUSIVE" };
/* Index order is the cycle order, not the SDL swap interval: the host maps
 * 0/1/2 -> swap interval 0/1/-1 (see psx_apply_video_menu_state). */
static const char *const VSYNC_LABELS[]   = { "OFF (LOWEST LAG)", "ON (TEAR-FREE)",
                                              "ADAPTIVE" };
/* Indexed by scale-1. Cost is ~N^2 in fill rate, so the labels say so. */
static const char *const SSAA_LABELS[]    = { "NATIVE (1X)", "2X", "3X", "4X" };
static const char *const LOADS_LABELS[]   = { "OFF (AUTHENTIC)", "FAST", "INSTANT" };
/* Write-only cheat: index IS the number of copies given, 0 = do nothing. */
static const char *const ALLCARDS_LABELS[] = { "OFF", "1 OF EACH", "2 OF EACH",
                                               "3 OF EACH" };
static const char *const FUSION_HINT_LABELS[] = { "OFF", "NUMBERS",
                                                  "NUMBERS + INFO" };
static const char *const RANK_LABELS[]    = { "OFF", "IN GAME",
                                              "IN GAME + SCORE",
                                              "OVERLAY TEXT" };

static int s_visible;    /* bar drawn; does not capture input */
static int s_expanded;   /* dropdown open; DOES capture input */
static int s_menu;
static int s_item[MENU_COUNT];
static int s_dirty = 1;
/* Set once redraw() has produced a canvas, so the presentation thread can tell
 * "nothing drawn yet" from "drawn and current" without redrawing itself. */
static int s_canvas_ready;
static int s_changed;
static int s_quit;
static int s_savestate;
static int s_rewind;

static int s_lw = 640, s_lh = 480, s_ui = 1;   /* logical size + magnification */
static int s_hover_menu = -1, s_hover_row = -1;
/* Hover-to-open dwell: which title the pointer is resting on, and since when.
 * s_now_ms is fed by psx_video_menu_tick so this module still owns no clock. */
#define VM_HOVER_OPEN_MS 350u
static int          s_hover_title = -1;
static unsigned int s_hover_title_ms;
static unsigned int s_now_ms;

/* Designated: the old positional list had 7 values for 8 fields, so every
 * member from texture_filter on was initialised with its neighbour's value
 * (screen got 8000). Harmless only because psx_video_menu_init overwrites the
 * whole struct from the caller's seed — but it is a trap for the next field
 * added, which is exactly what vsync would have been. */
static PsxVideoMenuState s_state = {
    .scaling        = PSX_VM_SCALING_FILL,
    .filter         = PSX_VM_FILTER_LINEAR,
    .texture_filter = PSX_VM_FILTER_NEAREST,
    .screen         = PSX_VM_SCREEN_WINDOWED,
    .vsync          = PSX_VM_VSYNC_ON,
    .supersampling  = 1,
    .fast_loads     = PSX_VM_LOADS_OFF,
    .life_points    = PSX_VM_LIFE_POINTS_DEFAULT,
    .speed          = PSX_VM_SPEED_DEFAULT,
    .starchips      = 0,   /* write-only, never restored */
    .free_spending  = 0,
    .vol_master     = 100,
    .vol_music      = 100,
    .vol_sound      = 100,
    .rank_meter     = PSX_VM_RANK_OFF,
    .fusion_hint    = PSX_VM_FUSION_HINT_OFF,
    .fusion_by_def  = 0,
    .card_drops     = PSX_VM_CARD_DROPS_DEFAULT,
};

/* Inline numeric entry. Active only while the player is typing into a row. */
static int  s_editing;
static char s_edit_buf[VM_EDIT_MAX + 1];
static int  s_edit_len;

static uint32_t s_canvas[VM_MAX_W * VM_MAX_H];

/* ---- numeric options -----------------------------------------------------
 * Everything needed to make a row typeable lives in these three functions, so
 * adding another numeric option is one case each rather than new UI code.
 * Ranges are inclusive and enforced on COMMIT, not while typing, so a prefix
 * like "3" on the way to "3000" is not fought by the clamp. */

static int num_range(int m, int row, int *lo, int *hi) {
    if (m == MENU_CHEATS && row == 0) {
        /* The patched instruction is `addiu`, whose sign-extended immediate
         * would allow up to 32767 — but the game's LP display is four digits,
         * so the useful range stops at 9999 and that is what the slider spans. */
        *lo = 1; *hi = 9999; return 1;
    }
    if (m == MENU_CHEATS && row == 1) {
        /* 32-bit field, but the entry box holds 5 digits. */
        *lo = 0; *hi = 99999; return 1;
    }
    if (m == MENU_GAME && row == 0) {
        *lo = 1; *hi = PSX_VM_SPEED_MAX; return 1;
    }
    if (m == MENU_MODS && row == 0) {
        *lo = 1; *hi = PSX_VM_CARD_DROPS_MAX; return 1;
    }
    if (m == MENU_AUDIO) { *lo = 0; *hi = 100; return 1; }   /* percent */
    return 0;
}

static int num_get(int m, int row) {
    if (m == MENU_CHEATS && row == 0) return s_state.life_points;
    if (m == MENU_CHEATS && row == 1) return s_state.starchips;
    if (m == MENU_GAME && row == 0) return s_state.speed;
    if (m == MENU_MODS && row == 0) return s_state.card_drops;
    if (m == MENU_AUDIO) {
        if (row == AUD_MASTER) return s_state.vol_master;
        if (row == AUD_MUSIC)  return s_state.vol_music;
        return s_state.vol_sound;
    }
    return 0;
}

static void num_set(int m, int row, int v) {
    if (m == MENU_CHEATS && row == 0) s_state.life_points = v;
    if (m == MENU_CHEATS && row == 1) s_state.starchips = v;
    if (m == MENU_GAME && row == 0) s_state.speed = v;
    if (m == MENU_MODS && row == 0) s_state.card_drops = v;
    if (m == MENU_AUDIO) {
        if (row == AUD_MASTER)     s_state.vol_master = v;
        else if (row == AUD_MUSIC) s_state.vol_music  = v;
        else                       s_state.vol_sound  = v;
    }
}

static const char *menu_title(int m) {
    if (m == MENU_FILE) return "FILE";
    if (m == MENU_VIEW) return "VIEW";
    if (m == MENU_VIDEO) return "VIDEO";
    if (m == MENU_AUDIO) return "AUDIO";
    if (m == MENU_GAME) return "GAME";
    if (m == MENU_CHEATS) return "CHEATS";
    return "MODS";
}

static int menu_rows(int m) {
    if (m == MENU_FILE) return 2;
    if (m == MENU_VIEW) return 4;
    if (m == MENU_VIDEO) return 6;
    if (m == MENU_AUDIO) return 3;
    if (m == MENU_CHEATS) return 4;
    if (m == MENU_MODS) return 1;
    if (m == MENU_GAME) return 4;
    return 1;
}

static int row_kind(int m, int row) {
    int lo, hi;
    if (m == MENU_FILE) return IT_ACTION;
    /* VIEW row 0 is the only ACTION here (it hides the bar); row 1 is an
     * ordinary cycling option. Returning IT_ACTION for the whole menu, as this
     * did while VIEW had a single row, would make picking the new row hide the
     * bar instead of changing it. */
    if (m == MENU_VIEW) return (row == 0) ? IT_ACTION : IT_OPTION;
    /* GAME's only ACTION row: it hands off to the save-state overlay rather
     * than changing a setting. It has to be named explicitly — the rows above
     * it are a number and an option, so unlike FILE this menu has no
     * whole-menu answer, and num_range below reports 0 here, which would
     * otherwise make it an IT_OPTION that cycles nothing. */
    if (m == MENU_GAME && (row == 2 || row == 3)) return IT_ACTION;
    if (num_range(m, row, &lo, &hi)) return IT_NUMBER;
    return IT_OPTION;
}

/* ---- inline numeric entry ------------------------------------------------ */

static const char *lp_text(int v);   /* defined below; used by edit_nudge */

static void edit_begin(void) {
    s_edit_len = 0;
    s_edit_buf[0] = '\0';
    s_editing = 1;
    s_dirty = 1;
}

static void edit_cancel(void) {
    if (!s_editing) return;
    s_editing = 0;
    s_edit_len = 0;
    s_edit_buf[0] = '\0';
    s_dirty = 1;
}

/* Commit the typed digits. An empty entry means "leave it alone" rather than
 * silently writing zero. */
static void edit_commit(void) {
    int lo = 0, hi = 0, v = 0, i;
    if (!s_editing) return;
    if (s_edit_len > 0 && num_range(s_menu, s_item[s_menu], &lo, &hi)) {
        for (i = 0; i < s_edit_len; i++)
            v = v * 10 + (s_edit_buf[i] - '0');
        if (v < lo) v = lo;
        if (v > hi) v = hi;
        if (v != num_get(s_menu, s_item[s_menu])) {
            num_set(s_menu, s_item[s_menu], v);
            s_changed = 1;
        }
    }
    edit_cancel();
}

static void edit_digit(int d) {
    if (!s_editing || s_edit_len >= VM_EDIT_MAX) return;
    if (s_edit_len == 0 && d == 0) {
        /* "0" is a legal VALUE for the audio buses; the blanket leading-zero
         * ban dates from when every numeric row had a minimum of 1. */
        int lo = 0, hi = 0;
        if (!num_range(s_menu, s_item[s_menu], &lo, &hi) || lo > 0) return;
    }
    s_edit_buf[s_edit_len++] = (char)('0' + d);
    s_edit_buf[s_edit_len] = '\0';
    s_dirty = 1;
}

static void edit_backspace(void) {
    if (!s_editing || s_edit_len <= 0) return;
    s_edit_buf[--s_edit_len] = '\0';
    s_dirty = 1;
}

/* Adjust the value being edited without typing. This is what makes numeric
 * options reachable from a controller, which has no digits: the field opens
 * with A, then the d-pad walks the number. Also handy on a keyboard. */
static void edit_nudge(int delta) {
    int lo = 0, hi = 0, v = 0, i;
    const char *t;
    if (!s_editing) return;
    if (!num_range(s_menu, s_item[s_menu], &lo, &hi)) return;
    if (s_edit_len > 0) {
        for (i = 0; i < s_edit_len; i++) v = v * 10 + (s_edit_buf[i] - '0');
    } else {
        v = num_get(s_menu, s_item[s_menu]);
    }
    v += delta;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    t = lp_text(v);
    s_edit_len = 0;
    while (*t && s_edit_len < VM_EDIT_MAX) s_edit_buf[s_edit_len++] = *t++;
    s_edit_buf[s_edit_len] = '\0';
    s_dirty = 1;
}

static const char *row_label(int m, int row) {
    if (m == MENU_FILE)
        return (row == 0) ? "CLOSE MENU" : "QUIT";
    if (m == MENU_VIEW)
        return (row == 0) ? "MENU BAR"
             : (row == 1) ? "DUEL RANK"
             : (row == 2) ? "FUSION HINT"
                          : "SUGGEST BY";
    if (m == MENU_CHEATS)
        return (row == 0) ? "LIFE POINTS"
             : (row == 1) ? "STARCHIPS"
             : (row == 2) ? "FREE SPENDING"
                          : "ALL CARDS";
    if (m == MENU_MODS)
        return "CARD DROPS";
    if (m == MENU_GAME)
        return (row == 0) ? "SPEED"
             : (row == 1) ? "FAST LOADING"
             : (row == 2) ? "SAVE / LOAD STATE"
                          : "REWIND";
    if (m == MENU_AUDIO)
        return (row == AUD_MASTER) ? "MASTER"
             : (row == AUD_MUSIC)  ? "MUSIC"
                                   : "SOUND EFFECTS";
    switch (row) {
        case 0:  return "SCALING";
        case 1:  return "PRESENT FILTER";
        case 2:  return "TEXTURE FILTER";
        case 3:  return "SCREEN";
        case 4:  return "VSYNC";
        default: return "RESOLUTION";
    }
}

/* Small itoa so this module keeps no stdio dependency. */
static const char *lp_text(int v) {
    /* Rotating pool, not a single static buffer: callers legitimately hold
     * more than one value at a time now that panel_rect measures every row's
     * value to size itself. With one buffer, the measure pass overwrote the
     * string the draw loop was about to print and all rows showed the last
     * row's number. */
    static char pool[4][12];
    static int  slot;
    char *buf = pool[slot];
    slot = (slot + 1) & 3;
    int i = 0, j, n = v;
    char tmp[12];
    if (n <= 0) { buf[0] = '0'; buf[1] = '\0'; return buf; }
    while (n > 0 && i < (int)sizeof(tmp) - 1) { tmp[i++] = (char)('0' + n % 10); n /= 10; }
    for (j = 0; j < i; j++) buf[j] = tmp[i - 1 - j];
    buf[i] = '\0';
    return buf;
}

static const char *row_value(int m, int row) {
    int lo, hi;
    if (num_range(m, row, &lo, &hi)) {
        /* While typing, show the buffer plus a caret so the row reads as an
         * active text field rather than a stale value. */
        if (s_editing && m == s_menu && row == s_item[s_menu]) {
            static char buf[VM_EDIT_MAX + 2];
            int i;
            for (i = 0; i < s_edit_len; i++) buf[i] = s_edit_buf[i];
            buf[s_edit_len] = '_';
            buf[s_edit_len + 1] = '\0';
            return buf;
        }
        return lp_text(num_get(m, row));
    }
    if (m == MENU_VIEW) {
        if (row == 0) return "VISIBLE   F10";
        if (row == 1)
            return RANK_LABELS[(s_state.rank_meter >= 0 &&
                                s_state.rank_meter <= 3) ? s_state.rank_meter : 0];
        if (row == 2)
            return FUSION_HINT_LABELS[(s_state.fusion_hint >= 0 &&
                                       s_state.fusion_hint <= 2)
                                          ? s_state.fusion_hint : 0];
        return s_state.fusion_by_def ? "DEFENSE" : "ATTACK";
    }
    if (m == MENU_CHEATS && row == 2)
        return s_state.free_spending ? "ON" : "OFF";
    if (m == MENU_CHEATS && row == 3)
        return ALLCARDS_LABELS[(s_state.all_cards >= 0 && s_state.all_cards <= 3)
                                   ? s_state.all_cards : 0];
    if (m == MENU_GAME && row == 1)
        return LOADS_LABELS[(s_state.fast_loads >= 0 && s_state.fast_loads <= 2)
                                ? s_state.fast_loads : 0];
    if (m != MENU_VIDEO) return NULL;
    switch (row) {
        case 0:  return SCALING_LABELS[s_state.scaling ? 1 : 0];
        case 1:  return FILTER_LABELS[s_state.filter ? 1 : 0];
        case 2:  return TEXFILTER_LABELS[s_state.texture_filter ? 1 : 0];
        case 3:  return SCREEN_LABELS[(s_state.screen >= 0 && s_state.screen <= 2)
                                          ? s_state.screen : 0];
        case 4:  return VSYNC_LABELS[(s_state.vsync >= 0 && s_state.vsync <= 2)
                                          ? s_state.vsync : 1];
        default: return SSAA_LABELS[(s_state.supersampling >= 1 &&
                                     s_state.supersampling <= PSX_VM_SUPERSAMPLING_MAX)
                                        ? s_state.supersampling - 1 : 0];
    }
}

static const char *row_hint(int m, int row) {
    int lo, hi;
    if (s_editing && m == s_menu && row == s_item[s_menu])
        return "TYPE DIGITS  ENTER APPLY  ESC CANCEL";
    if (m == MENU_FILE)
        return (row == 0) ? "CLOSE THIS MENU" : "EXIT THE GAME";
    if (m == MENU_VIEW && row == 2)
        return (s_state.fusion_hint == PSX_VM_FUSION_HINT_OFF)
                   ? "SHOW WHAT YOUR HAND CAN FUSE INTO"
             : (s_state.fusion_hint == PSX_VM_FUSION_HINT_NUMBERS)
                   ? "PICK ORDER ON THE CARDS ONLY"
                   : "PICK ORDER PLUS THE CARD IT MAKES";
    if (m == MENU_VIEW && row == 3)
        return s_state.fusion_by_def
                   ? "SUGGEST THE BEST DEFENSE"
                   : "SUGGEST THE BEST ATTACK";
    if (m == MENU_VIEW)
        return (row == 0)
                   ? "PRESS F10 ANY TIME TO SHOW OR HIDE"
             : (s_state.rank_meter == PSX_VM_RANK_OFF)
                   ? "SHOW LIVE RANK IN DUEL"
             : (s_state.rank_meter == PSX_VM_RANK_INGAME)
                   ? "GAME SPRITES BESIDE THE FIELD BOX"
             : (s_state.rank_meter == PSX_VM_RANK_INGAME_SCORE)
                   ? "SPRITES PLUS THE 0-99 SCORE"
                   : "TEXT IN THE CORNER - NEVER COVERED";
    if (m == MENU_CHEATS && row == 2)
        return s_state.free_spending
                   ? "PURCHASES REFUNDED - EARNINGS STILL COUNT"
                   : "SPENDING DEDUCTS NORMALLY";
    if (m == MENU_CHEATS && row == 3)
        return s_state.all_cards == 0
                   ? "FILLS THE TRUNK - CLOSE THE CHEST FIRST"
                   : "APPLIES NOW - PERMANENT ONCE YOU SAVE";
    if (m == MENU_GAME && row == 1)
        return s_state.fast_loads == PSX_VM_LOADS_OFF
                   ? "REAL DRIVE TIMING"
             : s_state.fast_loads == PSX_VM_LOADS_FAST
                   ? "SHORTER LOADS - GAME SPEED UNCHANGED"
                   : "FASTEST - BACK OFF IF A LOAD STALLS";
    /* Says the quiet part out loud: the overlay this opens freezes the guest,
     * which is expected from a hotkey but surprising from a menu row. */
    if (m == MENU_GAME && row == 2)
        return "PAUSES THE GAME UNTIL YOU PICK A SLOT";
    /* Names the hotkey as well as the effect: the row exists because the
     * feature was previously reachable only by a key nothing advertised. */
    if (m == MENU_GAME && row == 3)
        return "STEP BACK THROUGH RECENT FRAMES - ALSO F8";
    if (m == MENU_AUDIO)
        return (row == AUD_MASTER)
                   ? "SCALES EVERYTHING - ENTER TO TYPE"
             : (row == AUD_MUSIC)
                   ? "BGM AND CD AUDIO - ENTER TO TYPE"
                   : "SOUND EFFECTS ONLY - ENTER TO TYPE";
    if (num_range(m, row, &lo, &hi)) {
        if (m == MENU_CHEATS && row == 0)
            return (s_state.life_points == PSX_VM_LIFE_POINTS_DEFAULT)
                       ? "8000 IS STOCK. ENTER TO TYPE"
                       : "MODDED. APPLIES ON NEXT DUEL";
        if (m == MENU_CHEATS && row == 1)
            return "WRITES TO THE LIVE SAVE IMMEDIATELY";
        if (m == MENU_GAME && row == 0)
            return (s_state.speed <= 1)
                       ? "1X IS NORMAL SPEED. 1 TO 16"
                       : "FAST FORWARD - AUDIO MAY DISTORT";
        if (m == MENU_MODS && row == 0)
            return (s_state.card_drops <= 1)
                       ? "1 IS STOCK. CARDS PER DUEL WON"
                       : "SAME DROP POOL - ROLLED ONCE PER CARD";
        return "ENTER TO TYPE A VALUE";
    }
    switch (row) {
        case 0:
            return s_state.scaling
                ? "WHOLE PIXELS - SHARP, MAY BORDER"
                : "FILLS WINDOW - UNEVEN PIXELS";
        case 1:
            return s_state.filter
                ? "SMOOTHS THE WHOLE IMAGE"
                : "NO SMOOTHING - CRISP PIXELS";
        case 2:
            return s_state.texture_filter
                ? "SMOOTHS IN-GAME TEXTURES ONLY"
                : "RAW TEXELS - ORIGINAL LOOK";
        case 3:
            return "ALT+ENTER ALSO TOGGLES";
        case 4:
            return s_state.vsync == PSX_VM_VSYNC_OFF
                ? "LOWEST INPUT LAG - MAY TEAR"
                : (s_state.vsync == PSX_VM_VSYNC_ADAPTIVE
                       ? "TEAR-FREE, TEARS IF A FRAME IS LATE"
                       : "TEAR-FREE - ADDS UP TO ONE REFRESH");
        default:
            return s_state.supersampling <= 1
                ? "SHARPER 3D. TAKES EFFECT ON RESTART"
                : "SHARPER 3D - COSTS FILL RATE. ON RESTART";
    }
}

static void cycle_row(int m, int row, int delta) {
    if (m == MENU_VIEW && row == 2) {
        int v = s_state.fusion_hint + delta;
        while (v < 0) v += 3;
        while (v > 2) v -= 3;
        s_state.fusion_hint = v;
        s_dirty = 1;
        return;
    }
    if (m == MENU_VIEW && row == 3) {
        /* Attack and defence are one row, not two toggles: choosing one has
         * to unchoose the other, and a single cycling row makes that
         * structural instead of something two handlers have to agree on. */
        s_state.fusion_by_def = !s_state.fusion_by_def;
        s_dirty = 1;
        return;
    }
    if (m == MENU_VIEW && row == 1) {
        int v = s_state.rank_meter + delta;
        while (v < 0) v += 4;
        while (v > 3) v -= 4;
        s_state.rank_meter = v;
        s_changed = 1;
        s_dirty = 1;
        return;
    }
    if (m == MENU_CHEATS && row == 2) {
        s_state.free_spending = s_state.free_spending ? 0 : 1;
        s_changed = 1;
        s_dirty = 1;
        return;
    }
    if (m == MENU_CHEATS && row == 3) {
        int v = s_state.all_cards + delta;
        while (v < 0) v += 4;
        while (v > 3) v -= 4;
        s_state.all_cards = v;
        s_changed = 1;
        s_dirty = 1;
        return;
    }
    if (m == MENU_GAME && row == 1) {
        int v = s_state.fast_loads + delta;
        while (v < 0) v += 3;
        while (v > 2) v -= 3;
        s_state.fast_loads = v;
        s_changed = 1;
        s_dirty = 1;
        return;
    }
    if (m == MENU_VIDEO) {
        switch (row) {
            case 0: s_state.scaling = s_state.scaling ? 0 : 1; break;
            case 1: s_state.filter  = s_state.filter  ? 0 : 1; break;
            case 2: s_state.texture_filter = s_state.texture_filter ? 0 : 1; break;
            case 3: {
                int v = s_state.screen + delta;
                while (v < 0) v += 3;
                while (v > 2) v -= 3;
                s_state.screen = v;
                break;
            }
            case 4: {
                int v = s_state.vsync + delta;
                while (v < 0) v += 3;
                while (v > 2) v -= 3;
                s_state.vsync = v;
                break;
            }
            default: {
                int v = s_state.supersampling + delta;
                while (v < 1) v += PSX_VM_SUPERSAMPLING_MAX;
                while (v > PSX_VM_SUPERSAMPLING_MAX) v -= PSX_VM_SUPERSAMPLING_MAX;
                s_state.supersampling = v;
                break;
            }
        }
    } else {
        return;
    }
    s_changed = 1;
    s_dirty = 1;
}

/* ---- geometry (shared by drawing and hit-testing) ------------------------ */

static int text_w(const char *s, int scale) {
    int n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n * 8 * scale;
}

static int title_x(int m) {
    int x = VM_TITLE_X0, i;
    for (i = 0; i < m; i++)
        x += text_w(menu_title(i), 1) + VM_TITLE_PAD * 2;
    return x;
}

static void panel_rect(int m, int *px, int *pw, int *ph) {
    int rows = menu_rows(m);
    int x = title_x(m) - VM_TITLE_PAD;
    int w = 0, i, has_number = 0, lo, hi;
    /* MEASURE the content instead of guessing a per-menu constant. The old
     * fixed widths (420/340/300/230) did not actually fit their menus: a value
     * or a hint longer than the guess simply ran past the panel edge and was
     * clipped by the canvas. Anything added to a menu later would silently
     * reintroduce that, which is how VSYNC and the AUDIO rows hit it. */
    for (i = 0; i < rows; i++) {
        const char *v = row_value(m, i);
        const char *h = row_hint(m, i);
        int need = 12 + text_w(row_label(m, i), 1);
        if (v) need += 24 + text_w(v, 1);
        need += 12;
        if (need > w) w = need;
        if (h) { int hw = 12 + text_w(h, 1) + 12; if (hw > w) w = hw; }
        if (num_range(m, i, &lo, &hi)) has_number = 1;
    }
    /* Typing swaps in a long "TYPE DIGITS / ENTER APPLY / ESC CANCEL" hint.
     * Reserve it up front so the panel does not resize mid-edit. */
    if (has_number) {
        int ew = 12 + text_w("TYPE DIGITS  ENTER APPLY  ESC CANCEL", 1) + 12;
        if (ew > w) w = ew;
    }
    if (w < 200) w = 200;
    if (w > s_lw) w = s_lw;          /* never wider than the canvas */
    if (x + w > s_lw) x = s_lw - w;
    if (x < 0) x = 0;
    *px = x;
    *pw = w;
    *ph = rows * VM_ROW_H + 28;
}

/* Slider rows: a draggable track instead of type-only entry.
 *
 * Volume is the one option people adjust by feel rather than by number, so the
 * AUDIO rows carry a track you can click or drag anywhere along. Typing still
 * works (ENTER opens the field) — this is an additional way in, not a
 * replacement. Only rows that report a range AND opt in get one, so the
 * cheat/speed fields keep their exact-entry behaviour. */
#define VM_SLIDER_W 120
#define VM_SLIDER_H 8

static int row_is_slider(int m, int row) {
    int lo, hi;
    if (!num_range(m, row, &lo, &hi)) return 0;
    if (m == MENU_AUDIO) return 1;
    if (m == MENU_GAME) return 1;                 /* SPEED 1..16 */
    if (m == MENU_MODS) return 1;                 /* CARD DROPS 1..99 */
    if (m == MENU_CHEATS && row == 0) return 1;   /* LIFE POINTS 1..9999 */
    return 0;                                     /* STARCHIPS stays type-only */
}

/* A value worth marking on the track, or -1. LIFE POINTS gets a notch at the
 * stock 8000 so the original value is findable by eye after dragging. */
static int slider_mark(int m, int row) {
    if (m == MENU_CHEATS && row == 0) return PSX_VM_LIFE_POINTS_DEFAULT;
    return -1;
}

/* Track rect for a slider row, in logical canvas pixels. */
static void slider_rect(int m, int row, int *sx, int *sy, int *sw, int *sh) {
    int px, pw, ph;
    panel_rect(m, &px, &pw, &ph);
    *sw = VM_SLIDER_W;
    *sh = VM_SLIDER_H;
    /* Right-aligned, leaving room for the numeric readout beyond it. */
    *sx = px + pw - VM_SLIDER_W - 52;
    *sy = VM_ROWS_Y0 + row * VM_ROW_H + (VM_ROW_H - VM_SLIDER_H) / 2 - 3;
}

/* Which row's slider is being dragged, or -1. Held across motion events so a
 * drag keeps control even when the pointer strays off the track. */
static int s_drag_row = -1;

static void slider_set_from_x(int m, int row, int lx) {
    int sx, sy, sw, sh, lo = 0, hi = 0, v;
    if (!num_range(m, row, &lo, &hi)) return;
    slider_rect(m, row, &sx, &sy, &sw, &sh);
    if (sw <= 1) return;
    v = lo + ((lx - sx) * (hi - lo) + (sw - 1) / 2) / (sw - 1);
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    if (v != num_get(m, row)) {
        num_set(m, row, v);
        s_changed = 1;
        s_dirty = 1;
    }
}

/* ---- drawing ------------------------------------------------------------- */

static void fill_rect(int x0, int y0, int w, int h, uint32_t col) {
    int x, y;
    if (x0 < 0) { w += x0; x0 = 0; }
    if (y0 < 0) { h += y0; y0 = 0; }
    if (x0 + w > s_lw) w = s_lw - x0;
    if (y0 + h > s_lh) h = s_lh - y0;
    if (w <= 0 || h <= 0) return;
    for (y = y0; y < y0 + h; y++)
        for (x = x0; x < x0 + w; x++)
            s_canvas[y * s_lw + x] = col;
}

static void stroke_rect(int x, int y, int w, int h, uint32_t col) {
    fill_rect(x, y, w, 1, col);
    fill_rect(x, y + h - 1, w, 1, col);
    fill_rect(x, y, 1, h, col);
    fill_rect(x + w - 1, y, 1, h, col);
}

static void draw_char(int x0, int y0, char c, uint32_t col, int scale) {
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
                    if ((unsigned)dx < (unsigned)s_lw &&
                        (unsigned)dy < (unsigned)s_lh)
                        s_canvas[dy * s_lw + dx] = col;
                }
        }
    }
}

static void draw_text(int x, int y, const char *s, uint32_t col, int scale) {
    if (!s) return;
    while (*s) {
        draw_char(x, y, *s++, col, scale);
        x += 8 * scale;
    }
}

static void redraw(void) {
    int i, rows, y, px, pw, ph;

    memset(s_canvas, 0, (size_t)s_lw * (size_t)s_lh * sizeof(uint32_t));
    if (!s_visible) { s_dirty = 0; return; }

    fill_rect(0, 0, s_lw, VM_BAR_H, COL_BAR);
    fill_rect(0, VM_BAR_H - 1, s_lw, 1, COL_BAR_EDGE);

    for (i = 0; i < MENU_COUNT; i++) {
        int tx = title_x(i);
        int tw = text_w(menu_title(i), 1);
        /* A title is only "active" while its dropdown is actually open. With
         * the bar collapsed nothing is selected, so the last-used menu must not
         * stay highlighted — it would read as an open menu that isn't. */
        int active = (s_expanded && i == s_menu);
        if (active)
            fill_rect(tx - VM_TITLE_PAD, 0, tw + VM_TITLE_PAD * 2,
                      VM_BAR_H - 1, COL_TITLE_BG);
        else if (i == s_hover_menu)
            fill_rect(tx - VM_TITLE_PAD, 0, tw + VM_TITLE_PAD * 2,
                      VM_BAR_H - 1, COL_HOVER_BG);
        draw_text(tx, 7, menu_title(i), active ? COL_ACCENT : COL_TEXT, 1);
    }
    /* Collapsed: the bar alone, sitting over the game without taking input.
     * Only F10 / VIEW > MENU BAR removes it. */
    if (!s_expanded) { s_dirty = 0; return; }


    rows = menu_rows(s_menu);
    panel_rect(s_menu, &px, &pw, &ph);
    fill_rect(px, VM_BAR_H, pw, ph, COL_PANEL);
    stroke_rect(px, VM_BAR_H, pw, ph, COL_PANEL_ED);

    y = VM_ROWS_Y0;
    for (i = 0; i < rows; i++) {
        int sel = (i == s_item[s_menu]);
        int hov = (s_hover_row == i && s_hover_menu == s_menu);
        if (sel || hov)
            fill_rect(px + 3, y - 3, pw - 6, VM_ROW_H,
                      sel ? COL_SEL_BG : COL_HOVER_BG);
        draw_text(px + 12, y + 2, row_label(s_menu, i),
                  sel ? COL_ACCENT : COL_TEXT, 1);
        {
            /* Draw whatever the row reports as its value — this must cover
             * IT_NUMBER too, not just IT_OPTION, or a typed field renders
             * blank and the player edits it blind. */
            /* COPY it: slider_rect -> panel_rect measures every row, which
             * calls row_value again. Holding the returned pointer across that
             * is what made every row print the last row's number. */
            char vbuf[24];
            const char *vsrc = row_value(s_menu, i);
            const char *v = NULL;
            int editing_here = (s_editing && i == s_item[s_menu]);
            if (vsrc) {
                int c = 0;
                while (vsrc[c] && c < (int)sizeof(vbuf) - 1) { vbuf[c] = vsrc[c]; c++; }
                vbuf[c] = ' ';
                v = vbuf;
            }
            if (row_is_slider(s_menu, i) && !editing_here) {
                int sx, sy, sw, sh, lo = 0, hi = 0, fill;
                slider_rect(s_menu, i, &sx, &sy, &sw, &sh);
                num_range(s_menu, i, &lo, &hi);
                fill = (hi > lo)
                         ? ((num_get(s_menu, i) - lo) * sw) / (hi - lo) : 0;
                if (fill < 0) fill = 0;
                if (fill > sw) fill = sw;
                fill_rect(sx, sy, sw, sh, 0xFF0E1119u);
                if (fill > 0)
                    fill_rect(sx, sy, fill, sh, sel ? COL_ACCENT : COL_TEXT);
                stroke_rect(sx, sy, sw, sh, COL_PANEL_ED);
                {
                    const int mark = slider_mark(s_menu, i);
                    if (mark >= lo && mark <= hi && hi > lo) {
                        int mx = sx + ((mark - lo) * (sw - 1)) / (hi - lo);
                        /* Taller than the track so it reads as a notch on the
                         * scale rather than part of the fill. */
                        fill_rect(mx, sy - 3, 1, sh + 6, COL_ACCENT);
                    }
                }
                if (v)
                    draw_text(sx + sw + 10, y + 2, v,
                              sel ? COL_ACCENT : COL_DIM, 1);
            } else if (v && editing_here) {
                /* Active field: a boxed, left-aligned entry so the caret and
                 * the digits are unmistakable against the row highlight. */
                int bw = VM_EDIT_MAX * 8 + 14;
                int bx = px + pw - bw - 10;
                fill_rect(bx, y - 2, bw, VM_ROW_H - 2, 0xFF0E1119u);
                stroke_rect(bx, y - 2, bw, VM_ROW_H - 2, COL_ACCENT);
                draw_text(bx + 6, y + 2, v, COL_ACCENT, 1);
            } else if (v) {
                draw_text(px + pw - text_w(v, 1) - 12, y + 2, v,
                          sel ? COL_ACCENT : COL_DIM, 1);
            }
        }
        y += VM_ROW_H;
    }
    /* Hint follows the POINTER when it is over a row, and the keyboard
     * selection otherwise — so you can read what an option does just by
     * hovering it, without committing to selecting it. */
    {
        const int hint_row = (s_hover_row >= 0 && s_hover_menu == s_menu)
                                 ? s_hover_row : s_item[s_menu];
        draw_text(px + 12, y + 4, row_hint(s_menu, hint_row), COL_DIM, 1);
    }

    s_dirty = 0;
}

/* ---- public API ---------------------------------------------------------- */

void psx_video_menu_init(const PsxVideoMenuState *initial) {
    if (initial) s_state = *initial;
    /* Bar VISIBLE on launch so it is discoverable — a hotkey nobody knows about
     * may as well not exist — but COLLAPSED, so a dropdown is not sitting over
     * the game while it boots. Clicking a title (or F10 after hiding) expands. */
    s_visible = 1;
    s_expanded = 0;
    s_menu = MENU_VIDEO;
    for (int i = 0; i < MENU_COUNT; i++) s_item[i] = 0;
    s_changed = 0;
    s_quit = 0;
    s_savestate = 0;
    s_rewind = 0;
    s_hover_menu = s_hover_row = -1;
    s_dirty = 1;
}

void psx_video_menu_sync_screen(int screen) {
    if (screen < 0 || screen > 2) return;
    if (s_state.screen == screen) return;
    s_state.screen = screen;
    s_dirty = 1;   /* deliberately no s_changed: this reflects, not requests */
}

void psx_video_menu_sync_fast_loads(int level) {
    if (level < 0 || level > 2) return;
    if (s_state.fast_loads == level) return;
    s_state.fast_loads = level;
    s_dirty = 1;   /* reflects a change made elsewhere; raises no change event */
}

void psx_video_menu_set_layout(int logical_w, int logical_h, int ui_scale) {
    if (logical_w < 160) logical_w = 160;
    if (logical_h < 120) logical_h = 120;
    if (logical_w > VM_MAX_W) logical_w = VM_MAX_W;
    if (logical_h > VM_MAX_H) logical_h = VM_MAX_H;
    if (ui_scale < 1) ui_scale = 1;
    if (s_lw == logical_w && s_lh == logical_h && s_ui == ui_scale) return;
    s_lw = logical_w;
    s_lh = logical_h;
    s_ui = ui_scale;
    s_dirty = 1;
}

int psx_video_menu_is_open(void)    { return s_expanded; }
int psx_video_menu_is_visible(void) { return s_visible; }

void psx_video_menu_debug_snapshot(PsxVideoMenuDebug *out) {
    if (!out) return;
    out->visible    = s_visible;
    out->expanded   = s_expanded;
    out->menu       = s_menu;
    out->item       = (s_menu >= 0 && s_menu < MENU_COUNT) ? s_item[s_menu] : -1;
    out->hover_menu = s_hover_menu;
    out->hover_row  = s_hover_row;
    out->editing    = s_editing;
    out->dirty      = s_dirty;
    out->logical_w  = s_lw;
    out->logical_h  = s_lh;
    out->ui_scale   = s_ui;
    out->rows       = menu_rows(s_menu);
    out->vol_master = s_state.vol_master;
    out->vol_music  = s_state.vol_music;
    out->vol_sound  = s_state.vol_sound;
}

/* F10 (and the controller Guide button): show the menu EXPANDED, or hide it
 * entirely. It must come back expanded, because expanding is what captures the
 * keyboard and pad — psx_video_menu_handle_key returns 0 while collapsed, so a
 * bar that reappeared collapsed left keyboard- and controller-only players with
 * no way to reach a dropdown at all (s_expanded is otherwise set only by
 * psx_video_menu_mouse_click).
 *
 * The non-capturing visible+collapsed bar is not lost: it is the launch state,
 * and it is what clicking into the game or picking an item drops back to. */
void psx_video_menu_toggle(void) {
    edit_cancel();
    s_visible = !s_visible;
    s_expanded = s_visible;
    s_hover_menu = s_hover_row = -1;
    s_dirty = 1;
}

void psx_video_menu_hide(void) {
    s_drag_row = -1;
    edit_cancel();
    s_visible = 0;
    s_expanded = 0;
    s_hover_menu = s_hover_row = -1;
    s_dirty = 1;
}

/* Close the dropdown but keep the bar on screen — what clicking into the game
 * does. The bar only disappears via psx_video_menu_hide. */
void psx_video_menu_collapse(void) {
    s_drag_row = -1;
    edit_cancel();
    if (!s_expanded) return;
    s_expanded = 0;
    s_hover_menu = s_hover_row = -1;
    s_dirty = 1;
}

void psx_video_menu_close(void) { psx_video_menu_collapse(); }

/* Window pixels -> logical canvas pixels. */
static void to_logical(int wx, int wy, int *lx, int *ly) {
    int s = (s_ui > 0) ? s_ui : 1;
    *lx = wx / s;
    *ly = wy / s;
}

/* Which top-level title is at logical (x,y)? -1 for none. */
static int hit_title(int x, int y) {
    int i;
    if (y < 0 || y >= VM_BAR_H) return -1;
    for (i = 0; i < MENU_COUNT; i++) {
        int tx = title_x(i) - VM_TITLE_PAD;
        int tw = text_w(menu_title(i), 1) + VM_TITLE_PAD * 2;
        if (x >= tx && x < tx + tw) return i;
    }
    return -1;
}

/* Which row of the OPEN menu is at logical (x,y)? -1 for none. */
static int hit_row(int x, int y) {
    int px, pw, ph, rows, i;
    panel_rect(s_menu, &px, &pw, &ph);
    if (x < px || x >= px + pw) return -1;
    rows = menu_rows(s_menu);
    for (i = 0; i < rows; i++) {
        int ry = VM_ROWS_Y0 + i * VM_ROW_H - 3;
        if (y >= ry && y < ry + VM_ROW_H) return i;
    }
    return -1;
}

void psx_video_menu_mouse_move(int win_x, int win_y) {
    int lx, ly, t, r;
    if (!s_visible) return;
    to_logical(win_x, win_y, &lx, &ly);
    /* A drag in progress owns the pointer: keep tracking X even once it
     * wanders off the track, which is what makes dragging feel right. */
    if (s_drag_row >= 0) {
        slider_set_from_x(s_menu, s_drag_row, lx);
        return;
    }
    t = hit_title(lx, ly);
    /* Rows only exist while a dropdown is OPEN. Hit-testing them on a collapsed
     * bar reports hits in the empty space below it — where no panel is drawn —
     * and that lit up the selected menu's title as "hovered" whenever the
     * pointer merely passed under the bar, over the running game. The click
     * path has always had this guard; the hover path was missing it. */
    r = (t >= 0 || !s_expanded) ? -1 : hit_row(lx, ly);
    if (t != s_hover_menu || r != s_hover_row) {
        s_hover_menu = (t >= 0) ? t : ((r >= 0) ? s_menu : -1);
        s_hover_row = r;
        s_dirty = 1;
    }
    /* Hover-to-open. Once a dropdown is up, sliding along the bar switches
     * menus at once, the way a real menu bar behaves. From the COLLAPSED bar
     * it waits out a dwell instead: the bar sits over the running game, so a
     * pointer merely crossing the top of the screen must not start capturing
     * input. psx_video_menu_tick does the timing. */
    if (t >= 0) {
        if (s_expanded) {
            if (t != s_menu) { s_menu = t; s_dirty = 1; }
            s_hover_title = -1;
        } else if (t != s_hover_title) {
            s_hover_title = t;
            s_hover_title_ms = s_now_ms;
        }
    } else {
        s_hover_title = -1;
    }
}

void psx_video_menu_mouse_release(void) {
    s_drag_row = -1;
}

/* The pointer left the window. Cancels the pending hover-to-open dwell and
 * drops the hover highlight.
 *
 * Needed because the dwell is armed by a mouse-move over a title and cancelled
 * only by a LATER move that lands elsewhere — and no such move ever arrives
 * once the pointer is outside the window. A pointer parked over the bar on its
 * way off-screen therefore kept its timer running and the menu opened itself
 * afterwards, over the running game, with the mouse somewhere else entirely.
 * A drag is left alone: it owns the pointer until the button comes up, which
 * is what lets a slider keep tracking outside the window. */
void psx_video_menu_mouse_leave(void) {
    if (s_drag_row >= 0) return;
    s_hover_title = -1;
    if (s_hover_menu != -1 || s_hover_row != -1) {
        s_hover_menu = s_hover_row = -1;
        s_dirty = 1;
    }
}

void psx_video_menu_tick(unsigned int now_ms) {
    s_now_ms = now_ms;
    if (!s_visible || s_expanded || s_hover_title < 0) return;
    if ((unsigned int)(now_ms - s_hover_title_ms) < VM_HOVER_OPEN_MS) return;
    s_menu = s_hover_title;
    s_expanded = 1;
    s_hover_menu = s_hover_title;
    s_hover_row = -1;
    s_hover_title = -1;
    s_dirty = 1;
}

int psx_video_menu_mouse_click(int win_x, int win_y) {
    int lx, ly, t, r;
    if (!s_visible) return 0;
    to_logical(win_x, win_y, &lx, &ly);
    t = hit_title(lx, ly);
    if (t >= 0) {
        if (s_editing) edit_commit();
        /* Clicking the already-open menu's own title closes it, like a real
         * menu bar; any other title switches to it and opens. */
        if (s_expanded && t == s_menu) {
            s_expanded = 0;
        } else {
            s_menu = t;
            s_expanded = 1;
        }
        s_dirty = 1;
        return 1;
    }
    if (!s_expanded) return 0;   /* bar only: clicks belong to the game */
    r = hit_row(lx, ly);
    if (r >= 0) {
        int k;
        if (s_editing && r != s_item[s_menu]) edit_commit();
        s_item[s_menu] = r;
        /* Slider rows: clicking anywhere on the track jumps there and starts a
         * drag. Clicking the row OUTSIDE the track just selects it, so the
         * label area stays a safe place to click. */
        if (row_is_slider(s_menu, r) && !s_editing) {
            int sx, sy, sw, sh;
            slider_rect(s_menu, r, &sx, &sy, &sw, &sh);
            if (lx >= sx - 4 && lx <= sx + sw + 4) {
                s_drag_row = r;
                slider_set_from_x(s_menu, r, lx);
                s_dirty = 1;
                return 1;
            }
            s_dirty = 1;
            return 1;
        }
        k = row_kind(s_menu, r);
        if (k == IT_ACTION) {
            if (s_menu == MENU_FILE && r == ACT_QUIT) s_quit = 1;
            else if (s_menu == MENU_VIEW) psx_video_menu_hide();
            else {
                /* GAME > SAVE / LOAD STATE raises a one-shot for the host, the
                 * same hands-off way FILE > QUIT does — this module knows
                 * nothing about save states. It then falls into the ordinary
                 * collapse, which is not incidental: the overlay the host is
                 * about to open runs its own pause loop and takes the keyboard
                 * and pad, so an expanded dropdown would fight it for keys. */
                if (s_menu == MENU_GAME) {
                    if (r == 3) s_rewind = 1;
                    else        s_savestate = 1;
                }
                psx_video_menu_collapse();
            }
        } else if (k == IT_NUMBER) {
            if (!s_editing) edit_begin();
        } else {
            cycle_row(s_menu, r, 1);
        }
        s_dirty = 1;
        return 1;
    }
    /* Clicking into the game collapses the dropdown but leaves the bar on
     * screen, and the click is NOT consumed — the game keeps it. */
    psx_video_menu_collapse();
    return 0;
}

int psx_video_menu_handle_key(int key) {
    int rows;
    /* Only an EXPANDED menu captures the keyboard. With just the bar showing,
     * every key belongs to the game. */
    if (!s_expanded) return 0;

    /* Typing takes the whole keyboard: arrows must not navigate away from a
     * half-entered value, and Esc cancels the edit before it closes the menu. */
    if (s_editing) {
        if (key >= SDLK_0 && key <= SDLK_9) {
            edit_digit(key - SDLK_0);
        } else if (key >= SDLK_KP_1 && key <= SDLK_KP_9) {
            edit_digit(key - SDLK_KP_1 + 1);
        } else if (key == SDLK_KP_0) {
            edit_digit(0);
        } else if (key == SDLK_BACKSPACE || key == SDLK_DELETE) {
            edit_backspace();
        } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
            edit_commit();
        } else if (key == SDLK_ESCAPE) {
            edit_cancel();
        } else if (key == SDLK_UP) {
            edit_nudge(+1);
        } else if (key == SDLK_DOWN) {
            edit_nudge(-1);
        } else if (key == SDLK_RIGHT) {
            edit_nudge(+100);
        } else if (key == SDLK_LEFT) {
            edit_nudge(-100);
        }
        return 1;
    }

    rows = menu_rows(s_menu);
    switch (key) {
        case SDLK_ESCAPE:
            psx_video_menu_close();
            return 1;
        case SDLK_LEFT:
            s_menu = (s_menu + MENU_COUNT - 1) % MENU_COUNT;
            s_dirty = 1;
            return 1;
        case SDLK_RIGHT:
            s_menu = (s_menu + 1) % MENU_COUNT;
            s_dirty = 1;
            return 1;
        case SDLK_UP:
            s_item[s_menu] = (s_item[s_menu] + rows - 1) % rows;
            s_dirty = 1;
            return 1;
        case SDLK_DOWN:
            s_item[s_menu] = (s_item[s_menu] + 1) % rows;
            s_dirty = 1;
            return 1;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_SPACE: {
            int k = row_kind(s_menu, s_item[s_menu]);
            if (k == IT_ACTION) {
                if (s_menu == MENU_FILE && s_item[s_menu] == ACT_QUIT)
                    s_quit = 1;
                else if (s_menu == MENU_VIEW)
                    psx_video_menu_hide();
                else {
                    /* See the mouse path: raise the one-shot, then collapse so
                     * the save-state overlay's pause loop owns the keyboard. */
                    if (s_menu == MENU_GAME) {
                        if (s_item[s_menu] == 3) s_rewind = 1;
                        else                     s_savestate = 1;
                    }
                    psx_video_menu_collapse();
                }
            } else if (k == IT_NUMBER) {
                edit_begin();
            } else {
                cycle_row(s_menu, s_item[s_menu], 1);
            }
            return 1;
        }
        default:
            break;
    }
    /* Swallow everything else while open so stray keys never reach the guest. */
    return 1;
}

int psx_video_menu_take_change(PsxVideoMenuState *out) {
    if (!s_changed) return 0;
    s_changed = 0;
    if (out) *out = s_state;
    return 1;
}

int psx_video_menu_take_quit(void) {
    int q = s_quit;
    s_quit = 0;
    return q;
}

int psx_video_menu_take_savestate(void) {
    int s = s_savestate;
    s_savestate = 0;
    return s;
}

int psx_video_menu_take_rewind(void) {
    int r = s_rewind;
    s_rewind = 0;
    return r;
}

/* ---- settings persistence ------------------------------------------------
 * Deliberately a tiny hand-rolled key=value reader rather than a TOML/ini
 * dependency: this is five integers, and the file must stay readable/editable
 * by hand. Unknown keys are ignored so an older build cannot be tripped up by
 * a newer file. */

static int parse_kv(const char *line, char *key, int keysz, int *val) {
    int i = 0, n = 0, neg = 0, seen = 0;
    while (line[i] == ' ' || line[i] == '\t') i++;
    if (line[i] == '#' || line[i] == ';' || line[i] == '\0' ||
        line[i] == '\n' || line[i] == '\r')
        return 0;
    while (line[i] && line[i] != '=' && n < keysz - 1) {
        if (line[i] != ' ' && line[i] != '\t') key[n++] = line[i];
        i++;
    }
    key[n] = '\0';
    if (line[i] != '=') return 0;
    i++;
    while (line[i] == ' ' || line[i] == '\t') i++;
    if (line[i] == '-') { neg = 1; i++; }
    while (line[i] >= '0' && line[i] <= '9') { *val = *val * 10 + (line[i] - '0'); i++; seen = 1; }
    if (!seen) return 0;
    if (neg) *val = -(*val);
    return 1;
}

int psx_video_menu_settings_load(const char *path, PsxVideoMenuState *out) {
    FILE *f;
    char line[128], key[48];
    if (!path || !out) return 0;
    f = fopen(path, "r");
    if (!f) return 0;
    while (fgets(line, (int)sizeof(line), f)) {
        int v = 0;
        if (!parse_kv(line, key, (int)sizeof(key), &v)) continue;
        if      (!strcmp(key, "scaling"))        out->scaling        = v ? 1 : 0;
        else if (!strcmp(key, "present_filter")) out->filter         = v ? 1 : 0;
        else if (!strcmp(key, "texture_filter")) out->texture_filter = v ? 1 : 0;
        else if (!strcmp(key, "screen"))         out->screen = (v >= 0 && v <= 2) ? v : 0;
        else if (!strcmp(key, "vsync"))          out->vsync  = (v >= 0 && v <= 2) ? v : PSX_VM_VSYNC_ON;
        else if (!strcmp(key, "supersampling")) {
            if (v >= 1 && v <= PSX_VM_SUPERSAMPLING_MAX) out->supersampling = v;
        }
        else if (!strcmp(key, "life_points")) {
            if (v >= 1 && v <= 32767) out->life_points = v;
        }
        else if (!strcmp(key, "speed")) {
            if (v >= 1 && v <= PSX_VM_SPEED_MAX) out->speed = v;
        }
        else if (!strcmp(key, "free_spending")) out->free_spending = v ? 1 : 0;
        else if (!strcmp(key, "fast_loads")) out->fast_loads = (v >= 0 && v <= 2) ? v : 0;
        else if (!strcmp(key, "vol_master")) out->vol_master = (v >= 0 && v <= 100) ? v : 100;
        else if (!strcmp(key, "vol_music"))  out->vol_music  = (v >= 0 && v <= 100) ? v : 100;
        else if (!strcmp(key, "vol_sound"))  out->vol_sound  = (v >= 0 && v <= 100) ? v : 100;
        else if (!strcmp(key, "rank_meter")) out->rank_meter = (v >= 0 && v <= 3) ? v : 0;
        else if (!strcmp(key, "fusion_hint")) out->fusion_hint = (v >= 0 && v <= 2) ? v : 0;
        else if (!strcmp(key, "fusion_by_def")) out->fusion_by_def = v ? 1 : 0;
        else if (!strcmp(key, "card_drops")) {
            if (v >= 1 && v <= PSX_VM_CARD_DROPS_MAX) out->card_drops = v;
        }
    }
    fclose(f);
    return 1;
}

int psx_video_menu_settings_save(const char *path) {
    FILE *f;
    if (!path) return 0;
    f = fopen(path, "w");
    if (!f) return 0;
    fprintf(f,
        "# PSXRecomp menu settings. Edit by hand if you like; values are\n"
        "# re-written whenever you change something in the F10 menu.\n"
        "scaling=%d         # 0 fill window, 1 integer (whole pixels)\n"
        "present_filter=%d  # 0 nearest (sharp), 1 linear (smooths whole frame)\n"
        "texture_filter=%d  # 0 nearest (sharp), 1 bilinear (in-game textures)\n"
        "screen=%d          # 0 windowed, 1 borderless, 2 exclusive\n"
        "vsync=%d           # 0 off (lowest lag, tearing), 1 on, 2 adaptive\n"
        "supersampling=%d   # internal render scale 1..4; applies on next launch\n"
        "life_points=%d     # starting duel LP; 8000 is stock\n"
        "speed=%d           # emulation speed multiplier, 1..16 (1 = normal)\n"
        "free_spending=%d   # 1 = refund any StarChip decrease\n"
        "fast_loads=%d      # 0 authentic, 1 fast, 2 instant disc loads\n"
        "vol_master=%d      # 0..100 master volume\n"
        "vol_music=%d       # 0..100 music bus (enveloped SPU voices + CD/XA)\n"
        "vol_sound=%d       # 0..100 sound-effect bus (one-shot voices)\n"
        "rank_meter=%d      # 0 off, 1 live duel rank, 2 rank + worst term\n"
        "card_drops=%d      # cards awarded per duel won, 1..99 (1 = stock)\n"
        "fusion_hint=%d     # 0 off, 1 pick-order numbers, 2 numbers + card\n"
        "fusion_by_def=%d   # 0 suggest best attack, 1 best defense\n",
        s_state.scaling ? 1 : 0,
        s_state.filter ? 1 : 0,
        s_state.texture_filter ? 1 : 0,
        (s_state.screen >= 0 && s_state.screen <= 2) ? s_state.screen : 0,
        (s_state.vsync >= 0 && s_state.vsync <= 2) ? s_state.vsync : PSX_VM_VSYNC_ON,
        (s_state.supersampling >= 1 &&
         s_state.supersampling <= PSX_VM_SUPERSAMPLING_MAX) ? s_state.supersampling : 1,
        s_state.life_points,
        (s_state.speed >= 1 && s_state.speed <= PSX_VM_SPEED_MAX)
            ? s_state.speed : PSX_VM_SPEED_DEFAULT,
        s_state.free_spending ? 1 : 0,
        (s_state.fast_loads >= 0 && s_state.fast_loads <= 2) ? s_state.fast_loads : 0,
        (s_state.vol_master >= 0 && s_state.vol_master <= 100) ? s_state.vol_master : 100,
        (s_state.vol_music  >= 0 && s_state.vol_music  <= 100) ? s_state.vol_music  : 100,
        (s_state.vol_sound  >= 0 && s_state.vol_sound  <= 100) ? s_state.vol_sound  : 100,
        (s_state.rank_meter >= 0 && s_state.rank_meter <= 3) ? s_state.rank_meter : 0,
        (s_state.card_drops >= 1 && s_state.card_drops <= PSX_VM_CARD_DROPS_MAX)
            ? s_state.card_drops : PSX_VM_CARD_DROPS_DEFAULT,
        (s_state.fusion_hint >= 0 && s_state.fusion_hint <= 2)
            ? s_state.fusion_hint : 0,
        s_state.fusion_by_def ? 1 : 0);
    fclose(f);
    return 1;
}

int psx_video_menu_bar_height(void) { return VM_BAR_H; }

int psx_video_menu_ui_scale(int drawable_w, int drawable_h) {
    int mui;
    if (drawable_w <= 0 || drawable_h <= 0) return 1;
    mui = drawable_h / 480;
    if (mui < 1) mui = 1;
    if (mui > 8) mui = 8;
    /* Raise until the logical canvas fits the module's buffer cap. */
    while (mui < 16 && (drawable_w / mui > VM_MAX_W || drawable_h / mui > VM_MAX_H))
        mui++;
    return mui;
}

int psx_video_menu_bar_px(int drawable_w, int drawable_h) {
    if (!s_visible) return 0;
    return VM_BAR_H * psx_video_menu_ui_scale(drawable_w, drawable_h);
}

int psx_video_menu_needs_present(void) { return s_visible; }

int psx_video_menu_overlay_image(const uint32_t **pixels, int *w, int *h) {
    if (!s_visible) return 0;
    if (s_dirty) { redraw(); s_canvas_ready = 1; }
    if (pixels) *pixels = s_canvas;
    if (w) *w = s_lw;
    if (h) *h = s_lh;
    return 1;
}

/* Emu-thread half of the split below: bring the canvas up to date on the
 * thread that OWNS this module's state. */
void psx_video_menu_prepare(void) {
    if (!s_visible) return;
    if (s_dirty) { redraw(); s_canvas_ready = 1; }
}

/* Presentation-thread half: hand back the last rendered canvas and NEVER
 * redraw. redraw() reads every field this module has (rows, selection, hover,
 * the edit buffer) while the emu thread is free to mutate them, so calling it
 * from the interpolation thread is a data race on the whole module — and with
 * FRAME RATE now living in the menu, "menu open while interpolating" is the
 * expected flow rather than a corner case. Worst case here is a canvas one
 * emu frame stale, which at 240 presents/s nobody can see. */
int psx_video_menu_overlay_image_ro(const uint32_t **pixels, int *w, int *h) {
    if (!s_visible || !s_canvas_ready) return 0;
    if (pixels) *pixels = s_canvas;
    if (w) *w = s_lw;
    if (h) *h = s_lh;
    return 1;
}
