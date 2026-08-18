#ifndef PSX_VIDEO_MENU_H
#define PSX_VIDEO_MENU_H

/* Top menu bar (FILE / VIDEO / GAME) toggled by a hotkey, drawn as an ARGB
 * overlay in the same style as the save-state menu. Owns the presentation
 * options the player can change live, plus the modded gameplay constants.
 *
 * The module is pure UI + state: it never touches SDL or GL itself. The host
 * polls psx_video_menu_take_change() once a frame and applies whatever moved,
 * which keeps the renderer/window plumbing in main.cpp where it already lives. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { PSX_VM_SCALING_FILL = 0, PSX_VM_SCALING_INTEGER = 1 };
enum { PSX_VM_FILTER_NEAREST = 0, PSX_VM_FILTER_LINEAR = 1 };
enum { PSX_VM_SCREEN_WINDOWED = 0, PSX_VM_SCREEN_BORDERLESS = 1,
       PSX_VM_SCREEN_EXCLUSIVE = 2 };
/* Cycle order, deliberately worst-lag first. Mapped to an SDL swap interval by
 * the host: OFF -> 0, ON -> 1, ADAPTIVE -> -1. */
enum { PSX_VM_VSYNC_OFF = 0, PSX_VM_VSYNC_ON = 1, PSX_VM_VSYNC_ADAPTIVE = 2 };
/* Live duel-rank readout.
 *   IN GAME        — the game's own DUEL SKILL badge and rank letter, drawn
 *                    beside the FIELD box and riding its tween. Just the band,
 *                    which is what actually picks the drop table.
 *   IN GAME+SCORE  — the same, plus the 0-99 number in the card-stat font, for
 *                    seeing progress WITHIN a band.
 *   OVERLAY TEXT   — host text at the window corner: always legible, never
 *                    hidden by a card view, unaffected by HUD tracking.
 * Ordered so the two in-game variants sit together in the cycle; a stored 1
 * still means IN GAME, so existing settings files keep their meaning. */
enum { PSX_VM_RANK_OFF = 0, PSX_VM_RANK_INGAME = 1,
       PSX_VM_RANK_INGAME_SCORE = 2, PSX_VM_RANK_TEXT = 3 };

typedef struct PsxVideoMenuState {
    int scaling;         /* PSX_VM_SCALING_*  — present rect snapping */
    int filter;          /* PSX_VM_FILTER_*   — final present filter  */
    int texture_filter;  /* PSX_VM_FILTER_*   — in-game texture filter */
    int screen;          /* PSX_VM_SCREEN_*   */
    /* PSX_VM_VSYNC_* — cycle index, NOT the SDL swap interval. The host maps
     * it to 0 / 1 / -1 so the menu order reads worst-lag -> best-lag. */
    int vsync;
    /* Internal render scale, 1..PSX_VM_SUPERSAMPLING_MAX. Unlike every other
     * row here this one canNOT be applied live: the GL backend fixes its scale
     * when the context comes up (the hi-res texture, scratch texture, depth /
     * stencil renderbuffer, both FBOs and the wide surfaces are all sized from
     * it), and nothing in the renderer can resize that set. The menu therefore
     * persists the choice and it takes effect on the next launch — which the
     * row's hint says out loud, because a control that silently does nothing
     * until restart is worse than no control. */
    int supersampling;
    int life_points;     /* starting duel LP; 8000 is stock */
    int speed;           /* emulation speed multiplier, 1..16 (1 = normal) */
    /* PSX_VM_LOADS_* — how hard to accelerate disc loads. Drives the emulated
     * drive's sector delay ONLY, never host pacing: host pacing speeds the
     * whole machine up while a load is detected, which also speeds the sound
     * driver, and this title's music tempo rides on that. Shortening the
     * sector delay instead leaves the game running at normal speed. */
    int fast_loads;
    /* StarChips is a LIVE save value, not a preference: setting it writes to
     * guest RAM immediately and is never re-applied at startup (that would
     * clobber the player's actual save). Not persisted for the same reason. */
    int starchips;
    int free_spending;   /* 1 = refund any StarChip decrease */
    /* 0 = off, else set EVERY card in the trunk to this many copies. Like
     * starchips this is a write-only cheat applied on change and never
     * restored at startup — re-applying it would clobber a real collection. */
    int all_cards;
    /* Audio buses, 0..100. MASTER scales everything the SPU emits; MUSIC and
     * SOUND are the split buses (see spu_set_bus_gains). 100/100/100 leaves
     * the mix bit-for-bit unchanged. */
    int vol_master;
    int vol_music;
    int vol_sound;
    /* PSX_VM_RANK_* — live duel-rank meter drawn as the OSD status line. */
    int rank_meter;
    /* MODS > CARD DROPS: how many cards a won duel awards, 1..99. 1 is stock.
     * Unlike the CHEATS rows this is a PREFERENCE, not a live save write — it
     * changes what the next duel awards and is persisted like any other
     * setting. The extra cards are rolled by the game's own drop routine, so
     * they come from the same per-opponent, per-rank pool as the first. */
    int card_drops;
} PsxVideoMenuState;

#define PSX_VM_LIFE_POINTS_DEFAULT 8000
#define PSX_VM_SPEED_DEFAULT 1
#define PSX_VM_SPEED_MAX 16
/* Cards awarded per won duel. 1 is stock; the 99 ceiling keeps the value two
 * digits and matches how many New! labels the chest can carry. It is not a
 * game limit — the award routine caps each card at 251 copies, so nothing
 * here can overflow a trunk entry. */
#define PSX_VM_CARD_DROPS_DEFAULT 1
#define PSX_VM_CARD_DROPS_MAX 99
/* Matches the recompiler's [video] supersampling range (config_loader: 1..4). */
#define PSX_VM_SUPERSAMPLING_MAX 4

/* Disc-load acceleration levels. OFF is the authentic 1x drive. FAST divides
 * the sector delay; INSTANT selects cdrom.c's bounded instant scheduler. Both
 * change WHEN the game receives CD interrupts, which is the risk the built-in
 * CD Speed mod warns about — hence a small ladder the player can back down,
 * not a single all-or-nothing switch. */
enum { PSX_VM_LOADS_OFF = 0, PSX_VM_LOADS_FAST = 1, PSX_VM_LOADS_INSTANT = 2 };

/* Seed the menu with the values the runtime booted with. */
void psx_video_menu_init(const PsxVideoMenuState *initial);

/* Settings file (plain key=value) kept beside the executable. _load fills *out
 * with whatever the file specified, leaving untouched fields alone, and returns
 * 1 when the file was read. Call it BEFORE psx_video_menu_init so the stored
 * values become the seed. _save writes the current state; returns 1 on success. */
int  psx_video_menu_settings_load(const char *path, PsxVideoMenuState *out);
int  psx_video_menu_settings_save(const char *path);

/* Reflect a change made outside the menu (e.g. the fullscreen hotkey) so the
 * menu never shows a stale value. Does not raise a change event. */
void psx_video_menu_sync_screen(int screen);
void psx_video_menu_sync_fast_loads(int level);

/* Two independent pieces of state:
 *   VISIBLE  — the bar is drawn. It does NOT take input from the game, so you
 *              can play with it on screen. Only F10 / VIEW > MENU BAR hides it.
 *   EXPANDED — a dropdown is open. THIS captures the keyboard/pad, so the guest
 *              sees an idle pad. Clicking into the game collapses the dropdown
 *              but leaves the bar visible.
 * psx_video_menu_is_open() means "expanded", i.e. input is captured — that is
 * the question every input path needs answered. */
int  psx_video_menu_is_open(void);
int  psx_video_menu_is_visible(void);

void psx_video_menu_toggle(void);    /* show+expand, or hide entirely */
void psx_video_menu_hide(void);      /* bar off */
void psx_video_menu_collapse(void);  /* close the dropdown, keep the bar */
void psx_video_menu_close(void);     /* alias of collapse (legacy callers) */

/* Renderer tells the menu how big its canvas should be, in LOGICAL pixels, and
 * the integer factor it will be magnified by. Drawing at an integer multiple is
 * what keeps the 8x8 glyphs crisp; stretching a fixed canvas to an arbitrary
 * window size makes the text uneven. */
void psx_video_menu_set_layout(int logical_w, int logical_h, int ui_scale);

/* Mouse input, in WINDOW pixels (the module divides by ui_scale itself).
 * _click returns 1 when the click landed on the menu and was consumed. */
void psx_video_menu_mouse_move(int win_x, int win_y);
int  psx_video_menu_mouse_click(int win_x, int win_y);
/* Ends a slider drag. Must be called on mouse-button-up, or a drag started on
 * a volume track would keep following the pointer after the button is let go. */
void psx_video_menu_mouse_release(void);

/* Pointer left the window: cancels the hover-to-open dwell and clears the
 * hover highlight, so a menu never opens itself after the mouse has gone. */
void psx_video_menu_mouse_leave(void);

/* Feed the module a millisecond clock once per frame. Drives hover-to-open:
 * resting on a title opens its dropdown after a short dwell. The module keeps
 * no clock of its own, so without this call hover-to-open simply never fires
 * (everything else still works). */
void psx_video_menu_tick(unsigned int now_ms);

/* Returns 1 when the key was consumed by the menu (host must not forward it
 * to the guest). key is an SDL_Keycode; passed as int to keep SDL out of this
 * header. */
int  psx_video_menu_handle_key(int key);

/* 1 + fills *out when any option changed since the previous call. */
int  psx_video_menu_take_change(PsxVideoMenuState *out);

/* 1 exactly once after the player picks FILE > QUIT. */
int  psx_video_menu_take_quit(void);

/* 1 exactly once after the player picks GAME > SAVE / LOAD STATE. The host
 * opens its save-state slot overlay; this module owns no part of that and only
 * reports the request, the same way it does for QUIT. The dropdown has already
 * collapsed by the time this returns 1, so the overlay is free to take the
 * keyboard and pad without two menus contending for input. */
int  psx_video_menu_take_savestate(void);

/* Bar height in LOGICAL pixels (multiply by the ui scale for drawable px).
 * The renderer reserves this strip at the top so the game is letterboxed
 * BELOW the bar rather than hidden underneath it. */
int  psx_video_menu_bar_height(void);

/* Canonical integer magnification for a given drawable size, and the bar's
 * height in those drawable pixels (0 when hidden). Defined HERE, not in a
 * renderer, so every backend reserves exactly the strip that gets drawn —
 * two copies of this formula would drift the moment either changed. */
int  psx_video_menu_ui_scale(int drawable_w, int drawable_h);
int  psx_video_menu_bar_px(int drawable_w, int drawable_h);

int  psx_video_menu_needs_present(void);

/* Overlay pixels, split by which thread is asking.
 *
 * _overlay_image redraws when dirty and must therefore only be called from the
 * thread that owns this module's state (the emu thread). The frame-
 * interpolation presenter runs on a SECOND thread, so it uses the _ro variant
 * and the emu thread calls _prepare once per frame before handing over the
 * swap. Both are no-ops when the bar is hidden. */
void psx_video_menu_prepare(void);
int  psx_video_menu_overlay_image_ro(const uint32_t **pixels, int *w, int *h);
int  psx_video_menu_overlay_image(const uint32_t **pixels, int *w, int *h);

/* Read-only snapshot of the menu's internal state, for the TCP debug server.
 * Without this the only way to ask "is the dropdown actually open?" is to
 * inspect presented pixels, which cannot tell an open dropdown from a hover
 * highlight and cannot see the state at all when the overlay fails to draw. */
typedef struct PsxVideoMenuDebug {
    int visible;      /* bar drawn */
    int expanded;     /* dropdown open (captures input) */
    int menu;         /* selected top-level menu index */
    int item;         /* selected row within that menu */
    int hover_menu;   /* -1 when the cursor is off the titles */
    int hover_row;
    int editing;      /* inline numeric entry active */
    int dirty;        /* canvas needs re-rendering */
    int logical_w, logical_h, ui_scale;
    int rows;         /* rows in the selected menu */
    int vol_master, vol_music, vol_sound;   /* audio buses, 0..100 */
} PsxVideoMenuDebug;

void psx_video_menu_debug_snapshot(PsxVideoMenuDebug *out);

#ifdef __cplusplus
}
#endif

#endif /* PSX_VIDEO_MENU_H */
