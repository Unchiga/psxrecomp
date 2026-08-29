/* host_osd.c — top-left toast messages for host hotkeys / savestate results.
 *
 * The visual OSD used to be gated on RECOMP_LAUNCHER, which only ever removed
 * the PIXELS: every renderer composites host_osd_image() / consults
 * host_osd_needs_present() unconditionally (see gpu_gl_renderer.c), so a
 * launcher-less build still ran the whole present path and just had nothing to
 * draw. Toasts are host feedback, not launcher UI — a player who plugs in a
 * controller or saves a state needs to be told either way. Built always.
 *
 * TEXT COMES FROM psx_ui_font NOW, not from an 8x8 uppercase sheet magnified
 * by a whole number. That sheet was fine while every host overlay shared it;
 * once the F10 bar above these toasts started drawing antialiased mixed-case
 * Inter, a toast landing directly under it read as a screenshot from a
 * different program. Two consequences:
 *
 *  - Layout is in DESIGN UNITS, one unit being 1/480 of the surface height,
 *    exactly as in psx_video_menu.c. S() converts. The images are rasterised
 *    at the size they will OCCUPY and the renderers blit them 1:1; they used
 *    to be authored small and magnified at composite time, which is the other
 *    half of why this looked like a bitmap.
 *  - The module therefore has to be told how big the surface is, which is what
 *    host_osd_set_layout is for. Every present path calls it before asking for
 *    an image; without it the OSD simply stays at its 480-tall proportions.
 *
 * THE TOAST IS TRANSLUCENT AND THE VOLUME BAR IS NOT, and that is not an
 * oversight. The toast is composited by a blending path in all three backends
 * (GL with blend on, SDL_BLENDMODE_BLEND, and Vulkan's overlay render pass).
 * The volume bar still goes through Vulkan's plain buffer-to-image copy, which
 * writes the bytes it is given with no blend at all — so anything transparent
 * in it, a rounded corner included, would come out as a black notch there and
 * as glass everywhere else. It stays an opaque rectangle until it moves onto
 * the same path as the toast. */

#include "host_osd.h"
#include "psx_rewind.h"
#include "psx_savestate_menu.h"
#include "psx_sdl.h"
#include "psx_ui_draw.h"

#include <stdio.h>
#include <string.h>

#define HOST_OSD_VISUAL 1

#define OSD_MAX_CHARS  64
#define OSD_DEFAULT_MS 2000

/* ---- layout, in DESIGN UNITS (1 unit = 1/480 of surface height) ---------- */

#define OSD_FS_TEXT    10.0f   /* deliberately the menu's row size: same face,
                                * one bake, and a toast that reads as part of
                                * the same UI as the bar it sits under */
#define OSD_PAD_X      11.0f
#define OSD_PAD_Y       6.0f
#define OSD_RADIUS      8.0f
#define OSD_EDGE_W      1.0f

/* Volume bar. Not round numbers because they reproduce what the old fixed
 * 16x120 bitmap actually measured on screen once OSD_SCALE and the renderer's
 * magnification had both been applied — the bar is not being resized here,
 * only authored in the units everything else now uses. */
#define VOL_W          28.0f
#define VOL_H         214.0f
#define VOL_BORDER      2.0f

/* Ceiling on the design-unit factor.
 *
 * Not a taste decision: the images below live in fixed arrays, and the arrays
 * have to hold the largest thing this can ask for. Five covers every display
 * up to and past 4K at true size; beyond that the OSD stops growing with the
 * screen, which is the same point where the menu switches to whole-number
 * magnification and nothing is being drawn at its natural size anyway.
 * Anything that still would not fit is clipped, not overrun. */
#define OSD_UNIT_MAX    5.0f

/* Palette, matching psx_video_menu.c's bar so the two read as one surface. */
#define OSD_COL_BG     0xE8141826u
#define OSD_COL_EDGE   0x40FFFFFFu
#define OSD_COL_TEXT   0xFFC9CFDDu
#define VOL_COL_BG     0xFF141826u   /* opaque: see the file header */
#define VOL_COL_EDGE   0xFF3A4352u
#define VOL_COL_FILL   0xFF7FA6FFu

static char     s_msg[OSD_MAX_CHARS];
static Uint32   s_expire_ms;
static int      s_active;
static int      s_img_dirty = 1;

static char     s_status_msg[OSD_MAX_CHARS];
static int      s_status_active;
static int      s_status_dirty = 1;

static int      s_volume = 100;          /* host master 0..100 */
static int      s_vol_pct = 100;         /* last shown bar fill */
static Uint32   s_vol_expire_ms;
static int      s_vol_active;
static int      s_vol_dirty = 1;

static int      s_needs_clear;

/* Design unit -> pixel, from the surface HEIGHT alone: the OSD should be the
 * same fraction of the screen on a 16:10 panel as on a 21:9 one, and only the
 * height is common to both. */
static float s_unit = 1.0f;

static int S(float design) {
    return (int)(design * s_unit + 0.5f);
}

/* Worst case at OSD_UNIT_MAX: a full OSD_MAX_CHARS message at ~50 px/em, plus
 * padding. Sized generously rather than exactly — being wrong costs a clipped
 * toast, and being generous costs a megabyte. */
#define OSD_IMG_W  1792
#define OSD_IMG_H   160
static uint32_t s_img[OSD_IMG_W * OSD_IMG_H];
static int      s_img_w;
static int      s_img_h;

#define VOL_IMG_W   160
#define VOL_IMG_H  1088
static uint32_t s_vol_img[VOL_IMG_W * VOL_IMG_H];
static int      s_vol_w;
static int      s_vol_h;

#ifndef PSX_SDL_NO_RENDER
static SDL_Texture *s_sdl_tex;
static int          s_sdl_tw;
static int          s_sdl_th;
static SDL_Texture *s_sdl_vol_tex;
static int          s_sdl_vol_tw;
static int          s_sdl_vol_th;
static SDL_Texture *s_sdl_rw_tex;
static int          s_sdl_rw_tw;
static int          s_sdl_rw_th;
static SDL_Texture *s_sdl_ssm_tex;
static int          s_sdl_ssm_tw;
static int          s_sdl_ssm_th;
static SDL_Renderer *s_sdl_ren;
#endif

static int clamp_pct(int p) {
    if (p < 0) return 0;
    if (p > 100) return 100;
    return p;
}

void host_osd_set_layout(int surface_w, int surface_h) {
    float u;
    (void)surface_w;
    if (surface_h < 120) surface_h = 120;
    u = (float)surface_h / 480.0f;
    if (u < 1.0f) u = 1.0f;
    if (u > OSD_UNIT_MAX) u = OSD_UNIT_MAX;
    if (u == s_unit) return;
    s_unit = u;
    /* Every rasterised image is at the old size. Deliberately NOT dropping the
     * stale font faces with psx_ui_font_reset(): the menu does that from its
     * own set_layout, and doing it here would invalidate face pointers the
     * menu is holding across its redraw. */
    s_img_dirty = 1;
    s_status_dirty = 1;
    s_vol_dirty = 1;
}

static int msg_visible(void) {
    if (!s_active) return 0;
    if ((int32_t)(SDL_GetTicks() - s_expire_ms) >= 0) {
        s_active = 0;
        s_msg[0] = '\0';
        s_needs_clear = 1;
        s_img_dirty = 1;
        return 0;
    }
    return 1;
}

static int vol_visible(void) {
    if (!s_vol_active) return 0;
    if ((int32_t)(SDL_GetTicks() - s_vol_expire_ms) >= 0) {
        s_vol_active = 0;
        s_needs_clear = 1;
        s_vol_dirty = 1;
        return 0;
    }
    return 1;
}

/* One rounded pill exactly the size of its text, transparent outside the pill
 * so the game shows through around it.
 *
 * No dirty-box bookkeeping, unlike the menu: this canvas IS the toast. It is
 * re-cut to the message's own width every time the message changes, so there
 * is never anything left from a previous pass to clear. */
static void rasterize_text(const char *msg) {
    PsxUiCanvas c;
    const PsxUiFace *f =
        psx_ui_font_face(OSD_FS_TEXT * s_unit, PSX_UI_FONT_REGULAR);
    const int pad_x = S(OSD_PAD_X), pad_y = S(OSD_PAD_Y);
    const int ink   = f ? psx_ui_font_ascent(f) + psx_ui_font_descent(f)
                        : S(OSD_FS_TEXT);
    const int tw    = f ? psx_ui_font_text_w(f, msg) : 0;
    float edge      = OSD_EDGE_W * s_unit;

    s_img_w = tw + pad_x * 2;
    s_img_h = ink + pad_y * 2;
    if (s_img_w < 1) s_img_w = 1;
    if (s_img_h < 1) s_img_h = 1;
    if (s_img_w > OSD_IMG_W) s_img_w = OSD_IMG_W;
    if (s_img_h > OSD_IMG_H) s_img_h = OSD_IMG_H;

    memset(s_img, 0, (size_t)s_img_w * (size_t)s_img_h * sizeof(uint32_t));

    c.px = s_img;
    c.w  = s_img_w;
    c.h  = s_img_h;
    psx_ui_dirty_reset(&c);

    if (edge < 1.0f) edge = 1.0f;
    psx_ui_round_rect(&c, 0, 0, s_img_w, s_img_h, OSD_RADIUS * s_unit,
                      OSD_COL_BG);
    psx_ui_round_rect_line(&c, 0, 0, s_img_w, s_img_h, OSD_RADIUS * s_unit,
                           OSD_COL_EDGE, edge);
    /* Clipped rather than trusted to fit: s_img_w was just clamped to the
     * buffer, and past that clamp the string is wider than the pill. */
    psx_ui_text_clip(&c, pad_x, psx_ui_baseline_in(0, s_img_h, f), msg,
                     OSD_COL_TEXT, f, s_img_w - pad_x * 2);
    s_img_dirty = 0;
}

static void rasterize_volume(void) {
    PsxUiCanvas c;
    int border = S(VOL_BORDER), inner_w, inner_h, fill_h, y0;

    s_vol_w = S(VOL_W);
    s_vol_h = S(VOL_H);
    if (s_vol_w < 4) s_vol_w = 4;
    if (s_vol_h < 8) s_vol_h = 8;
    if (s_vol_w > VOL_IMG_W) s_vol_w = VOL_IMG_W;
    if (s_vol_h > VOL_IMG_H) s_vol_h = VOL_IMG_H;
    if (border < 1) border = 1;
    if (border * 2 >= s_vol_w) border = 1;

    c.px = s_vol_img;
    c.w  = s_vol_w;
    c.h  = s_vol_h;
    psx_ui_dirty_reset(&c);

    /* Opaque everywhere, corners included — see the file header. */
    psx_ui_fill(&c, 0, 0, s_vol_w, s_vol_h, VOL_COL_BG);
    psx_ui_fill(&c, 0, 0, s_vol_w, border, VOL_COL_EDGE);
    psx_ui_fill(&c, 0, s_vol_h - border, s_vol_w, border, VOL_COL_EDGE);
    psx_ui_fill(&c, 0, 0, border, s_vol_h, VOL_COL_EDGE);
    psx_ui_fill(&c, s_vol_w - border, 0, border, s_vol_h, VOL_COL_EDGE);

    inner_w = s_vol_w - 2 * border;
    inner_h = s_vol_h - 2 * border;
    fill_h  = (inner_h * clamp_pct(s_vol_pct) + 50) / 100;
    if (fill_h < 0) fill_h = 0;
    if (fill_h > inner_h) fill_h = inner_h;
    y0 = border + (inner_h - fill_h);
    psx_ui_fill(&c, border, y0, inner_w, fill_h, VOL_COL_FILL);
    s_vol_dirty = 0;
}

#ifndef PSX_SDL_NO_RENDER
static void sdl_blit_argb(SDL_Renderer *renderer, SDL_Texture **tex,
                          int *tw, int *th, const uint32_t *px, int w, int h,
                          int dst_x, int dst_y, int dst_w, int dst_h) {
    if (!*tex || *tw != w || *th != h || renderer != s_sdl_ren) {
        if (*tex) SDL_DestroyTexture(*tex);
        *tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                 SDL_TEXTUREACCESS_STREAMING, w, h);
        *tw = w;
        *th = h;
        if (*tex) SDL_SetTextureBlendMode(*tex, SDL_BLENDMODE_BLEND);
    }
    if (!*tex) return;
    void *locked = NULL;
    int pitch = 0;
#if defined(PSX_SDL3)
    const int lock_ok = SDL_LockTexture(*tex, NULL, &locked, &pitch);
#else
    const int lock_ok = (SDL_LockTexture(*tex, NULL, &locked, &pitch) == 0);
#endif
    if (lock_ok && locked) {
        uint8_t *dst = (uint8_t *)locked;
        for (int y = 0; y < h; y++)
            memcpy(dst + (size_t)y * (size_t)pitch,
                   px + (size_t)y * (size_t)w,
                   (size_t)w * sizeof(uint32_t));
        SDL_UnlockTexture(*tex);
    }
    if (dst_w < 1) dst_w = w;
    if (dst_h < 1) dst_h = h;
    SDL_Rect dst = { dst_x, dst_y, dst_w, dst_h };
#if defined(PSX_SDL3)
    (void)psx_sdl_render_copy(renderer, *tex, NULL, &dst);
#else
    SDL_RenderCopy(renderer, *tex, NULL, &dst);
#endif
}

/* Present uses SDL_RenderSetLogicalSize — position/size in logical pixels,
 * not window/output pixels (GetRenderOutputSize mixed spaces and broke scale). */
static void sdl_logical_size(SDL_Renderer *renderer, int *lw, int *lh) {
    int w = 0, h = 0;
#if defined(PSX_SDL3)
    {
        SDL_RendererLogicalPresentation mode =
            SDL_LOGICAL_PRESENTATION_DISABLED;
        if (!SDL_GetRenderLogicalPresentation(renderer, &w, &h, &mode) ||
            w <= 0 || h <= 0)
            SDL_GetRenderOutputSize(renderer, &w, &h);
    }
#else
    SDL_RenderGetLogicalSize(renderer, &w, &h);
    if (w <= 0 || h <= 0)
        SDL_GetRendererOutputSize(renderer, &w, &h);
#endif
    if (lw) *lw = w > 0 ? w : 640;
    if (lh) *lh = h > 0 ? h : 480;
}
#endif

void host_osd_push(const char *msg, int duration_ms) {
#if !HOST_OSD_VISUAL
    (void)msg;
    (void)duration_ms;
    return;
#else
    if (!msg || !msg[0]) return;
    if (duration_ms <= 0) duration_ms = OSD_DEFAULT_MS;
    snprintf(s_msg, sizeof(s_msg), "%s", msg);
    s_expire_ms = SDL_GetTicks() + (Uint32)duration_ms;
    s_active = 1;
    s_needs_clear = 0;
    s_img_dirty = 1;
#endif
}

void host_osd_set_status(const char *msg) {
#if !HOST_OSD_VISUAL
    (void)msg;
    return;
#else
    if (!msg || !msg[0]) {
        if (s_status_active) s_needs_clear = 1;
        s_status_msg[0] = '\0';
        s_status_active = 0;
        s_status_dirty = 1;
        return;
    }
    snprintf(s_status_msg, sizeof(s_status_msg), "%s", msg);
    s_status_active = 1;
    s_status_dirty = 1;
    s_needs_clear = 0;
#endif
}

void host_osd_show_volume(int percent, int duration_ms) {
#if !HOST_OSD_VISUAL
    (void)percent;
    (void)duration_ms;
    return;
#else
    if (duration_ms <= 0) duration_ms = OSD_DEFAULT_MS;
    s_vol_pct = clamp_pct(percent);
    s_vol_expire_ms = SDL_GetTicks() + (Uint32)duration_ms;
    s_vol_active = 1;
    s_needs_clear = 0;
    s_vol_dirty = 1;
#endif
}

int host_volume_get(void) {
    return s_volume;
}

void host_volume_set(int percent) {
    s_volume = clamp_pct(percent);
}

int host_volume_adjust(int delta) {
    s_volume = clamp_pct(s_volume + delta);
#if HOST_OSD_VISUAL
    host_osd_show_volume(s_volume, OSD_DEFAULT_MS);
#endif
    return s_volume;
}

int host_osd_needs_present(void) {
    if (psx_rewind_needs_present()) return 1;
    if (psx_savestate_menu_needs_present()) return 1;
#if !HOST_OSD_VISUAL
    return 0;
#else
    if (msg_visible() || s_status_active || vol_visible()) return 1;
    return s_needs_clear;
#endif
}

int host_osd_image(const uint32_t **pixels, int *w, int *h) {
#if !HOST_OSD_VISUAL
    if (pixels) *pixels = NULL;
    if (w) *w = 0;
    if (h) *h = 0;
    return 0;
#else
    const int show_msg = msg_visible();
    if (!show_msg && !s_status_active) {
        if (pixels) *pixels = NULL;
        if (w) *w = 0;
        if (h) *h = 0;
        return 0;
    }
    if (show_msg) {
        if (s_img_dirty) rasterize_text(s_msg);
    } else {
        if (s_status_dirty || s_img_dirty) {
            rasterize_text(s_status_msg);
            s_status_dirty = 0;
        }
    }
    if (pixels) *pixels = s_img;
    if (w) *w = s_img_w;
    if (h) *h = s_img_h;
    return 1;
#endif
}

int host_osd_volume_image(const uint32_t **pixels, int *w, int *h) {
#if !HOST_OSD_VISUAL
    if (pixels) *pixels = NULL;
    if (w) *w = 0;
    if (h) *h = 0;
    return 0;
#else
    if (!vol_visible()) {
        if (pixels) *pixels = NULL;
        if (w) *w = 0;
        if (h) *h = 0;
        return 0;
    }
    if (s_vol_dirty) rasterize_volume();
    if (pixels) *pixels = s_vol_img;
    if (w) *w = s_vol_w;
    if (h) *h = s_vol_h;
    return 1;
#endif
}

void host_osd_present_done(void) {
#if !HOST_OSD_VISUAL
    return;
#else
    if (!s_active && !s_status_active && !s_vol_active) s_needs_clear = 0;
#endif
}

void host_osd_draw_sdl(struct SDL_Renderer *renderer) {
#ifndef PSX_SDL_NO_RENDER
    int lw = 640, lh = 480;
    if (!renderer) return;
    s_sdl_ren = renderer;
    sdl_logical_size(renderer, &lw, &lh);
#if HOST_OSD_VISUAL
    {
        const uint32_t *px;
        int w, h;
        int ui, margin;
        host_osd_set_layout(lw, lh);
        /* Margins still scale off a 480-tall reference; the IMAGES no longer
         * do, because they are rasterised at their final size and blitted 1:1
         * rather than magnified here. */
        ui = lh / 480;
        if (ui < 1) ui = 1;
        if (ui > 8) ui = 8;
        margin = 8 * ui;
        if (host_osd_image(&px, &w, &h) && px)
            sdl_blit_argb(renderer, &s_sdl_tex, &s_sdl_tw, &s_sdl_th, px, w, h,
                          margin, margin, w, h);
        if (host_osd_volume_image(&px, &w, &h) && px) {
            int x = (lw > w + margin) ? (lw - w - margin) : margin;
            int y = (lh > h) ? ((lh - h) / 2) : margin;
            sdl_blit_argb(renderer, &s_sdl_vol_tex, &s_sdl_vol_tw, &s_sdl_vol_th,
                          px, w, h, x, y, w, h);
        }
    }
    host_osd_present_done();
#endif
    {
        const uint32_t *px = NULL;
        int w = 0, h = 0;
        if (psx_rewind_overlay_image(&px, &w, &h) && px) {
            float slide = psx_rewind_slide();
            int dw = lw;
            int dh = (lh * h) / 480;
            int y;
            if (dh < 8) dh = h;
            y = lh - (int)((float)dh * slide + 0.5f);
            sdl_blit_argb(renderer, &s_sdl_rw_tex, &s_sdl_rw_tw, &s_sdl_rw_th,
                          px, w, h, 0, y, dw, dh);
        }
        /* The slot browser authors its canvas at the surface's own size now,
         * so this stretch is 1:1 unless the canvas allocation fell back to the
         * smaller static buffer. */
        psx_savestate_menu_set_layout(lw, lh);
        if (psx_savestate_menu_overlay_image(&px, &w, &h) && px)
            sdl_blit_argb(renderer, &s_sdl_ssm_tex, &s_sdl_ssm_tw,
                          &s_sdl_ssm_th, px, w, h, 0, 0, lw, lh);
    }
#else
    (void)renderer;
#endif /* PSX_SDL_NO_RENDER */
}
