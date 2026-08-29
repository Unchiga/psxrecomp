/* Top menu bar overlay: FILE / VIEW / VIDEO / AUDIO / GAME / CHEATS / MODS.
 *
 * Rendered into an ARGB canvas that the presenter blends over the frame, so
 * the game stays visible wherever the bar and the open dropdown do not cover
 * (see gl_draw_osd_image_ex and vk_overlay_pass).
 *
 * WHAT CHANGED, AND WHY IT MATTERS TO ANYONE EDITING THIS
 *
 * The canvas used to be authored small -- capped at 1280x720 -- and magnified
 * by a WHOLE NUMBER, because the text was an 8x8 bitmap sheet and a fractional
 * stretch made its stems uneven. That constraint is gone: text now comes from
 * a real typeface rasterised by stb_truetype at the exact pixel size wanted
 * (psx_ui_font.c), so the canvas is authored at the window's own resolution
 * and a bigger display gets more DETAIL instead of bigger blocks.
 *
 * Two consequences to keep straight:
 *
 *  - Layout is written in DESIGN UNITS, not pixels. One design unit is 1/480
 *    of the canvas height, so the UI keeps the same proportion of the screen
 *    at every window size. S(x) converts; s_unit holds the factor. Never write
 *    a raw pixel count into layout code -- it will be right on one monitor.
 *  - psx_video_menu_ui_scale() now returns 1 for every window up to the canvas
 *    cap. It is NOT dead: past the cap it goes back to whole-number
 *    magnification, which is what keeps an 8K display readable instead of
 *    showing a bar across the left third of the screen. Canvas-to-screen is
 *    that factor; design-to-canvas is s_unit; the product is what the player
 *    sees, and psx_video_menu_bar_px() is the one place that multiplies them.
 *
 * redraw() still runs ONLY when s_dirty, which is what makes a 4K canvas
 * affordable, and it clears only the box it drew last time rather than the
 * whole buffer -- a full 33 MB memset per pointer move during a slider drag is
 * milliseconds of the frame budget for no reason.
 *
 * Labels are mixed case now. Rows registered by titles (psx_video_menu_add_*)
 * are drawn exactly as given: an all-caps string from a game or a mod renders
 * as emphasis rather than as damage, and quietly case-folding somebody else's
 * label would mangle "VSYNC" and "3D" for a cosmetic gain. */

#include "psx_video_menu.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psx_sdl.h"
#include "psx_ui_font.h"

/* Stringify PSX_VM_SPEED_MAX into the SPEED row's hint. Spelling the ceiling
 * out as a literal is what left the hint reading "1 TO 16" after the ceiling
 * moved to 4 — a menu that misstates its own range is worse than one with no
 * hint at all. */
#define PSX_VM_STR2(x) #x
#define PSX_VM_STR(x)  PSX_VM_STR2(x)

/* Canvas ceiling. Sized for a 4K window drawn 1:1; past it the renderer goes
 * back to whole-number magnification (psx_video_menu_ui_scale), so the bar
 * stays full width and readable on an 8K panel instead of running out of
 * canvas. Allocated once at init and never moved, because the presentation
 * thread reads this buffer while the emu thread owns everything else. */
#define VM_MAX_W 3840
#define VM_MAX_H 2160
/* Static fallback, used when the 33 MB allocation fails. Same size as the old
 * fixed canvas, so the worst case is exactly the UI this replaced rather than
 * no menu at all -- and the menu is how a player undoes a bad setting. */
#define VM_FB_W 1280
#define VM_FB_H 720

/* ---- layout, in DESIGN UNITS (1 unit = 1/480 of canvas height) ------------
 *
 * Chosen against a 1080p window (s_unit = 2.25), where these land at: 50 px
 * bar, 43 px rows, 23 px text. Anything here that reads like a magic number is
 * a proportion, not a pixel count -- see the file header. */
#define VM_BAR_H        22.0f   /* menu bar strip */
#define VM_BAR_PAD_X     7.0f   /* inset before the first title */
#define VM_TITLE_PAD    11.0f   /* horizontal padding inside a title chip */
#define VM_TITLE_GAP     1.0f   /* between chips */
#define VM_TITLE_R       6.0f   /* chip corner radius */
#define VM_PANEL_GAP     3.0f   /* bar bottom -> panel top */
#define VM_PANEL_PAD     6.0f   /* panel inner padding */
#define VM_PANEL_R       9.0f
#define VM_ROW_H        19.0f
#define VM_ROW_R         7.0f   /* active-row pill radius */
#define VM_ROW_PAD_X     9.0f   /* text inset inside a row */
#define VM_SEP_H        10.0f   /* band holding the hairline between groups */
#define VM_HINT_GAP      3.0f   /* rows -> hint rule */
#define VM_HINT_H       15.0f   /* hint strip */
#define VM_VALUE_GAP    16.0f   /* minimum label-to-value gutter */
#define VM_PANEL_MIN_W 150.0f
#define VM_SLIDER_W    104.0f   /* drag track on volume / speed / zoom rows */
#define VM_SLIDER_H      4.0f
#define VM_SLIDER_KNOB   6.0f   /* knob radius */
#define VM_CARET         3.5f   /* half-size of the cycle arrows */
#define VM_SHADOW        9.0f   /* how far the panel's shadow reaches */

/* Text sizes, also design units. Two weights: the bar and a selected row are
 * SEMIBOLD so the active thing reads as active at a glance, which is work the
 * old UI could only do with colour. */
#define VM_FS_ICON      13.0f   /* icons read small; sized above the text */
#define VM_ICON_GAP      5.0f   /* icon -> title text */
#define VM_FS_TITLE     10.0f
#define VM_FS_ROW       10.0f
#define VM_FS_HINT       8.8f

/* ---- palette -------------------------------------------------------------
 *
 * Dark navy panel, muted-blue fill behind the active row, bright blue for its
 * text, light grey idle, separators barely there. Alpha is real: the bar and
 * panel are translucent so the game reads through them, which is why the row
 * pill is drawn as a solid over that rather than as a colour swap. */
#define COL_BAR        0xE8141826u
#define COL_BAR_EDGE   0x40FFFFFFu   /* hairline under the bar */
#define COL_PANEL      0xF21E2233u
#define COL_PANEL_ED   0x24FFFFFFu
#define COL_SEP        0x1EFFFFFFu   /* group separator inside the panel */
#define COL_TEXT       0xFFC9CFDDu   /* idle label */
#define COL_DIM        0xFF7C8598u   /* value, hint */
#define COL_ACCENT     0xFF7FA6FFu   /* active text */
#define COL_SEL_BG     0xFF2C3B60u   /* muted blue fill behind the active row */
#define COL_HOVER_BG   0x14FFFFFFu
#define COL_TRACK      0x66101420u   /* slider groove */
#define COL_TICK       0x59FFFFFFu   /* stop marks under a short-range track */
#define COL_SHADOW     0x66000000u   /* lifts the dropdown off the game */
#define COL_EDIT_BG    0xF00E1119u   /* inline numeric field */

/* ---- menu model ---------------------------------------------------------- */

/* MODS is appended rather than inserted: s_item[] is indexed by these values,
 * so renumbering the existing menus would silently move every remembered row
 * selection to a different menu. */
enum { MENU_FILE = 0, MENU_VIEW = 1, MENU_VIDEO = 2, MENU_AUDIO = 3,
       MENU_GAME = 4, MENU_CHEATS = 5, MENU_MODS = 6, MENU_COUNT = 7 };
/* AUDIO rows. MASTER scales everything; MUSIC and SOUND are the split buses. */
enum { AUD_MASTER = 0, AUD_MUSIC = 1, AUD_SOUND = 2, AUD_SPEED_GOV = 3 };

/* ---- per-title registration ---------------------------------------------
 * Rows a game registers live here rather than in the dispatch chains below,
 * which stay the framework's own. Fixed capacity on purpose: this runs before
 * the guest starts and a title registering more than this has a design
 * problem, not an allocation problem. */
#define VM_MENU_MAX 12
#define VM_REG_MAX  32

typedef struct VmRegRow {
    int   menu;
    int   kind;              /* PSX_VM_ROW_* */
    const char *label;
    const char *hint;
    const char *const *choices;
    const char *const *choice_hints;   /* optional; NULL = use ->hint */
    int   choice_count;
    int   lo, hi;
    int   slider;
    int   mark;              /* notch on the track, or -1 */
    const char *key;         /* menu_settings.ini name; NULL = not persisted */
    int   value;
    int   restored;          /* value came from the settings file, not the
                              * registered default -- see
                              * psx_video_menu_apply_restored */
    void (*on_change)(int);
    void (*on_activate)(void);
} VmRegRow;

static VmRegRow    s_reg[VM_REG_MAX];
static int         s_reg_count;
static const char *s_menu_extra_title[VM_MENU_MAX];
static int         s_menu_total = MENU_COUNT;

/* Registered rows for menu m, and the n-th of them. */
static int reg_count_for(int m) {
    int i, n = 0;
    for (i = 0; i < s_reg_count; i++) if (s_reg[i].menu == m) n++;
    return n;
}
/* Defined below, next to builtin_rows: the dispatch chains above need it. */
static struct VmRegRow *row_reg(int m, int row);

static VmRegRow *reg_at(int m, int nth) {
    int i, n = 0;
    for (i = 0; i < s_reg_count; i++) {
        if (s_reg[i].menu != m) continue;
        if (n == nth) return &s_reg[i];
        n++;
    }
    return NULL;
}
enum { IT_OPTION = 0, IT_ACTION = 1, IT_NUMBER = 2 };
/* ACT_DISC sits between CLOSE and QUIT so QUIT stays the last row, where
 * players expect it. Safe to renumber: unlike s_item[]'s MENU_* index, these
 * row constants are never persisted — menu_settings.ini stores options, not
 * the cursor. */
enum { ACT_CLOSE = 0, ACT_DISC = 1, ACT_QUIT = 2 };

#define VM_EDIT_MAX 5   /* digits; 32767 is the widest useful value */
/* Named once: panel_rect reserves width for it so the panel cannot resize
 * the moment a field opens, and row_hint prints it. Two copies of the
 * string is two chances for the reservation to stop matching the text. */
#define VM_EDIT_HINT "Type digits   Enter apply   Esc cancel"
/* Draw a notch per step at or below this many steps. Above it the ticks
 * would be sub-pixel and just muddy the track (a 0..100 volume). */
#define VM_SLIDER_TICK_MAX 16


static const char *const SCALING_LABELS[] = { "Fill window", "Whole pixels" };
static const char *const FILTER_LABELS[]  = { "Nearest \xe2\x80\x94 sharp", "Linear \xe2\x80\x94 smooth" };
static const char *const TEXFILTER_LABELS[] = { "Nearest \xe2\x80\x94 sharp", "Bilinear" };
static const char *const SCREEN_LABELS[]  = { "Windowed", "Borderless", "Exclusive" };
/* Windowed zoom, indexed by scale-1. */
static const char *const WSCALE_LABELS[]  = { "1x", "2x", "3x", "4x",
                                              "5x", "6x", "7x", "8x" };
/* Index order is the cycle order, not the SDL swap interval: the host maps
 * 0/1/2 -> swap interval 0/1/-1 (see psx_apply_video_menu_state). */
static const char *const VSYNC_LABELS[]   = { "Off \xe2\x80\x94 lowest lag", "On \xe2\x80\x94 tear-free",
                                              "Adaptive" };
/* Indexed by scale-1. Cost is ~N^2 in fill rate, so the labels say so. */
static const char *const SSAA_LABELS[]    = { "Native (1x)", "2x", "3x", "4x" };
static const char *const LOADS_LABELS[]   = { "Off \xe2\x80\x94 authentic", "Fast", "Instant" };
static const char *const SPEEDGOV_LABELS[] = { "Off", "On" };
/* Write-only cheat: index IS the number of copies given, 0 = do nothing. */

static int s_visible;    /* bar drawn; does not capture input */
static int s_expanded;   /* dropdown open; DOES capture input */
static int s_menu;
static int s_item[VM_MENU_MAX];
static int s_dirty = 1;
/* Set once redraw() has produced a canvas, so the presentation thread can tell
 * "nothing drawn yet" from "drawn and current" without redrawing itself. */
static int s_canvas_ready;
static int s_changed;
static int s_quit;
static int s_savestate;
static int s_rewind;
static int s_pick_disc;

/* Canvas size in pixels, and the whole-number factor the renderer will
 * magnify it by (1 for every window up to the canvas cap). */
static int s_lw = 640, s_lh = 480, s_ui = 1;
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
    .windowed_scale = PSX_VM_WINDOWED_SCALE_DEFAULT,
    /* OFF. Driver vsync is the emulator's clock only on a ~60 Hz panel, and
     * that path has now been wrong twice for the players who default into
     * it: first a still screen skipped its present and nothing paced the
     * guest at all, then presenting every frame instead traded that for
     * visible judder, because a blocking swap turns any frame overrun into
     * a whole dropped frame where the wall-clock pacer simply absorbs it.
     * The pacer path is the one every report calls smooth. Tearing is the
     * cost, and it is the smaller one; ON remains a row away. */
    .vsync          = PSX_VM_VSYNC_OFF,
    /* UNSET, so an absent key leaves game.toml's choice alone rather than
     * this default silently becoming an override. */
    .renderer       = PSX_VM_RENDERER_UNSET,
    .supersampling  = 1,
    .fast_loads     = PSX_VM_LOADS_OFF,
    .speed          = PSX_VM_SPEED_DEFAULT,
    .vol_master     = 100,
    .vol_music      = 100,
    .vol_sound      = 100,
    .speed_governor = 0,
    .update_check   = 1,
};

/* Inline numeric entry. Active only while the player is typing into a row. */
static int  s_editing;
static char s_edit_buf[VM_EDIT_MAX + 1];
static int  s_edit_len;

/* ---- numeric options -----------------------------------------------------
 * Everything needed to make a row typeable lives in these three functions, so
 * adding another numeric option is one case each rather than new UI code.
 * Ranges are inclusive and enforced on COMMIT, not while typing, so a prefix
 * like "3" on the way to "3000" is not fought by the clamp. */

static int num_range(int m, int row, int *lo, int *hi) {
    {
        VmRegRow *r = row_reg(m, row);
        if (r) {
            if (r->kind != PSX_VM_ROW_NUMBER) return 0;
            *lo = r->lo; *hi = r->hi; return 1;
        }
    }
    if (m == MENU_GAME && row == 0) {
        *lo = 1; *hi = PSX_VM_SPEED_MAX; return 1;
    }
    if (m == MENU_VIDEO && row == 4) {   /* WINDOWED SCALE */
        *lo = PSX_VM_WINDOWED_SCALE_MIN;
        *hi = PSX_VM_WINDOWED_SCALE_MAX; return 1;
    }
    /* The three bus rows are percentages; AUTO SLOW FOR AUDIO is a toggle, and
     * a whole-menu range here would have made it a 0..100 number. */
    if (m == MENU_AUDIO && row != AUD_SPEED_GOV) {
        *lo = 0; *hi = 100; return 1;                        /* percent */
    }
    return 0;
}

static int num_get(int m, int row) {
    { VmRegRow *r = row_reg(m, row); if (r) return r->value; }
    if (m == MENU_GAME && row == 0) return s_state.speed;
    if (m == MENU_VIDEO && row == 4) return s_state.windowed_scale;
    if (m == MENU_AUDIO) {
        if (row == AUD_MASTER) return s_state.vol_master;
        if (row == AUD_MUSIC)  return s_state.vol_music;
        return s_state.vol_sound;
    }
    return 0;
}

static void num_set(int m, int row, int v) {
    {
        VmRegRow *r = row_reg(m, row);
        if (r) {
            if (r->value == v) return;
            r->value = v;
            if (r->on_change) r->on_change(v);
            return;
        }
    }
    if (m == MENU_GAME && row == 0) s_state.speed = v;
    if (m == MENU_VIDEO && row == 4) s_state.windowed_scale = v;
    if (m == MENU_AUDIO) {
        if (row == AUD_MASTER)     s_state.vol_master = v;
        else if (row == AUD_MUSIC) s_state.vol_music  = v;
        else                       s_state.vol_sound  = v;
    }
}

static const char *menu_title(int m) {
    if (m == MENU_FILE) return "File";
    if (m == MENU_VIEW) return "View";
    if (m == MENU_VIDEO) return "Video";
    if (m == MENU_AUDIO) return "Audio";
    if (m == MENU_GAME) return "Game";
    if (m == MENU_CHEATS) return "Cheats";
    if (m == MENU_MODS) return "Mods";
    if (m >= MENU_COUNT && m < s_menu_total && s_menu_extra_title[m])
        return s_menu_extra_title[m];
    return "Mods";
}

/* The icon beside a top-level title.
 *
 * Framework menus get a specific one; anything a TITLE registered gets the
 * generic "tune", because this module cannot know what a game's own menu is
 * about and must not pretend to. That is the same reason there is no icon
 * argument on psx_video_menu_add_menu: an icon nobody can choose sensibly is
 * better left as one honest default than as a required guess at registration
 * time. */
static unsigned menu_icon(int m) {
    switch (m) {
        case MENU_FILE:   return PSX_UI_ICON_FOLDER;
        case MENU_VIEW:   return PSX_UI_ICON_EYE;
        case MENU_VIDEO:  return PSX_UI_ICON_MONITOR;
        case MENU_AUDIO:  return PSX_UI_ICON_VOLUME;
        case MENU_GAME:   return PSX_UI_ICON_GAMEPAD;
        case MENU_CHEATS: return PSX_UI_ICON_BOLT;
        case MENU_MODS:   return PSX_UI_ICON_EXTENSION;
        default:          return PSX_UI_ICON_TUNE;
    }
}

static int builtin_rows(int m) {
    if (m == MENU_FILE) return 3;
    if (m == MENU_VIEW) return 1;
    if (m == MENU_VIDEO) return 7;
    if (m == MENU_AUDIO) return 4;
    if (m == MENU_CHEATS) return 0;   /* titles fill this; empty until they do */
    if (m == MENU_GAME) return 4;
    if (m == MENU_MODS) return 0;   /* mods fill this; empty until they do */
    return 1;
}

/* Built-in rows first, then whatever the title registered. */
static int menu_rows(int m) { return builtin_rows(m) + reg_count_for(m); }

/* The registered row at this index, or NULL when the index is built-in. */
static VmRegRow *row_reg(int m, int row) {
    const int b = builtin_rows(m);
    if (row < b) return NULL;
    return reg_at(m, row - b);
}

static int row_kind(int m, int row) {
    int lo, hi;
    {
        VmRegRow *r = row_reg(m, row);
        if (r) return (r->kind == PSX_VM_ROW_ACTION) ? IT_ACTION
                    : (r->kind == PSX_VM_ROW_NUMBER) ? IT_NUMBER : IT_OPTION;
    }
    if (m == MENU_FILE) return IT_ACTION;
    /* VIEW row 0 is the only ACTION here (it hides the bar); row 1 is an
     * ordinary cycling option. Returning IT_ACTION for the whole menu, as this
     * did while VIEW had a single row, would make picking the new row hide the
     * bar instead of changing it. */
    if (m == MENU_VIEW) return IT_ACTION;   /* MENU BAR; the rest are registered */
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
    { VmRegRow *r = row_reg(m, row); if (r) return r->label; }
    if (m == MENU_FILE)
        return (row == 0) ? "Close menu"
             : (row == 1) ? "Change game disc"
                          : "Quit";
    if (m == MENU_VIEW) return "Menu bar";
    if (m == MENU_GAME)
        return (row == 0) ? "Speed"
             : (row == 1) ? "Fast loading"
             : (row == 2) ? "Save / load state"
                          : "Rewind";
    if (m == MENU_AUDIO)
        return (row == AUD_MASTER) ? "Master"
             : (row == AUD_MUSIC)  ? "Music"
             : (row == AUD_SOUND)  ? "Sound effects"
                                   : "Auto slow for audio";
    switch (row) {
        case 0:  return "Scaling";
        case 1:  return "Present filter";
        case 2:  return "Texture filter";
        case 3:  return "Screen";
        case 4:  return "Windowed scale";
        case 5:  return "VSync";
        default: return "Resolution";
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
    {
        VmRegRow *r = row_reg(m, row);
        if (r && r->kind == PSX_VM_ROW_ACTION) return "";
        if (r && r->kind == PSX_VM_ROW_OPTION) {
            if (r->choices && r->value >= 0 && r->value < r->choice_count)
                return r->choices[r->value];
            return "";
        }
        /* NUMBER falls through: num_range() below already answers for
         * registered rows, and that path owns the typing caret. */
    }
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
        /* WINDOWED SCALE reads as a zoom factor, not a count: the slider now
         * owns this row, and the numeric path below would print a bare "3"
         * where "3X" is what tells the player it means three times native. */
        if (m == MENU_VIDEO && row == 4) {
            static char zbuf[8];
            const char *d = lp_text(num_get(m, row));
            int i = 0;
            while (d[i] && i < (int)sizeof(zbuf) - 2) { zbuf[i] = d[i]; i++; }
            zbuf[i] = 'x';
            zbuf[i + 1] = '\0';
            return zbuf;
        }
        return lp_text(num_get(m, row));
    }
    if (m == MENU_VIEW) return "Visible   F10";
    if (m == MENU_AUDIO && row == AUD_SPEED_GOV)
        return SPEEDGOV_LABELS[s_state.speed_governor ? 1 : 0];
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
        case 4:  return WSCALE_LABELS[
                     (s_state.windowed_scale >= PSX_VM_WINDOWED_SCALE_MIN &&
                      s_state.windowed_scale <= PSX_VM_WINDOWED_SCALE_MAX)
                         ? s_state.windowed_scale - 1
                         : PSX_VM_WINDOWED_SCALE_DEFAULT - 1];
        case 5:  return VSYNC_LABELS[(s_state.vsync >= 0 && s_state.vsync <= 2)
                                          ? s_state.vsync : 1];
        default: return SSAA_LABELS[(s_state.supersampling >= 1 &&
                                     s_state.supersampling <= PSX_VM_SUPERSAMPLING_MAX)
                                        ? s_state.supersampling - 1 : 0];
    }
}

static const char *row_hint(int m, int row) {
    int lo, hi;
    {
        VmRegRow *r = row_reg(m, row);
        if (r && !(s_editing && m == s_menu && row == s_item[s_menu])) {
            if (r->choice_hints && r->kind == PSX_VM_ROW_OPTION &&
                r->value >= 0 && r->value < r->choice_count &&
                r->choice_hints[r->value])
                return r->choice_hints[r->value];
            return r->hint ? r->hint : "";
        }
    }
    if (s_editing && m == s_menu && row == s_item[s_menu])
        return VM_EDIT_HINT;
    if (m == MENU_FILE)
        return (row == 0) ? "Close this menu"
             : (row == 1) ? "Pick your disc again if it moved \xe2\x80\x94 applies on restart"
                          : "Exit the game";
    if (m == MENU_VIEW) return "Press F10 any time to show or hide";
    if (m == MENU_GAME && row == 1)
        return s_state.fast_loads == PSX_VM_LOADS_OFF
                   ? "Real drive timing"
             : s_state.fast_loads == PSX_VM_LOADS_FAST
                   ? "Shorter loads \xe2\x80\x94 game speed unchanged"
                   : "Fastest \xe2\x80\x94 back off if a load stalls";
    /* Says the quiet part out loud: the overlay this opens freezes the guest,
     * which is expected from a hotkey but surprising from a menu row. */
    if (m == MENU_GAME && row == 2)
        return "Pauses the game until you pick a slot";
    /* Names the hotkey as well as the effect: the row exists because the
     * feature was previously reachable only by a key nothing advertised. */
    if (m == MENU_GAME && row == 3)
        return "Step back through recent frames \xe2\x80\x94 also F8";
    if (m == MENU_AUDIO && row == AUD_SPEED_GOV)
        return s_state.speed_governor
                   ? "Drops speed in heavy scenes to keep sound clean"
                   : "Speed stays put \xe2\x80\x94 sound may break up if it cannot keep up";
    if (m == MENU_AUDIO)
        return (row == AUD_MASTER)
                   ? "Scales everything \xe2\x80\x94 Enter to type"
             : (row == AUD_MUSIC)
                   ? "BGM and CD audio \xe2\x80\x94 Enter to type"
                   : "Sound effects only \xe2\x80\x94 Enter to type";
    if (num_range(m, row, &lo, &hi)) {
        if (m == MENU_GAME && row == 0)
            /* No longer "audio may distort": the pacer and the guest VBlank
             * period now scale together, so device time — and with it the
             * SPU's 44.1 kHz — is unchanged at every setting. */
            return (s_state.speed <= 1)
                       ? "1x is normal speed. 1 to "
                         PSX_VM_STR(PSX_VM_SPEED_MAX)
                       : "Faster game, music stays normal";
        return "Enter to type a value";
    }
    switch (row) {
        case 0:
            return s_state.scaling
                ? "Whole pixels \xe2\x80\x94 sharp, may border"
                : "Fills the window \xe2\x80\x94 uneven pixels";
        case 1:
            return s_state.filter
                ? "Smooths the whole image"
                : "No smoothing \xe2\x80\x94 crisp pixels";
        case 2:
            return s_state.texture_filter
                ? "Smooths in-game textures only"
                : "Raw texels \xe2\x80\x94 the original look";
        case 3:
            return "Alt+Enter also toggles";
        /* Names the precondition instead of silently doing nothing. A row
         * that ignores you without saying why reads as broken. */
        case 4:
            return (s_state.screen != PSX_VM_SCREEN_WINDOWED)
                ? "Needs Screen = Windowed"
                : (s_state.scaling != PSX_VM_SCALING_INTEGER
                       ? "Needs Scaling = Whole pixels"
                       : "Resizes the window to whole pixels");
        case 5:
            return s_state.vsync == PSX_VM_VSYNC_OFF
                ? "Lowest input lag \xe2\x80\x94 may tear"
                : (s_state.vsync == PSX_VM_VSYNC_ADAPTIVE
                       ? "Tear-free, tears if a frame is late"
                       : "Tear-free \xe2\x80\x94 adds up to one refresh");
        default:
            return s_state.supersampling <= 1
                ? "Sharper 3D. Takes effect on restart"
                : "Sharper 3D \xe2\x80\x94 costs fill rate. On restart";
    }
}

/* How many values an option row cycles through; 1 or 0 means it does not
 * cycle. Mirrors cycle_row -- the arrows drawn beside a value are a promise
 * that left/right do something, so the two have to agree row for row. */
static int row_choices(int m, int row) {
    { VmRegRow *r = row_reg(m, row);
      if (r) return (r->kind == PSX_VM_ROW_OPTION) ? r->choice_count : 0; }
    if (m == MENU_AUDIO) return (row == AUD_SPEED_GOV) ? 2 : 0;
    if (m == MENU_GAME)  return (row == 1) ? 3 : 0;
    if (m != MENU_VIDEO) return 0;
    switch (row) {
        case 0: case 1: case 2: return 2;
        case 3: case 5:         return 3;
        case 4:                 return 0;   /* number row, has a slider */
        default:                return PSX_VM_SUPERSAMPLING_MAX;
    }
}

static void cycle_row(int m, int row, int delta) {
    {
        VmRegRow *r = row_reg(m, row);
        if (r) {
            if (r->kind != PSX_VM_ROW_OPTION || r->choice_count <= 0) return;
            int v = r->value + delta;
            while (v < 0) v += r->choice_count;
            v %= r->choice_count;
            if (v != r->value) {
                r->value = v;
                if (r->on_change) r->on_change(v);
                s_changed = 1;
                s_dirty = 1;
            }
            return;
        }
    }
    if (m == MENU_AUDIO && row == AUD_SPEED_GOV) {
        s_state.speed_governor = s_state.speed_governor ? 0 : 1;
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
                const int span = PSX_VM_WINDOWED_SCALE_MAX -
                                 PSX_VM_WINDOWED_SCALE_MIN + 1;
                int v = s_state.windowed_scale + delta;
                while (v < PSX_VM_WINDOWED_SCALE_MIN) v += span;
                while (v > PSX_VM_WINDOWED_SCALE_MAX) v -= span;
                s_state.windowed_scale = v;
                break;
            }
            case 5: {
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

/* ---- scale, faces, canvas ------------------------------------------------ */

/* Design unit -> canvas pixel. Derived from the canvas HEIGHT alone: the UI
 * should be the same fraction of the screen on a 16:10 panel as on a 21:9 one,
 * and only the height is common to both. */
static float s_unit = 1.0f;

/* Rounded to whole pixels. Layout that lands on half-pixels puts a row's
 * highlight one pixel off from the row it highlights, once per frame, in a
 * different direction depending on where it started. */
static int S(float design) {
    int v = (int)(design * s_unit + 0.5f);
    return v;
}

static float unit_for(int canvas_h) {
    float u = (float)canvas_h / 480.0f;
    if (u < 1.0f) u = 1.0f;
    if (u > 8.0f) u = 8.0f;
    return u;
}

static const PsxUiFace *face_title(void) {
    return psx_ui_font_face(VM_FS_TITLE * s_unit, PSX_UI_FONT_SEMIBOLD);
}
static const PsxUiFace *face_row(int selected) {
    return psx_ui_font_face(VM_FS_ROW * s_unit,
                            selected ? PSX_UI_FONT_SEMIBOLD
                                     : PSX_UI_FONT_REGULAR);
}
static const PsxUiFace *face_hint(void) {
    return psx_ui_font_face(VM_FS_HINT * s_unit, PSX_UI_FONT_REGULAR);
}
static const PsxUiFace *face_icon(void) {
    return psx_ui_font_face(VM_FS_ICON * s_unit, PSX_UI_FONT_ICONS);
}

/* Width an icon occupies in the bar, gap included, or 0 when the icon face
 * could not be baked -- in which case the titles simply close up and the bar
 * still works. Measured from the face rather than assumed square, so a
 * different icon set would not silently overlap the text. */
static int icon_slot_w(void) {
    char buf[5];
    const PsxUiFace *fi = face_icon();
    int w;
    if (!fi) return 0;
    w = psx_ui_font_text_w(fi, psx_ui_font_utf8(PSX_UI_ICON_TUNE, buf));
    return w > 0 ? w + S(VM_ICON_GAP) : 0;
}

/* The canvas. Allocated once (psx_video_menu_init) at the full cap and never
 * reallocated: psx_video_menu_overlay_image_ro hands this pointer to the
 * presentation thread, so moving it under that thread is a use-after-free, not
 * a resize. s_cap_* record what actually came back, and ui_scale below reads
 * them, so a failed allocation degrades to the old magnified 1280x720 UI
 * rather than to a bar that covers a third of the window. */
static uint32_t  s_canvas_fb[VM_FB_W * VM_FB_H];
static uint32_t *s_canvas    = s_canvas_fb;
static int       s_cap_w     = VM_FB_W;
static int       s_cap_h     = VM_FB_H;

static void canvas_alloc(void) {
    uint32_t *p;
    if (s_canvas != s_canvas_fb) return;          /* already have the big one */
    p = (uint32_t *)malloc((size_t)VM_MAX_W * (size_t)VM_MAX_H * sizeof(uint32_t));
    if (!p) return;
    s_canvas = p;
    s_cap_w  = VM_MAX_W;
    s_cap_h  = VM_MAX_H;
}

/* Bounding box of everything the last redraw() touched, so the next one clears
 * that instead of the whole canvas. Empty (w == 0) means nothing to clear. */
static int s_dirty_x, s_dirty_y, s_dirty_w, s_dirty_h;

/* Rows of the canvas that actually carry anything, which is what the accessors
 * report as its height.
 *
 * This is a bandwidth control, not a cosmetic one. Both backends re-upload the
 * whole reported canvas every presented frame (glTexSubImage2D / the Vulkan
 * staging copy), and the canvas is now the width of the window -- so reporting
 * all 1080 rows would mean 8 MB a frame at 1080p and 33 MB at 4K, for a strip
 * of menu bar over a mostly-empty buffer. The drawn region always starts at
 * row 0, so a prefix is all anyone needs: during play that is the ~50 px bar
 * alone, which is LESS traffic than the old magnified canvas cost. It grows
 * while a dropdown is open, which is exactly when nobody is playing. */
static int s_used_h = 1;

static void mark_drawn(int x, int y, int w, int h) {
    int x1, y1;
    if (w <= 0 || h <= 0) return;
    if (s_dirty_w <= 0) { s_dirty_x = x; s_dirty_y = y; s_dirty_w = w; s_dirty_h = h; return; }
    x1 = s_dirty_x + s_dirty_w; y1 = s_dirty_y + s_dirty_h;
    if (x < s_dirty_x) s_dirty_x = x;
    if (y < s_dirty_y) s_dirty_y = y;
    if (x + w > x1) x1 = x + w;
    if (y + h > y1) y1 = y + h;
    s_dirty_w = x1 - s_dirty_x;
    s_dirty_h = y1 - s_dirty_y;
}

/* ---- geometry (shared by drawing and hit-testing) ------------------------ */

static int text_w(const char *s, const PsxUiFace *f) {
    return psx_ui_font_text_w(f, s);
}

/* Width of a title's chip: padding, icon slot, gap, label. */
static int title_w(int m) {
    return S(VM_TITLE_PAD) * 2 + icon_slot_w() +
           text_w(menu_title(m), face_title());
}

static int title_x(int m) {
    int x = S(VM_BAR_PAD_X), i;
    for (i = 0; i < m; i++) x += title_w(i) + S(VM_TITLE_GAP);
    return x;
}

/* Rows a menu owns itself, which is also where the group separator goes. A
 * menu with no built-in rows (CHEATS, MODS) or none registered gets no rule:
 * a separator with nothing on one side of it is just a stray line. */
static int has_sep(int m) {
    return builtin_rows(m) > 0 && reg_count_for(m) > 0;
}

/* Top edge of row `row`'s band, relative to the canvas. One function so
 * drawing and hit-testing cannot drift -- they did not share this before, and
 * the separator makes the mapping non-linear. */
static int rows_y0(void) {
    return S(VM_BAR_H) + S(VM_PANEL_GAP) + S(VM_PANEL_PAD);
}

static int row_y(int m, int row) {
    int y = rows_y0() + row * S(VM_ROW_H);
    if (has_sep(m) && row >= builtin_rows(m)) y += S(VM_SEP_H);
    return y;
}

/* Bottom of the last row's band. */
static int rows_y1(int m) {
    int rows = menu_rows(m);
    return rows > 0 ? row_y(m, rows - 1) + S(VM_ROW_H) : rows_y0();
}

/* Space a slider's numeric readout needs to its right. Widest built-in value
 * is a 0..100 volume; the padding either side is what keeps it off the panel
 * edge. Shared by the measure pass and the track's own placement so the two
 * cannot disagree. */
static int row_is_slider(int m, int row);
static int row_choices(int m, int row);

static int slider_readout_w(const PsxUiFace *f) {
    return text_w("100", f) + S(VM_ROW_PAD_X) * 2;
}

static void panel_rect(int m, int *px, int *pw, int *ph) {
    const PsxUiFace *fl = face_row(1);   /* SEMIBOLD: the widest a row gets */
    const PsxUiFace *fh = face_hint();
    int rows = menu_rows(m);
    int x = title_x(m);
    int w = 0, i, has_number = 0, lo, hi;
    int pad = S(VM_ROW_PAD_X), inner = S(VM_PANEL_PAD);

    /* MEASURE the content instead of guessing a per-menu constant. The old
     * fixed widths (420/340/300/230) did not actually fit their menus: a value
     * or a hint longer than the guess simply ran past the panel edge and was
     * clipped by the canvas. Anything added to a menu later would silently
     * reintroduce that, which is how VSYNC and the AUDIO rows hit it. */
    for (i = 0; i < rows; i++) {
        const char *v = row_value(m, i);
        const char *h = row_hint(m, i);
        int need = inner * 2 + pad * 2 + text_w(row_label(m, i), fl);
        /* A slider row's right-hand side is the track plus a numeric readout,
         * NOT the value string's width -- measuring the string is how "Speed"
         * ended up ellipsised to nothing while its slider had room to spare. */
        if (row_is_slider(m, i))
            need += S(VM_VALUE_GAP) + S(VM_SLIDER_W) + slider_readout_w(fl);
        else if (v && *v) {
            need += S(VM_VALUE_GAP) + text_w(v, fl);
            /* Room for the cycle arrows on every row that can show them, not
             * just the one selected now -- reserved here, the panel keeps its
             * width as the selection moves instead of breathing in and out. */
            if (row_choices(m, i) > 1) need += S(VM_CARET) * 4 + S(VM_VALUE_GAP);
        }
        if (need > w) w = need;
        if (h) {
            int hw = inner * 2 + pad * 2 + text_w(h, fh);
            if (hw > w) w = hw;
        }
        if (num_range(m, i, &lo, &hi)) has_number = 1;
    }
    /* Typing swaps in a long "Type digits / Enter apply / Esc cancel" hint.
     * Reserve it up front so the panel does not resize mid-edit. */
    if (has_number) {
        int ew = inner * 2 + pad * 2 + text_w(VM_EDIT_HINT, fh);
        if (ew > w) w = ew;
    }
    if (w < S(VM_PANEL_MIN_W)) w = S(VM_PANEL_MIN_W);
    if (w > s_lw) w = s_lw;          /* never wider than the canvas */
    if (x + w > s_lw) x = s_lw - w;
    if (x < 0) x = 0;
    *px = x;
    *pw = w;
    *ph = rows_y1(m) + S(VM_HINT_GAP) + S(VM_HINT_H) + S(VM_PANEL_PAD)
          - (S(VM_BAR_H) + S(VM_PANEL_GAP));
}

/* Slider rows: a draggable track instead of type-only entry.
 *
 * Volume is the one option people adjust by feel rather than by number, so the
 * AUDIO rows carry a track you can click or drag anywhere along. Typing still
 * works (ENTER opens the field) — this is an additional way in, not a
 * replacement. Only rows that report a range AND opt in get one, so the
 * cheat/speed fields keep their exact-entry behaviour. */
static int row_is_slider(int m, int row) {
    int lo, hi;
    { VmRegRow *r = row_reg(m, row); if (r) return r->slider ? 1 : 0; }
    if (!num_range(m, row, &lo, &hi)) return 0;
    if (m == MENU_AUDIO) return 1;
    if (m == MENU_GAME) return 1;                 /* SPEED: slider row */
    if (m == MENU_VIDEO && row == 4) return 1;    /* WINDOWED SCALE 1..8 */
    return 0;   /* built-in number rows are type-only unless listed above */
}

/* A value worth marking on the track, or -1. Only registered rows carry one
 * (see psx_video_menu_set_row_mark); no built-in row asks for a notch. */
static int slider_mark(int m, int row) {
    { VmRegRow *r = row_reg(m, row); if (r) return r->mark; }
    return -1;
}

/* Track rect for a slider row, in canvas pixels. Right-aligned, leaving room
 * for the numeric readout beyond it. */
static void slider_rect(int m, int row, int *sx, int *sy, int *sw, int *sh) {
    int px, pw, ph;
    const PsxUiFace *f = face_row(1);
    int readout = slider_readout_w(f);
    panel_rect(m, &px, &pw, &ph);
    *sw = S(VM_SLIDER_W);
    *sh = S(VM_SLIDER_H);
    *sx = px + pw - *sw - readout;
    *sy = row_y(m, row) + (S(VM_ROW_H) - *sh) / 2;
}

/* Which row's slider is being dragged, or -1. Held across motion events so a
 * drag keeps control even when the pointer strays off the track. */
static int s_drag_row = -1;

/* Where the KNOB's centre may sit, as a start x and a span. The knob is a
 * circle drawn on the value, so its centre has to stay one radius in from each
 * end or half of it hangs off the track -- and the click-to-value mapping has
 * to use the same range, or the knob lands somewhere other than the pointer. */
static void slider_travel(int sx, int sw, int *x0, int *span) {
    int k = S(VM_SLIDER_KNOB);
    *x0 = sx + k;
    *span = sw - k * 2;
    if (*span < 1) { *x0 = sx; *span = sw > 1 ? sw - 1 : 1; }
}

static void slider_set_from_x(int m, int row, int lx) {
    int sx, sy, sw, sh, lo = 0, hi = 0, v, tx0, span;
    if (!num_range(m, row, &lo, &hi)) return;
    slider_rect(m, row, &sx, &sy, &sw, &sh);
    if (sw <= 1) return;
    slider_travel(sx, sw, &tx0, &span);
    v = lo + ((lx - tx0) * (hi - lo) + span / 2) / span;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    if (v != num_get(m, row)) {
        num_set(m, row, v);
        s_changed = 1;
        s_dirty = 1;
    }
}

/* ---- drawing -------------------------------------------------------------
 *
 * Everything is composited, not written: the canvas is straight (non-
 * premultiplied) ARGB that the presenter blends with SRC_ALPHA /
 * ONE_MINUS_SRC_ALPHA, so a shape drawn over the transparent part has to carry
 * its own alpha out or it disappears at present time. That is also why the
 * rounded rects below compute a coverage value rather than a hard mask -- the
 * antialiased edge IS partial alpha. */

static void blend_px(int x, int y, uint32_t argb, float cov) {
    uint32_t *p, d;
    unsigned sa, da, oa, i, sc[3], dc[3], oc[3];

    if ((unsigned)x >= (unsigned)s_lw || (unsigned)y >= (unsigned)s_lh) return;
    if (cov <= 0.0f) return;
    if (cov > 1.0f) cov = 1.0f;

    sa = (unsigned)(((argb >> 24) & 0xFFu) * cov + 0.5f);
    if (!sa) return;

    p = &s_canvas[(size_t)y * s_lw + x];
    if (sa == 255u) { *p = 0xFF000000u | (argb & 0x00FFFFFFu); return; }

    d  = *p;
    da = (d >> 24) & 0xFFu;
    sc[0] = (argb >> 16) & 0xFFu; sc[1] = (argb >> 8) & 0xFFu; sc[2] = argb & 0xFFu;
    dc[0] = (d >> 16) & 0xFFu;    dc[1] = (d >> 8) & 0xFFu;    dc[2] = d & 0xFFu;

    oa = sa + da * (255u - sa) / 255u;
    if (!oa) { *p = 0; return; }
    for (i = 0; i < 3; i++)
        oc[i] = (sc[i] * sa + dc[i] * da * (255u - sa) / 255u) / oa;
    *p = (oa << 24) | (oc[0] << 16) | (oc[1] << 8) | oc[2];
}

static void fill_rect(int x0, int y0, int w, int h, uint32_t col) {
    int x, y;
    if (x0 < 0) { w += x0; x0 = 0; }
    if (y0 < 0) { h += y0; y0 = 0; }
    if (x0 + w > s_lw) w = s_lw - x0;
    if (y0 + h > s_lh) h = s_lh - y0;
    if (w <= 0 || h <= 0) return;
    mark_drawn(x0, y0, w, h);
    for (y = y0; y < y0 + h; y++)
        for (x = x0; x < x0 + w; x++)
            blend_px(x, y, col, 1.0f);
}

/* Signed distance from (px,py) to a rounded rect. Negative inside. This is the
 * whole antialiasing story: coverage is 0.5 - d clamped to 0..1, which is the
 * exact area of a pixel covered by a straight edge and close enough on a
 * curve at any radius the UI actually uses. */
static float rr_dist(float px, float py, float cx, float cy,
                     float hw, float hh, float r) {
    float qx, qy, ax, ay, m;
    if (r > hw) r = hw;
    if (r > hh) r = hh;
    qx = (px > cx ? px - cx : cx - px) - (hw - r);
    qy = (py > cy ? py - cy : cy - py) - (hh - r);
    ax = qx > 0.0f ? qx : 0.0f;
    ay = qy > 0.0f ? qy : 0.0f;
    m  = qx > qy ? qx : qy;
    return (float)sqrt((double)(ax * ax + ay * ay)) + (m < 0.0f ? m : 0.0f) - r;
}

/* Clamp a radius to something the rect can actually hold. */
static float rr_clamp(int w, int h, float r) {
    float lim = (float)(w < h ? w : h) * 0.5f;
    if (r < 0.0f) r = 0.0f;
    if (r > lim) r = lim;
    return r;
}

/* Filled rounded rect.
 *
 * The per-pixel distance function is only needed in the four corner bands: the
 * rows between them are a solid span, and because x/y/w/h are whole pixels
 * their left and right edges land exactly on pixel boundaries with no partial
 * coverage to compute. Skipping the square root on those rows is what keeps a
 * 4K redraw cheap -- the panel alone is a quarter of a million pixels and
 * redraw() runs on every pointer move during a slider drag. */
static void round_rect(int x, int y, int w, int h, float r, uint32_t col) {
    float cx, cy, hw, hh;
    int iy, ix, y0, y1, x0, x1, band0, band1;

    if (w <= 0 || h <= 0 || !(col >> 24)) return;
    r = rr_clamp(w, h, r);
    x0 = x < 0 ? 0 : x;
    y0 = y < 0 ? 0 : y;
    x1 = x + w > s_lw ? s_lw : x + w;
    y1 = y + h > s_lh ? s_lh : y + h;
    if (x1 <= x0 || y1 <= y0) return;
    mark_drawn(x0, y0, x1 - x0, y1 - y0);

    cx = (float)x + (float)w * 0.5f;
    cy = (float)y + (float)h * 0.5f;
    hw = (float)w * 0.5f;
    hh = (float)h * 0.5f;
    band0 = y + (int)r + 1;                 /* first fully straight row */
    band1 = y + h - (int)r - 1;             /* last  fully straight row */

    for (iy = y0; iy < y1; iy++) {
        if (iy >= band0 && iy < band1) {
            for (ix = x0; ix < x1; ix++) blend_px(ix, iy, col, 1.0f);
            continue;
        }
        for (ix = x0; ix < x1; ix++) {
            float cov = 0.5f - rr_dist((float)ix + 0.5f, (float)iy + 0.5f,
                                       cx, cy, hw, hh, r);
            if (cov <= 0.0f) continue;
            blend_px(ix, iy, col, cov);
        }
    }
}

/* Outline of a rounded rect, `width` px wide, centred on the edge. Only the
 * pixels near an edge can be lit, so the straight middle rows are visited at
 * their two ends and skipped in between. */
static void round_rect_line(int x, int y, int w, int h, float r,
                            uint32_t col, float width) {
    float cx, cy, hw, hh, half;
    int iy, ix, y0, y1, x0, x1, band0, band1, reach;

    if (w <= 0 || h <= 0 || !(col >> 24)) return;
    if (width <= 0.0f) width = 1.0f;
    r = rr_clamp(w, h, r);
    half = width * 0.5f;
    reach = (int)half + 2;

    x0 = x - reach < 0 ? 0 : x - reach;
    y0 = y - reach < 0 ? 0 : y - reach;
    x1 = x + w + reach > s_lw ? s_lw : x + w + reach;
    y1 = y + h + reach > s_lh ? s_lh : y + h + reach;
    if (x1 <= x0 || y1 <= y0) return;
    mark_drawn(x0, y0, x1 - x0, y1 - y0);

    cx = (float)x + (float)w * 0.5f;
    cy = (float)y + (float)h * 0.5f;
    hw = (float)w * 0.5f;
    hh = (float)h * 0.5f;
    band0 = y + (int)r + 1;
    band1 = y + h - (int)r - 1;

    for (iy = y0; iy < y1; iy++) {
        int straight = (iy >= band0 && iy < band1);
        for (ix = x0; ix < x1; ix++) {
            float d, ad, cov;
            if (straight && ix >= x + reach && ix < x + w - reach) {
                ix = x + w - reach - 1;     /* skip the hollow middle */
                continue;
            }
            d = rr_dist((float)ix + 0.5f, (float)iy + 0.5f, cx, cy, hw, hh, r);
            ad = d < 0.0f ? -d : d;
            cov = half + 0.5f - ad;
            if (cov <= 0.0f) continue;
            blend_px(ix, iy, col, cov);
        }
    }
}

/* Soft drop shadow OUTSIDE a rounded rect: coverage falls off as the square of
 * the distance from the edge, over `spread` pixels.
 *
 * This is what lifts the dropdown off the game underneath it. Drawn as one
 * pass over the ring rather than as a stack of ever-larger rounded rects: the
 * stack costs a full fill per layer over the whole panel, and the panel is the
 * most expensive thing this module draws. */
static void round_rect_shadow(int x, int y, int w, int h, float r,
                              uint32_t col, int spread) {
    float cx, cy, hw, hh, sp;
    int iy, ix, y0, y1, x0, x1, band0, band1;

    if (w <= 0 || h <= 0 || spread <= 0 || !(col >> 24)) return;
    r = rr_clamp(w, h, r);
    sp = (float)spread;

    x0 = x - spread < 0 ? 0 : x - spread;
    y0 = y - spread < 0 ? 0 : y - spread;
    x1 = x + w + spread > s_lw ? s_lw : x + w + spread;
    y1 = y + h + spread > s_lh ? s_lh : y + h + spread;
    if (x1 <= x0 || y1 <= y0) return;
    mark_drawn(x0, y0, x1 - x0, y1 - y0);

    cx = (float)x + (float)w * 0.5f;
    cy = (float)y + (float)h * 0.5f;
    hw = (float)w * 0.5f;
    hh = (float)h * 0.5f;
    band0 = y + (int)r + 1;
    band1 = y + h - (int)r - 1;

    for (iy = y0; iy < y1; iy++) {
        int straight = (iy >= band0 && iy < band1);
        for (ix = x0; ix < x1; ix++) {
            float d, t;
            if (straight && ix >= x && ix < x + w) {
                ix = x + w - 1;             /* the panel itself covers this */
                continue;
            }
            d = rr_dist((float)ix + 0.5f, (float)iy + 0.5f, cx, cy, hw, hh, r);
            if (d <= 0.0f || d >= sp) continue;
            t = 1.0f - d / sp;
            blend_px(ix, iy, col, t * t);
        }
    }
}

/* Filled triangle from three vertices, antialiased by distance to each edge.
 *
 * Explicit vertices rather than a rotated half-plane test: the rotated version
 * drew the left arrow pointing right, which is the kind of bug that survives a
 * code read and only shows up in a picture. */
static void draw_tri3(float ax, float ay, float bx, float by,
                      float cx, float cy, uint32_t col) {
    float ex[3], ey[3], vx[3], vy[3], inv[3], area;
    int i, x0, y0, x1, y1, ix, iy;

    vx[0] = ax; vy[0] = ay; vx[1] = bx; vy[1] = by; vx[2] = cx; vy[2] = cy;
    area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    if (area == 0.0f) return;

    for (i = 0; i < 3; i++) {
        int j = (i + 1) % 3;
        float dx = vx[j] - vx[i], dy = vy[j] - vy[i];
        float len = (float)sqrt((double)(dx * dx + dy * dy));
        if (len <= 0.0f) return;
        /* Outward normal, flipped so "positive" is always inside. */
        ex[i] = (area > 0.0f ? dy : -dy) / len;
        ey[i] = (area > 0.0f ? -dx : dx) / len;
        inv[i] = ex[i] * vx[i] + ey[i] * vy[i];
    }

    x0 = (int)((vx[0] < vx[1] ? (vx[0] < vx[2] ? vx[0] : vx[2])
                              : (vx[1] < vx[2] ? vx[1] : vx[2])) - 1.0f);
    x1 = (int)((vx[0] > vx[1] ? (vx[0] > vx[2] ? vx[0] : vx[2])
                              : (vx[1] > vx[2] ? vx[1] : vx[2])) + 2.0f);
    y0 = (int)((vy[0] < vy[1] ? (vy[0] < vy[2] ? vy[0] : vy[2])
                              : (vy[1] < vy[2] ? vy[1] : vy[2])) - 1.0f);
    y1 = (int)((vy[0] > vy[1] ? (vy[0] > vy[2] ? vy[0] : vy[2])
                              : (vy[1] > vy[2] ? vy[1] : vy[2])) + 2.0f);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > s_lw) x1 = s_lw;
    if (y1 > s_lh) y1 = s_lh;
    if (x1 <= x0 || y1 <= y0) return;
    mark_drawn(x0, y0, x1 - x0, y1 - y0);

    for (iy = y0; iy < y1; iy++) {
        for (ix = x0; ix < x1; ix++) {
            float px = (float)ix + 0.5f, py = (float)iy + 0.5f;
            float d = 1e30f, cov;
            for (i = 0; i < 3; i++) {
                float di = inv[i] - (ex[i] * px + ey[i] * py);
                if (di < d) d = di;
            }
            cov = d + 0.5f;
            if (cov <= 0.0f) continue;
            blend_px(ix, iy, col, cov);
        }
    }
}

/* Small solid caret. `dir`: -1 left, 1 right, 0 down. */
static void draw_caret(int cx, int cy, int size, int dir, uint32_t col) {
    float s = (float)size, x = (float)cx, y = (float)cy;
    if (s < 1.0f) return;
    if (dir < 0)
        draw_tri3(x + s * 0.7f, y - s, x + s * 0.7f, y + s, x - s * 0.8f, y, col);
    else if (dir > 0)
        draw_tri3(x - s * 0.7f, y - s, x - s * 0.7f, y + s, x + s * 0.8f, y, col);
    else
        draw_tri3(x - s, y - s * 0.7f, x + s, y - s * 0.7f, x, y + s * 0.8f, col);
}

/* Text. Baseline-positioned; every caller derives the baseline from a band's
 * top edge plus the face's ascent, so rows of different sizes still sit on a
 * common grid. */
static int draw_text(int x, int baseline, const char *s, uint32_t col,
                     const PsxUiFace *f) {
    int end;
    if (!s || !f) return x;
    end = psx_ui_font_draw(s_canvas, s_lw, s_lh, x, baseline, s, col, f);
    mark_drawn(x, baseline - psx_ui_font_ascent(f) - 1,
               end - x + 2, psx_ui_font_line_height(f) + 2);
    return end;
}

/* Text that must not run past `max_w`, ending in an ellipsis when it would.
 * Rows are registered by titles with labels of arbitrary length, so "it fits
 * because the panel measured it" holds only until the panel hits the canvas
 * width -- at which point something has to give, and a clipped half-glyph is
 * the worst of the options. */
static void draw_text_clip(int x, int baseline, const char *s, uint32_t col,
                           const PsxUiFace *f, int max_w) {
    char buf[128];
    int n, ell;
    if (!s || !f || max_w <= 0) return;
    if (text_w(s, f) <= max_w) { draw_text(x, baseline, s, col, f); return; }
    ell = text_w("...", f);
    for (n = 0; s[n] && n < (int)sizeof buf - 4; n++) {
        buf[n] = s[n];
        buf[n + 1] = '\0';
        if (text_w(buf, f) + ell > max_w) { buf[n] = '\0'; break; }
    }
    /* Never emit a dangling UTF-8 lead byte: the decoder would read past the
     * terminator looking for continuation bytes. */
    while (n > 0 && ((unsigned char)buf[n - 1] & 0xC0) == 0x80) buf[--n] = '\0';
    if (n > 0 && ((unsigned char)buf[n - 1] & 0x80)) buf[n - 1] = '\0';
    n = (int)strlen(buf);
    buf[n] = '.'; buf[n + 1] = '.'; buf[n + 2] = '.'; buf[n + 3] = '\0';
    draw_text(x, baseline, buf, col, f);
}

/* Vertically centre a face's ink inside a band of height h. Uses ascent and
 * descent rather than the line height, because a font's line box carries
 * leading that is not part of the glyphs and would push short text low. */
static int baseline_in(int y, int h, const PsxUiFace *f) {
    int a = psx_ui_font_ascent(f), d = psx_ui_font_descent(f);
    return y + (h - (a + d)) / 2 + a;
}

static void draw_slider(int m, int row, int sel) {
    int sx, sy, sw, sh, lo = 0, hi = 0, knob, tx0, span, cx;
    slider_rect(m, row, &sx, &sy, &sw, &sh);
    num_range(m, row, &lo, &hi);
    knob = S(VM_SLIDER_KNOB);
    slider_travel(sx, sw, &tx0, &span);
    cx = (hi > lo) ? tx0 + ((num_get(m, row) - lo) * span) / (hi - lo) : tx0;

    round_rect(sx, sy, sw, sh, (float)sh * 0.5f, COL_TRACK);
    if (cx > sx)
        round_rect(sx, sy, cx - sx, sh, (float)sh * 0.5f,
                   sel ? COL_ACCENT : COL_TEXT);

    /* Notches, one per selectable value, on short ranges only.
     *
     * A bare track says "anywhere along here", which is true for a 0..100
     * volume and a lie for a 1..8 zoom, where every position between the steps
     * is unreachable. The ticks show how many stops there are and where the
     * pointer will actually land. Skipped above VM_SLIDER_TICK_MAX steps, where
     * they would be closer together than the pixels drawing them. */
    if (hi > lo && (hi - lo) <= VM_SLIDER_TICK_MAX) {
        int t, tw = S(1.0f) < 1 ? 1 : S(1.0f);
        int ty = sy + sh + S(1.5f), th = S(2.5f) < 2 ? 2 : S(2.5f);
        for (t = lo; t <= hi; t++) {
            int tx = tx0 + ((t - lo) * span) / (hi - lo);
            /* UNDER the groove, not through it. Drawn across the track the
             * ticks washed out against the fill on one side and fought the
             * knob on the other, which made a 1..8 row read as about six
             * notches -- and counting the stops is the entire point. */
            fill_rect(tx, ty, tw, th, COL_TICK);
        }
    }
    {
        const int mark = slider_mark(m, row);
        if (mark >= lo && mark <= hi && hi > lo) {
            int mx = tx0 + ((mark - lo) * span) / (hi - lo);
            int tw = S(1.0f) < 1 ? 1 : S(1.0f);
            /* Taller than the ordinary ticks so the stock value stays findable
             * by eye after a drag. */
            fill_rect(mx, sy + sh + S(1.0f), tw, S(4.5f), COL_ACCENT);
        }
    }
    /* The knob. What makes the track look draggable rather than like a meter. */
    round_rect(cx - knob, sy + sh / 2 - knob, knob * 2, knob * 2,
               (float)knob, sel ? COL_ACCENT : COL_TEXT);
}

static void redraw(void) {
    int i, rows, px, pw, ph, bar_h;
    const PsxUiFace *ft, *fh;
    int clear_x = s_dirty_x, clear_y = s_dirty_y;
    int clear_w = s_dirty_w, clear_h = s_dirty_h;

    /* Clear only what the previous pass drew. A full memset of a 4K canvas is
     * 33 MB per redraw, and redraw runs on every pointer move during a slider
     * drag -- that is milliseconds of frame budget spent zeroing pixels that
     * were already zero. */
    if (clear_w > 0) {
        int y;
        if (clear_x < 0) { clear_w += clear_x; clear_x = 0; }
        if (clear_y < 0) { clear_h += clear_y; clear_y = 0; }
        if (clear_x + clear_w > s_lw) clear_w = s_lw - clear_x;
        if (clear_y + clear_h > s_lh) clear_h = s_lh - clear_y;
        for (y = clear_y; y < clear_y + clear_h; y++)
            memset(&s_canvas[(size_t)y * s_lw + clear_x], 0,
                   (size_t)clear_w * sizeof(uint32_t));
    }
    s_dirty_x = s_dirty_y = s_dirty_w = s_dirty_h = 0;

    if (!s_visible) goto done;

    ft    = face_title();
    fh    = face_hint();
    bar_h = S(VM_BAR_H);

    fill_rect(0, 0, s_lw, bar_h, COL_BAR);
    fill_rect(0, bar_h - 1, s_lw, 1, COL_BAR_EDGE);

    for (i = 0; i < s_menu_total; i++) {
        int tx = title_x(i);
        int tw = title_w(i);
        int chip_y = S(2.0f), chip_h = bar_h - S(4.0f);
        /* A title is only "active" while its dropdown is actually open. With
         * the bar collapsed nothing is selected, so the last-used menu must not
         * stay highlighted — it would read as an open menu that isn't. */
        int active = (s_expanded && i == s_menu);
        if (active)
            round_rect(tx, chip_y, tw, chip_h, VM_TITLE_R * s_unit, COL_SEL_BG);
        else if (i == s_hover_menu)
            round_rect(tx, chip_y, tw, chip_h, VM_TITLE_R * s_unit, COL_HOVER_BG);
        {
            const uint32_t col = active ? COL_ACCENT : COL_TEXT;
            int lx = tx + S(VM_TITLE_PAD);
            const PsxUiFace *fi = face_icon();
            int slot = icon_slot_w();
            if (fi && slot > 0) {
                char buf[5];
                /* Material Symbols sit on the baseline with the box reaching
                 * from there up to about one em, so centring the ICON means
                 * putting its baseline below the text's -- deriving it from
                 * the face's own ascent keeps that true at every size. */
                int ia = psx_ui_font_ascent(fi);
                draw_text(lx, bar_h / 2 + ia / 2,
                          psx_ui_font_utf8(menu_icon(i), buf), col, fi);
                lx += slot;
            }
            draw_text(lx, baseline_in(0, bar_h, ft), menu_title(i), col, ft);
        }
    }
    /* Collapsed: the bar alone, sitting over the game without taking input.
     * Only F10 / VIEW > MENU BAR removes it. */
    if (!s_expanded) goto done;

    rows = menu_rows(s_menu);
    panel_rect(s_menu, &px, &pw, &ph);
    round_rect_shadow(px, bar_h + S(VM_PANEL_GAP), pw, ph, VM_PANEL_R * s_unit,
                      COL_SHADOW, S(VM_SHADOW));
    round_rect(px, bar_h + S(VM_PANEL_GAP), pw, ph, VM_PANEL_R * s_unit,
               COL_PANEL);
    round_rect_line(px, bar_h + S(VM_PANEL_GAP), pw, ph, VM_PANEL_R * s_unit,
                    COL_PANEL_ED, 1.0f);

    /* The hairline between the rows this module owns and the rows a title
     * registered. Structure the player can see without reading: the top group
     * is the emulator's, the bottom group is this game's. */
    if (has_sep(s_menu)) {
        int sy = row_y(s_menu, builtin_rows(s_menu)) - S(VM_SEP_H) / 2;
        fill_rect(px + S(VM_PANEL_PAD) + S(VM_ROW_PAD_X), sy,
                  pw - (S(VM_PANEL_PAD) + S(VM_ROW_PAD_X)) * 2, 1, COL_SEP);
    }

    for (i = 0; i < rows; i++) {
        int sel = (i == s_item[s_menu]);
        int hov = (s_hover_row == i && s_hover_menu == s_menu);
        int ry  = row_y(s_menu, i);
        int rh  = S(VM_ROW_H);
        int inset = px + S(VM_PANEL_PAD);
        int iw    = pw - S(VM_PANEL_PAD) * 2;
        int tx    = inset + S(VM_ROW_PAD_X);
        const PsxUiFace *fr = face_row(sel);
        int base = baseline_in(ry, rh, fr);
        int editing_here = (s_editing && i == s_item[s_menu]);
        int label_max = iw - S(VM_ROW_PAD_X) * 2;

        if (sel)
            round_rect(inset, ry, iw, rh, VM_ROW_R * s_unit, COL_SEL_BG);
        else if (hov)
            round_rect(inset, ry, iw, rh, VM_ROW_R * s_unit, COL_HOVER_BG);

        {
            /* Draw whatever the row reports as its value — this must cover
             * IT_NUMBER too, not just IT_OPTION, or a typed field renders
             * blank and the player edits it blind. */
            /* COPY it: slider_rect -> panel_rect measures every row, which
             * calls row_value again. Holding the returned pointer across that
             * is what made every row print the last row's number. */
            char vbuf[32];
            const char *vsrc = row_value(s_menu, i);
            const char *v = NULL;
            if (vsrc && *vsrc) {
                int c = 0;
                while (vsrc[c] && c < (int)sizeof(vbuf) - 1) { vbuf[c] = vsrc[c]; c++; }
                vbuf[c] = '\0';
                v = vbuf;
            }
            if (row_is_slider(s_menu, i) && !editing_here) {
                int sx, sy, sw, sh;
                slider_rect(s_menu, i, &sx, &sy, &sw, &sh);
                draw_slider(s_menu, i, sel);
                label_max = sx - tx - S(VM_VALUE_GAP);
                if (v)
                    draw_text(sx + sw + S(VM_ROW_PAD_X), base, v,
                              sel ? COL_ACCENT : COL_DIM, fr);
            } else if (v && editing_here) {
                /* Active field: a boxed, left-aligned entry so the caret and
                 * the digits are unmistakable against the row highlight. */
                int bw = text_w("00000", fr) + S(VM_ROW_PAD_X) * 2;
                int bx = inset + iw - bw - S(VM_ROW_PAD_X);
                int bh = rh - S(4.0f);
                round_rect(bx, ry + S(2.0f), bw, bh, VM_ROW_R * s_unit * 0.6f,
                           COL_EDIT_BG);
                round_rect_line(bx, ry + S(2.0f), bw, bh,
                                VM_ROW_R * s_unit * 0.6f, COL_ACCENT, 1.0f);
                draw_text(bx + S(VM_ROW_PAD_X) / 2, base, v, COL_ACCENT, fr);
                label_max = bx - tx - S(VM_VALUE_GAP);
            } else if (v) {
                int vw = text_w(v, fr);
                int vx = inset + iw - S(VM_ROW_PAD_X) - vw;
                /* Arrows on the SELECTED option row only. They say that left
                 * and right change this row -- the one thing the old menu
                 * never told anyone, and the reason players cycled options by
                 * clicking them repeatedly. Suppressed on rows that do not
                 * cycle (actions, and anything with a single choice), where
                 * they would be a promise the row cannot keep. */
                if (sel && row_kind(s_menu, i) == IT_OPTION &&
                    row_choices(s_menu, i) > 1) {
                    int a = S(VM_CARET), gap = S(VM_VALUE_GAP) / 2;
                    int ay = ry + rh / 2;
                    int rx = inset + iw - S(VM_ROW_PAD_X) - a;
                    vx = rx - a - gap - vw;
                    draw_caret(rx, ay, a, 1, COL_ACCENT);
                    draw_caret(vx - gap - a, ay, a, -1, COL_ACCENT);
                }
                draw_text(vx, base, v, sel ? COL_ACCENT : COL_DIM, fr);
                label_max = vx - tx - S(VM_VALUE_GAP);
            }
            draw_text_clip(tx, base, row_label(s_menu, i),
                           sel ? COL_ACCENT : COL_TEXT, fr, label_max);
        }
    }

    /* Hint band, ruled off from the rows the way the reference pins its
     * secondary items to the bottom of the panel. Follows the POINTER when it
     * is over a row, and the keyboard selection otherwise — so you can read
     * what an option does just by hovering it, without committing to selecting
     * it. */
    {
        const int hint_row = (s_hover_row >= 0 && s_hover_menu == s_menu)
                                 ? s_hover_row : s_item[s_menu];
        int hy = rows_y1(s_menu) + S(VM_HINT_GAP);
        int hx = px + S(VM_PANEL_PAD) + S(VM_ROW_PAD_X);
        fill_rect(hx, hy, pw - (S(VM_PANEL_PAD) + S(VM_ROW_PAD_X)) * 2, 1,
                  COL_SEP);
        draw_text_clip(hx, baseline_in(hy, S(VM_HINT_H), fh),
                       row_hint(s_menu, hint_row), COL_DIM, fh,
                       pw - (S(VM_PANEL_PAD) + S(VM_ROW_PAD_X)) * 2);
    }

done:
    /* Exactly what was drawn, from the bounding box the draw calls just built.
     * Derived rather than computed from the layout a second time: a hand-rolled
     * "bar plus panel plus shadow" sum is one more thing to keep in step with
     * the drawing, and it would be wrong the first time anything moved.
     *
     * EVERY exit comes through here. The two early returns above used to skip
     * it, which left a collapsed bar reporting a one-pixel-tall canvas -- the
     * bar simply was not on screen, and with it goes the only way to reach the
     * menu that undoes a bad setting. */
    s_used_h = s_dirty_y + s_dirty_h;
    if (s_used_h > s_lh) s_used_h = s_lh;
    if (s_used_h < 1) s_used_h = 1;
    s_dirty = 0;
}

/* ---- public API ---------------------------------------------------------- */

void psx_video_menu_init(const PsxVideoMenuState *initial) {
    if (initial) s_state = *initial;
    /* Take the canvas HERE, on the main thread before the window exists,
     * rather than lazily on first draw. psx_video_menu_ui_scale is a pure
     * function the renderers call from two threads and it has to agree with
     * what was allocated; deciding that once, up front, is the only ordering
     * where it cannot disagree with itself. */
    canvas_alloc();
    s_unit = unit_for(s_lh);
    /* Bar VISIBLE on launch so it is discoverable — a hotkey nobody knows about
     * may as well not exist — but COLLAPSED, so a dropdown is not sitting over
     * the game while it boots. Clicking a title (or F10 after hiding) expands. */
    s_visible = 1;
    s_expanded = 0;
    s_menu = MENU_VIDEO;
    for (int i = 0; i < VM_MENU_MAX; i++) s_item[i] = 0;
    s_changed = 0;
    s_quit = 0;
    s_savestate = 0;
    s_rewind = 0;
    s_pick_disc = 0;
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
    /* Clamp to what was actually ALLOCATED, not to VM_MAX_*: if the big canvas
     * could not be had, the fallback is smaller and writing past it is the one
     * bug in this module that would be a crash rather than a cosmetic fault.
     * psx_video_menu_ui_scale reads the same caps, so the renderer has already
     * raised its magnification to match and this clamp never bites. */
    if (logical_w > s_cap_w) logical_w = s_cap_w;
    if (logical_h > s_cap_h) logical_h = s_cap_h;
    if (ui_scale < 1) ui_scale = 1;
    if (s_lw == logical_w && s_lh == logical_h && s_ui == ui_scale) return;
    s_lw = logical_w;
    s_lh = logical_h;
    s_ui = ui_scale;
    s_unit = unit_for(logical_h);
    /* Every baked face is at the old pixel sizes. Dropping them here rather
     * than letting the cache fill and evict keeps a window drag from carrying
     * eight stale atlases around. */
    psx_ui_font_reset();
    /* Nothing on the canvas belongs to the new size, and the old dirty box is
     * measured in the old one -- clear the whole thing once and start over. */
    memset(s_canvas, 0, (size_t)s_lw * (size_t)s_lh * sizeof(uint32_t));
    s_dirty_x = s_dirty_y = s_dirty_w = s_dirty_h = 0;
    s_dirty = 1;
}

int psx_video_menu_is_open(void)    { return s_expanded; }
int psx_video_menu_is_visible(void) { return s_visible; }

void psx_video_menu_debug_snapshot(PsxVideoMenuDebug *out) {
    if (!out) return;
    out->visible    = s_visible;
    out->expanded   = s_expanded;
    out->menu       = s_menu;
    out->item       = (s_menu >= 0 && s_menu < s_menu_total) ? s_item[s_menu] : -1;
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
    out->speed_governor = s_state.speed_governor ? 1 : 0;
    out->fast_loads    = s_state.fast_loads;
    out->speed         = s_state.speed;
    out->supersampling = s_state.supersampling;
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

/* Window pixels -> canvas pixels. */
static void to_logical(int wx, int wy, int *lx, int *ly) {
    int s = (s_ui > 0) ? s_ui : 1;
    *lx = wx / s;
    *ly = wy / s;
}

/* Which top-level title is at canvas (x,y)? -1 for none. */
static int hit_title(int x, int y) {
    int i;
    if (y < 0 || y >= S(VM_BAR_H)) return -1;
    for (i = 0; i < s_menu_total; i++) {
        int tx = title_x(i);
        if (x >= tx && x < tx + title_w(i)) return i;
    }
    return -1;
}

/* Which row of the OPEN menu is at canvas (x,y)? -1 for none. Rows are found
 * through row_y(), the same function that draws them, so a highlight can never
 * sit on a different row from the one a click lands on. */
static int hit_row(int x, int y) {
    int px, pw, ph, rows, i;
    panel_rect(s_menu, &px, &pw, &ph);
    if (x < px || x >= px + pw) return -1;
    rows = menu_rows(s_menu);
    for (i = 0; i < rows; i++) {
        int ry = row_y(s_menu, i);
        if (y >= ry && y < ry + S(VM_ROW_H)) return i;
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
            /* VIEW's builtin row 0 is MENU BAR, which hides. Testing the MENU
             * instead of the ROW here made every registered action row under
             * VIEW hide the bar rather than fire its callback. */
            else if (s_menu == MENU_VIEW && !row_reg(s_menu, r))
                psx_video_menu_hide();
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
                } else if (s_menu == MENU_FILE && r == ACT_DISC) {
                    s_pick_disc = 1;
                } else {
                    VmRegRow *rr = row_reg(s_menu, r);
                    if (rr && rr->on_activate) rr->on_activate();
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
            s_menu = (s_menu + s_menu_total - 1) % s_menu_total;
            s_dirty = 1;
            return 1;
        case SDLK_RIGHT:
            s_menu = (s_menu + 1) % s_menu_total;
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
                else if (s_menu == MENU_VIEW &&
                         !row_reg(s_menu, s_item[s_menu]))
                    psx_video_menu_hide();
                else {
                    /* See the mouse path: raise the one-shot, then collapse so
                     * the save-state overlay's pause loop owns the keyboard. */
                    if (s_menu == MENU_GAME) {
                        if (s_item[s_menu] == 3) s_rewind = 1;
                        else                     s_savestate = 1;
                    } else if (s_menu == MENU_FILE &&
                               s_item[s_menu] == ACT_DISC) {
                        s_pick_disc = 1;
                    } else {
                        VmRegRow *rr = row_reg(s_menu, s_item[s_menu]);
                        if (rr && rr->on_activate) rr->on_activate();
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

int psx_video_menu_add_menu(const char *title) {
    if (!title || s_menu_total >= VM_MENU_MAX) return -1;
    const int id = s_menu_total++;
    s_menu_extra_title[id] = title;
    s_dirty = 1;
    return id;
}

static int vm_add(int menu, int kind, const char *label, const char *hint) {
    if (s_reg_count >= VM_REG_MAX) return -1;
    if (menu < 0 || menu >= s_menu_total) return -1;
    VmRegRow *r = &s_reg[s_reg_count];
    memset(r, 0, sizeof(*r));
    r->mark = -1;
    r->menu = menu;
    r->kind = kind;
    r->label = label;
    r->hint = hint;
    s_dirty = 1;
    return s_reg_count++;
}

int psx_video_menu_add_option(int menu, const char *label, const char *hint,
                              const char *const *choices, int choice_count,
                              const char *settings_key, int initial,
                              void (*on_change)(int)) {
    const int h = vm_add(menu, PSX_VM_ROW_OPTION, label, hint);
    if (h < 0) return -1;
    VmRegRow *r = &s_reg[h];
    r->choices = choices;
    r->choice_count = choice_count;
    r->key = settings_key;
    r->value = (initial >= 0 && initial < choice_count) ? initial : 0;
    r->on_change = on_change;
    return h;
}

int psx_video_menu_add_number(int menu, const char *label, const char *hint,
                              int lo, int hi, int slider,
                              const char *settings_key, int initial,
                              void (*on_change)(int)) {
    const int h = vm_add(menu, PSX_VM_ROW_NUMBER, label, hint);
    if (h < 0) return -1;
    VmRegRow *r = &s_reg[h];
    r->lo = lo; r->hi = hi;
    r->slider = slider;
    r->key = settings_key;
    r->value = (initial < lo) ? lo : (initial > hi) ? hi : initial;
    r->on_change = on_change;
    return h;
}

int psx_video_menu_add_action(int menu, const char *label, const char *hint,
                              void (*on_activate)(void)) {
    const int h = vm_add(menu, PSX_VM_ROW_ACTION, label, hint);
    if (h < 0) return -1;
    s_reg[h].on_activate = on_activate;
    return h;
}

void psx_video_menu_set_row_hints(int row_handle, const char *const *hints) {
    if (row_handle < 0 || row_handle >= s_reg_count) return;
    s_reg[row_handle].choice_hints = hints;
    s_dirty = 1;
}

void psx_video_menu_set_row_mark(int row_handle, int value) {
    if (row_handle < 0 || row_handle >= s_reg_count) return;
    s_reg[row_handle].mark = value;
    s_dirty = 1;
}

int psx_video_menu_get_row(int row_handle) {
    if (row_handle < 0 || row_handle >= s_reg_count) return 0;
    return s_reg[row_handle].value;
}

void psx_video_menu_set_row(int row_handle, int value) {
    if (row_handle < 0 || row_handle >= s_reg_count) return;
    VmRegRow *r = &s_reg[row_handle];
    if (r->kind == PSX_VM_ROW_NUMBER) {
        if (value < r->lo) value = r->lo;
        if (value > r->hi) value = r->hi;
    } else if (r->kind == PSX_VM_ROW_OPTION) {
        if (value < 0 || value >= r->choice_count) return;
    } else {
        return;
    }
    if (r->value == value) return;
    r->value = value;
    if (r->on_change) r->on_change(value);
    s_dirty = 1;
}

int psx_video_menu_take_pick_disc(void) {
    int d = s_pick_disc;
    s_pick_disc = 0;
    return d;
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
        else if (!strcmp(key, "windowed_scale")) {
            if (v >= PSX_VM_WINDOWED_SCALE_MIN && v <= PSX_VM_WINDOWED_SCALE_MAX)
                out->windowed_scale = v;
        }
        else if (!strcmp(key, "supersampling")) {
            if (v >= 1 && v <= PSX_VM_SUPERSAMPLING_MAX) out->supersampling = v;
        }
        else if (!strcmp(key, "speed")) {
            if (v >= 1 && v <= PSX_VM_SPEED_MAX) out->speed = v;
        }
        else if (!strcmp(key, "fast_loads")) out->fast_loads = (v >= 0 && v <= 2) ? v : 0;
        else if (!strcmp(key, "vol_master")) out->vol_master = (v >= 0 && v <= 100) ? v : 100;
        else if (!strcmp(key, "speed_governor")) out->speed_governor = v ? 1 : 0;
        else if (!strcmp(key, "vol_music"))  out->vol_music  = (v >= 0 && v <= 100) ? v : 100;
        else if (!strcmp(key, "vol_sound"))  out->vol_sound  = (v >= 0 && v <= 100) ? v : 100;
        else if (!strcmp(key, "update_check")) out->update_check = v ? 1 : 0;
        /* Absent key leaves whatever the caller seeded (game.toml). Only a
         * value actually present here overrides it. */
        else if (!strcmp(key, "renderer"))
            out->renderer = (v >= PSX_VM_RENDERER_SOFTWARE &&
                             v <= PSX_VM_RENDERER_VULKAN)
                            ? v : PSX_VM_RENDERER_UNSET;
        else {
            /* Rows a title registered. Clamped to the row's own bounds so a
             * hand-edited file cannot push a value past what the row accepts. */
            int i;
            for (i = 0; i < s_reg_count; i++) {
                VmRegRow *r = &s_reg[i];
                if (!r->key || strcmp(key, r->key)) continue;
                if (r->kind == PSX_VM_ROW_NUMBER)
                    r->value = (v < r->lo) ? r->lo : (v > r->hi) ? r->hi : v;
                else if (r->kind == PSX_VM_ROW_OPTION)
                    r->value = (v >= 0 && v < r->choice_count) ? v : 0;
                r->restored = 1;
                break;
            }
        }
    }
    fclose(f);
    return 1;
}

/* Hand every restored value to the row that owns it.
 *
 * Loading a setting only put the number back in the MENU. The module
 * behind the row hears about a value through its change callback, and that
 * fires on a change -- which a restore is not. So a stored choice showed
 * correctly in the menu and did nothing in the game until the player nudged
 * the row, at which point it finally took. Reported as: started with CARD
 * DROPS reading 99, won one card, and only after lowering and raising it
 * again did 99 actually apply.
 *
 * The builtin rows never had this problem because main.cpp seeded each of
 * them by hand after the settings file was read. Rows that moved to the
 * registration API lost that seeding and nothing replaced it.
 *
 * Deliberately NOT called from the loader: settings are read long before
 * the guest exists, and these callbacks touch it -- LIFE POINTS patches a
 * code word. The runtime calls this once the game is up, alongside the
 * title start hooks.
 *
 * Only rows that actually came from the file are applied. A row sitting at
 * its registered default is already what the module believes, so firing its
 * callback would be a behaviour change nobody asked for. */
/* Set only while the loop below is replaying stored values, so a row's
 * on_change can tell "the file said so at startup" from "the player just
 * moved me". Without it a callback cannot distinguish the two, and any row
 * that announces itself announces itself on every launch. */
static int s_applying_restored;

int psx_video_menu_is_restoring(void) { return s_applying_restored; }

void psx_video_menu_apply_restored(void) {
    int i;
    s_applying_restored = 1;
    for (i = 0; i < s_reg_count; i++) {
        VmRegRow *r = &s_reg[i];
        if (!r->restored || !r->on_change) continue;
        r->on_change(r->value);
    }
    s_applying_restored = 0;
}

/* The renderer line, written separately from the block below.
 *
 * The menu rewrites this whole file every time a row changes, so a key it does
 * not own has to be round-tripped deliberately or the next menu tweak silently
 * reverts it. It is emitted COMMENTED when unset: that documents the option
 * for anyone reading the file without making the value an active override,
 * which is what writing a real default here would do -- it would override
 * game.toml on every title, not just one that asked for it. */
static void write_renderer_line(FILE *f, int renderer) {
    if (renderer >= PSX_VM_RENDERER_SOFTWARE &&
        renderer <= PSX_VM_RENDERER_VULKAN)
        fprintf(f, "renderer=%d        # 0 software, 1 opengl, 2 vulkan"
                   " (see the note below)\n", renderer);
    else
        fprintf(f, "# renderer=1      # 0 software, 1 opengl, 2 vulkan;"
                   " omit to use the game's own setting\n");
    fprintf(f, "#                  # VULKAN IS EXPERIMENTAL. It composites this\n"
               "#                  # menu and the build's other overlays now,\n"
               "#                  # but OpenGL is the backend the game is\n"
               "#                  # tested on. Takes effect on next launch;\n"
               "#                  # --renderer still wins.\n");
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
        "windowed_scale=%d  # 1..8 window zoom; needs screen=0 and scaling=1\n"
        "vsync=%d           # 0 off (lowest lag, tearing), 1 on, 2 adaptive\n"
        "supersampling=%d   # internal render scale 1..4; applies on next launch\n"
        "speed=%d           # emulation speed multiplier, 1.."
        PSX_VM_STR(PSX_VM_SPEED_MAX) " (1 = normal)\n"
        "fast_loads=%d      # 0 authentic, 1 fast, 2 instant disc loads\n"
        "vol_master=%d      # 0..100 master volume\n"
        "vol_music=%d       # 0..100 music bus (enveloped SPU voices + CD/XA)\n"
        "vol_sound=%d       # 0..100 sound-effect bus (one-shot voices)\n"
        "speed_governor=%d  # 1 = ease SPEED down when audio cannot keep up\n"
        "update_check=%d   # 1 = check GitHub for a newer release on launch\n",
        s_state.scaling ? 1 : 0,
        s_state.filter ? 1 : 0,
        s_state.texture_filter ? 1 : 0,
        (s_state.screen >= 0 && s_state.screen <= 2) ? s_state.screen : 0,
        (s_state.windowed_scale >= PSX_VM_WINDOWED_SCALE_MIN &&
         s_state.windowed_scale <= PSX_VM_WINDOWED_SCALE_MAX)
            ? s_state.windowed_scale : PSX_VM_WINDOWED_SCALE_DEFAULT,
        (s_state.vsync >= 0 && s_state.vsync <= 2) ? s_state.vsync : PSX_VM_VSYNC_ON,
        (s_state.supersampling >= 1 &&
         s_state.supersampling <= PSX_VM_SUPERSAMPLING_MAX) ? s_state.supersampling : 1,
        (s_state.speed >= 1 && s_state.speed <= PSX_VM_SPEED_MAX)
            ? s_state.speed : PSX_VM_SPEED_DEFAULT,
        (s_state.fast_loads >= 0 && s_state.fast_loads <= 2) ? s_state.fast_loads : 0,
        (s_state.vol_master >= 0 && s_state.vol_master <= 100) ? s_state.vol_master : 100,
        (s_state.vol_music  >= 0 && s_state.vol_music  <= 100) ? s_state.vol_music  : 100,
        (s_state.vol_sound  >= 0 && s_state.vol_sound  <= 100) ? s_state.vol_sound  : 100,
        s_state.speed_governor ? 1 : 0,
        s_state.update_check ? 1 : 0);
    write_renderer_line(f, s_state.renderer);
    /* Rows a title registered, in registration order. A row with no key is
     * deliberately not written: a live cheat re-applied at startup would
     * overwrite the player's real save. */
    {
        int i;
        for (i = 0; i < s_reg_count; i++) {
            const VmRegRow *r = &s_reg[i];
            if (!r->key) continue;
            fprintf(f, "%s=%d\n", r->key, r->value);
        }
    }
    fclose(f);
    return 1;
}

/* Bar height in DESIGN UNITS -- a 480-tall screen's worth, which is exactly
 * the coordinate space psx_savestate_menu.c authors its 640x480 panel in, so
 * it can keep asking this how much of its own canvas the bar covers. For a
 * count of real pixels use psx_video_menu_bar_h_px. */
int psx_video_menu_bar_height(void) { return (int)VM_BAR_H; }

int psx_video_menu_ui_scale(int drawable_w, int drawable_h) {
    int mui = 1;
    if (drawable_w <= 0 || drawable_h <= 0) return 1;
    /* 1:1 with the window, so glyphs are rasterised at their real size. The
     * loop only fires past the canvas cap (beyond 4K, or after a failed
     * allocation), where whole-number magnification is what keeps the bar full
     * width and legible instead of covering part of the screen. */
    while (mui < 16 && (drawable_w / mui > s_cap_w || drawable_h / mui > s_cap_h))
        mui++;
    return mui;
}

/* The strip the bar occupies, in drawable pixels, WHETHER OR NOT it is
 * currently visible. Callers that reserve space use this: the letterbox is
 * sized once, so pressing F10 uncovers the space rather than rescaling the
 * picture under the player.
 *
 * Design -> canvas is s_unit, canvas -> screen is the magnification, and this
 * is the only place the two are multiplied. Recomputed from the arguments
 * rather than read off s_unit, because callers ask about window sizes the
 * module has not been laid out for yet (psx_apply_windowed_scale iterates
 * toward one). */
int psx_video_menu_bar_h_px(int drawable_w, int drawable_h) {
    int mui = psx_video_menu_ui_scale(drawable_w, drawable_h);
    float unit = unit_for(drawable_h / mui);
    return (int)(VM_BAR_H * unit + 0.5f) * mui;
}

int psx_video_menu_bar_px(int drawable_w, int drawable_h) {
    if (!s_visible) return 0;
    return psx_video_menu_bar_h_px(drawable_w, drawable_h);
}

int psx_video_menu_needs_present(void) { return s_visible; }

int psx_video_menu_overlay_image(const uint32_t **pixels, int *w, int *h) {
    if (!s_visible) return 0;
    if (s_dirty) { redraw(); s_canvas_ready = 1; }
    if (pixels) *pixels = s_canvas;
    if (w) *w = s_lw;
    if (h) *h = s_used_h;
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
    if (h) *h = s_used_h;
    return 1;
}
