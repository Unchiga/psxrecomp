/* Antialiased shapes and text on a caller-owned ARGB canvas. See psx_ui_draw.h.
 *
 * Lifted, near enough verbatim, from the helpers psx_video_menu.c grew when it
 * moved off the 8x8 bitmap sheet -- the geometry and the fast paths are its,
 * and are commented here for the same reasons they were commented there. What
 * changed is that the canvas arrives as an argument instead of being a file
 * static, which is what let the toast strip and the save-state browser stop
 * carrying their own drawing code. */

#include "psx_ui_draw.h"

#include <math.h>
#include <string.h>

void psx_ui_dirty_reset(PsxUiCanvas *c)
{
    if (!c) return;
    c->dx = c->dy = c->dw = c->dh = 0;
}

void psx_ui_dirty_clear(PsxUiCanvas *c)
{
    int x, y, w, h, iy;
    if (!c || !c->px || c->dw <= 0 || c->dh <= 0) { psx_ui_dirty_reset(c); return; }
    x = c->dx; y = c->dy; w = c->dw; h = c->dh;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > c->w) w = c->w - x;
    if (y + h > c->h) h = c->h - y;
    for (iy = y; iy < y + h && w > 0; iy++)
        memset(&c->px[(size_t)iy * c->w + x], 0, (size_t)w * sizeof(uint32_t));
    psx_ui_dirty_reset(c);
}

void psx_ui_mark(PsxUiCanvas *c, int x, int y, int w, int h)
{
    int x1, y1;
    if (!c || w <= 0 || h <= 0) return;
    if (c->dw <= 0) { c->dx = x; c->dy = y; c->dw = w; c->dh = h; return; }
    x1 = c->dx + c->dw;
    y1 = c->dy + c->dh;
    if (x < c->dx) c->dx = x;
    if (y < c->dy) c->dy = y;
    if (x + w > x1) x1 = x + w;
    if (y + h > y1) y1 = y + h;
    c->dw = x1 - c->dx;
    c->dh = y1 - c->dy;
}

void psx_ui_blend(PsxUiCanvas *c, int x, int y, uint32_t argb, float cov)
{
    uint32_t *p, d;
    unsigned sa, da, oa, i, sc[3], dc[3], oc[3];

    if (!c || !c->px) return;
    if ((unsigned)x >= (unsigned)c->w || (unsigned)y >= (unsigned)c->h) return;
    if (cov <= 0.0f) return;
    if (cov > 1.0f) cov = 1.0f;

    sa = (unsigned)(((argb >> 24) & 0xFFu) * cov + 0.5f);
    if (!sa) return;

    p = &c->px[(size_t)y * c->w + x];
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

void psx_ui_fill(PsxUiCanvas *c, int x0, int y0, int w, int h, uint32_t col)
{
    int x, y;
    if (!c || !c->px) return;
    if (x0 < 0) { w += x0; x0 = 0; }
    if (y0 < 0) { h += y0; y0 = 0; }
    if (x0 + w > c->w) w = c->w - x0;
    if (y0 + h > c->h) h = c->h - y0;
    if (w <= 0 || h <= 0) return;
    psx_ui_mark(c, x0, y0, w, h);
    for (y = y0; y < y0 + h; y++)
        for (x = x0; x < x0 + w; x++)
            psx_ui_blend(c, x, y, col, 1.0f);
}

/* Signed distance from (px,py) to a rounded rect. Negative inside. This is the
 * whole antialiasing story: coverage is 0.5 - d clamped to 0..1, which is the
 * exact area of a pixel covered by a straight edge and close enough on a curve
 * at any radius this UI actually uses. */
static float rr_dist(float px, float py, float cx, float cy,
                     float hw, float hh, float r)
{
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
static float rr_clamp(int w, int h, float r)
{
    float lim = (float)(w < h ? w : h) * 0.5f;
    if (r < 0.0f) r = 0.0f;
    if (r > lim) r = lim;
    return r;
}

/* The per-pixel distance function is only needed in the four corner bands: the
 * rows between them are a solid span, and because x/y/w/h are whole pixels
 * their left and right edges land exactly on pixel boundaries with no partial
 * coverage to compute. Skipping the square root on those rows is what keeps a
 * full-window redraw affordable. */
void psx_ui_round_rect(PsxUiCanvas *c, int x, int y, int w, int h,
                       float r, uint32_t col)
{
    float cx, cy, hw, hh;
    int iy, ix, y0, y1, x0, x1, band0, band1;

    if (!c || !c->px || w <= 0 || h <= 0 || !(col >> 24)) return;
    r = rr_clamp(w, h, r);
    x0 = x < 0 ? 0 : x;
    y0 = y < 0 ? 0 : y;
    x1 = x + w > c->w ? c->w : x + w;
    y1 = y + h > c->h ? c->h : y + h;
    if (x1 <= x0 || y1 <= y0) return;
    psx_ui_mark(c, x0, y0, x1 - x0, y1 - y0);

    cx = (float)x + (float)w * 0.5f;
    cy = (float)y + (float)h * 0.5f;
    hw = (float)w * 0.5f;
    hh = (float)h * 0.5f;
    band0 = y + (int)r + 1;                 /* first fully straight row */
    band1 = y + h - (int)r - 1;             /* last  fully straight row */

    for (iy = y0; iy < y1; iy++) {
        if (iy >= band0 && iy < band1) {
            for (ix = x0; ix < x1; ix++) psx_ui_blend(c, ix, iy, col, 1.0f);
            continue;
        }
        for (ix = x0; ix < x1; ix++) {
            float cov = 0.5f - rr_dist((float)ix + 0.5f, (float)iy + 0.5f,
                                       cx, cy, hw, hh, r);
            if (cov <= 0.0f) continue;
            psx_ui_blend(c, ix, iy, col, cov);
        }
    }
}

/* Only the pixels near an edge can be lit, so the straight middle rows are
 * visited at their two ends and skipped in between. */
void psx_ui_round_rect_line(PsxUiCanvas *c, int x, int y, int w, int h,
                            float r, uint32_t col, float width)
{
    float cx, cy, hw, hh, half;
    int iy, ix, y0, y1, x0, x1, band0, band1, reach;

    if (!c || !c->px || w <= 0 || h <= 0 || !(col >> 24)) return;
    if (width <= 0.0f) width = 1.0f;
    r = rr_clamp(w, h, r);
    half = width * 0.5f;
    reach = (int)half + 2;

    x0 = x - reach < 0 ? 0 : x - reach;
    y0 = y - reach < 0 ? 0 : y - reach;
    x1 = x + w + reach > c->w ? c->w : x + w + reach;
    y1 = y + h + reach > c->h ? c->h : y + h + reach;
    if (x1 <= x0 || y1 <= y0) return;
    psx_ui_mark(c, x0, y0, x1 - x0, y1 - y0);

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
            psx_ui_blend(c, ix, iy, col, cov);
        }
    }
}

/* Drawn as one pass over the ring rather than as a stack of ever-larger
 * rounded rects: the stack costs a full fill per layer over the whole shape. */
void psx_ui_round_rect_shadow(PsxUiCanvas *c, int x, int y, int w, int h,
                              float r, uint32_t col, int spread)
{
    float cx, cy, hw, hh, sp;
    int iy, ix, y0, y1, x0, x1, band0, band1;

    if (!c || !c->px || w <= 0 || h <= 0 || spread <= 0 || !(col >> 24)) return;
    r = rr_clamp(w, h, r);
    sp = (float)spread;

    x0 = x - spread < 0 ? 0 : x - spread;
    y0 = y - spread < 0 ? 0 : y - spread;
    x1 = x + w + spread > c->w ? c->w : x + w + spread;
    y1 = y + h + spread > c->h ? c->h : y + h + spread;
    if (x1 <= x0 || y1 <= y0) return;
    psx_ui_mark(c, x0, y0, x1 - x0, y1 - y0);

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
                ix = x + w - 1;             /* the shape itself covers this */
                continue;
            }
            d = rr_dist((float)ix + 0.5f, (float)iy + 0.5f, cx, cy, hw, hh, r);
            if (d <= 0.0f || d >= sp) continue;
            t = 1.0f - d / sp;
            psx_ui_blend(c, ix, iy, col, t * t);
        }
    }
}

/* Distance to a capsule: project onto the segment, clamp to its ends, measure.
 * Round caps rather than square ones because every caller here is drawing a
 * stroke that meets another stroke at an angle, and square caps leave a notch
 * at the join. */
void psx_ui_line(PsxUiCanvas *c, float x0, float y0, float x1, float y1,
                 float width, uint32_t col)
{
    float dx, dy, len2, half;
    int ix, iy, bx0, by0, bx1, by1, reach;

    if (!c || !c->px || !(col >> 24)) return;
    if (width <= 0.0f) width = 1.0f;
    half = width * 0.5f;
    dx = x1 - x0;
    dy = y1 - y0;
    len2 = dx * dx + dy * dy;

    reach = (int)half + 2;
    bx0 = (int)((x0 < x1 ? x0 : x1)) - reach;
    by0 = (int)((y0 < y1 ? y0 : y1)) - reach;
    bx1 = (int)((x0 > x1 ? x0 : x1)) + reach + 1;
    by1 = (int)((y0 > y1 ? y0 : y1)) + reach + 1;
    if (bx0 < 0) bx0 = 0;
    if (by0 < 0) by0 = 0;
    if (bx1 > c->w) bx1 = c->w;
    if (by1 > c->h) by1 = c->h;
    if (bx1 <= bx0 || by1 <= by0) return;
    psx_ui_mark(c, bx0, by0, bx1 - bx0, by1 - by0);

    for (iy = by0; iy < by1; iy++) {
        for (ix = bx0; ix < bx1; ix++) {
            float px = (float)ix + 0.5f, py = (float)iy + 0.5f;
            float ax = px - x0, ay = py - y0, t = 0.0f, ex, ey, d, cov;
            if (len2 > 0.0f) {
                t = (ax * dx + ay * dy) / len2;
                if (t < 0.0f) t = 0.0f;
                if (t > 1.0f) t = 1.0f;
            }
            ex = ax - dx * t;
            ey = ay - dy * t;
            d = (float)sqrt((double)(ex * ex + ey * ey)) - half;
            cov = 0.5f - d;
            if (cov <= 0.0f) continue;
            psx_ui_blend(c, ix, iy, col, cov);
        }
    }
}

void psx_ui_blit_scaled(PsxUiCanvas *c, int x, int y, int w, int h,
                        float r, const uint32_t *src, int sw, int sh)
{
    float ccx, ccy, chw, chh;
    int ix, iy;
    if (!c || !c->px || !src || w <= 0 || h <= 0 || sw <= 0 || sh <= 0) return;
    psx_ui_mark(c, x, y, w, h);
    r = rr_clamp(w, h, r);
    ccx = (float)x + (float)w * 0.5f;
    ccy = (float)y + (float)h * 0.5f;
    chw = (float)w * 0.5f;
    chh = (float)h * 0.5f;
    for (iy = 0; iy < h; iy++) {
        /* Sample at the destination pixel's CENTRE mapped back into the
         * source, minus half a source texel: corner-mapped sampling shifts the
         * whole image by half a texel and mirrors the top row into the box's
         * edge, which reads as a smear on a thumbnail this small. */
        float fy = ((float)iy + 0.5f) * (float)sh / (float)h - 0.5f;
        int   sy0 = (int)floorf(fy), sy1;
        float ty = fy - (float)sy0;
        int   dy = y + iy;
        if (sy0 < 0) { sy0 = 0; ty = 0.0f; }
        if (sy0 > sh - 1) { sy0 = sh - 1; ty = 0.0f; }
        sy1 = sy0 + 1 < sh ? sy0 + 1 : sy0;
        if ((unsigned)dy >= (unsigned)c->h) continue;
        for (ix = 0; ix < w; ix++) {
            float fx = ((float)ix + 0.5f) * (float)sw / (float)w - 0.5f;
            int   sx0 = (int)floorf(fx), sx1;
            float tx = fx - (float)sx0;
            int   dx = x + ix, i;
            unsigned out = 0xFF000000u;
            float cov;
            if (sx0 < 0) { sx0 = 0; tx = 0.0f; }
            if (sx0 > sw - 1) { sx0 = sw - 1; tx = 0.0f; }
            sx1 = sx0 + 1 < sw ? sx0 + 1 : sx0;
            if ((unsigned)dx >= (unsigned)c->w) continue;
            cov = 0.5f - rr_dist((float)dx + 0.5f, (float)dy + 0.5f,
                                 ccx, ccy, chw, chh, r);
            if (cov <= 0.0f) continue;
            for (i = 0; i < 3; i++) {
                const int sft = 16 - i * 8;
                float a = (float)((src[(size_t)sy0 * sw + sx0] >> sft) & 0xFFu);
                float b = (float)((src[(size_t)sy0 * sw + sx1] >> sft) & 0xFFu);
                float cc = (float)((src[(size_t)sy1 * sw + sx0] >> sft) & 0xFFu);
                float d = (float)((src[(size_t)sy1 * sw + sx1] >> sft) & 0xFFu);
                float top = a + (b - a) * tx;
                float bot = cc + (d - cc) * tx;
                float v = top + (bot - top) * ty;
                unsigned u = (unsigned)(v + 0.5f);
                if (u > 255u) u = 255u;
                out |= u << sft;
            }
            /* Blended, not stored: the rounded edge is partial coverage and
             * has to composite over whatever the box is sitting on. */
            psx_ui_blend(c, dx, dy, out, cov);
        }
    }
}

int psx_ui_text(PsxUiCanvas *c, int x, int baseline, const char *s,
                uint32_t col, const PsxUiFace *f)
{
    int end;
    if (!c || !c->px || !s || !f) return x;
    end = psx_ui_font_draw(c->px, c->w, c->h, x, baseline, s, col, f);
    psx_ui_mark(c, x, baseline - psx_ui_font_ascent(f) - 1,
                end - x + 2, psx_ui_font_line_height(f) + 2);
    return end;
}

void psx_ui_text_clip(PsxUiCanvas *c, int x, int baseline, const char *s,
                      uint32_t col, const PsxUiFace *f, int max_w)
{
    char buf[192];
    int n, ell;
    if (!c || !s || !f || max_w <= 0) return;
    if (psx_ui_font_text_w(f, s) <= max_w) {
        psx_ui_text(c, x, baseline, s, col, f);
        return;
    }
    ell = psx_ui_font_text_w(f, "...");
    for (n = 0; s[n] && n < (int)sizeof buf - 4; n++) {
        buf[n] = s[n];
        buf[n + 1] = '\0';
        if (psx_ui_font_text_w(f, buf) + ell > max_w) { buf[n] = '\0'; break; }
    }
    /* Never emit a dangling UTF-8 lead byte: the decoder would read past the
     * terminator looking for continuation bytes. */
    while (n > 0 && ((unsigned char)buf[n - 1] & 0xC0) == 0x80) buf[--n] = '\0';
    if (n > 0 && ((unsigned char)buf[n - 1] & 0x80)) buf[n - 1] = '\0';
    n = (int)strlen(buf);
    buf[n] = '.'; buf[n + 1] = '.'; buf[n + 2] = '.'; buf[n + 3] = '\0';
    psx_ui_text(c, x, baseline, buf, col, f);
}

int psx_ui_baseline_in(int y, int h, const PsxUiFace *f)
{
    int a = psx_ui_font_ascent(f), d = psx_ui_font_descent(f);
    return y + (h - (a + d)) / 2 + a;
}
