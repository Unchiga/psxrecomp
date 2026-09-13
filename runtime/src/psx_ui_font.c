/* Antialiased proportional text for host overlays. See psx_ui_font.h.
 *
 * Two design notes worth keeping.
 *
 * BAKED ATLASES, NOT PER-CALL RASTERISATION. stb_truetype's per-codepoint
 * rasteriser is a scanline fill over the outline; running it for every glyph of
 * every string on every redraw is orders of magnitude more work than blending
 * a cached coverage bitmap. Callers redraw only when their state changes, so
 * the atlas is baked once per (size, weight) and reused until the window
 * resizes.
 *
 * INTEGER PEN POSITIONS. Advances are accumulated as floats and rounded once
 * at each glyph, rather than being truncated per character. Truncating is what
 * makes proportional text look like it has random extra spaces in it; carrying
 * the fraction keeps the whole string's width equal to the sum of its real
 * advances, which is also what psx_ui_font_text_w reports, so measuring and
 * drawing agree exactly. */

#include "psx_ui_font.h"

#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
/* Deliberately NOT STBTT_STATIC: this is the only translation unit that
 * includes the implementation, and marking the whole library static makes the
 * two thirds of it nobody calls into unused-function warnings. */
/* No stdio: this module never touches a file. The font is compiled in. */
#define STBTT_assert(x) ((void)0)
#include "../third_party/stb_truetype.h"

#include "../third_party/fonts/psx_ui_font_data.h"

/* Glyph set baked into an atlas: printable ASCII, plus the punctuation UI text
 * actually reaches for. The extras are a separate list rather than a wider
 * contiguous range because the gap between U+007F and U+2014 is thousands of
 * codepoints the font does not carry -- and it MUST match what
 * tools/make_ui_font.py subsets, or a dash renders as a question mark, which
 * is precisely the failure the whole exercise was meant to end. */
#define UF_FIRST 32
#define UF_LAST  126
#define UF_ASCII_N (UF_LAST - UF_FIRST + 1)

static const int UF_EXTRA[] = {
    0x00D7,                                     /* multiplication sign */
    0x2013, 0x2014,                             /* en dash, em dash */
    0x2018, 0x2019, 0x201C, 0x201D,             /* curly quotes */
    0x2022, 0x2026,                             /* bullet, ellipsis */
    0x2190, 0x2191, 0x2192, 0x2193,             /* arrows */
    0x2713                                      /* check mark */
};
#define UF_EXTRA_N ((int)(sizeof UF_EXTRA / sizeof UF_EXTRA[0]))

/* The icon face's whole repertoire, in the order tools/make_ui_font.py subsets
 * it. Must match ICONS there and the PSX_UI_ICON_* names in the header. */
static const int UF_ICON_CP[] = {
    0xE050, 0xE2C7, 0xE417, 0xE429, 0xE6EC, 0xE87B, 0xEA0B, 0xEF5B
};
#define UF_ICON_N ((int)(sizeof UF_ICON_CP / sizeof UF_ICON_CP[0]))

/* One array sized for the larger of the two repertoires, so a face of either
 * family fits the same struct. */
#define UF_TEXT_N  (UF_ASCII_N + UF_EXTRA_N)
#define UF_COUNT   (UF_TEXT_N > UF_ICON_N ? UF_TEXT_N : UF_ICON_N)

/* Faces are one per (rounded pixel size, weight). Sized to hold every face
 * ALIVE AT ONCE across all the overlays, not merely the ones any one of them
 * uses: see the eviction path in psx_ui_font_face, which drops the WHOLE cache
 * when it runs out of slots. A caller that took a face, then asked for one
 * more than fits, would be left drawing through a freed pointer -- and every
 * caller here does exactly that, holding a title face across a loop that
 * fetches a row face per item.
 *
 * Today: menu 5 (title, row in two weights, hint, icons), save-state browser
 * 3, toast 1 -- with the smaller sizes deliberately chosen to land on the
 * menu's, so they share a bake rather than adding one. Sixteen leaves room to
 * add a size without anyone having to rediscover the paragraph above. */
#define UF_MAX_FACES 16

/* Atlas dimensions. 96 glyphs of Inter at the largest size the UI asks for
 * (a 4K window lands near 60 px/em) pack into well under this; the bake fails
 * loudly rather than silently clipping if a caller ever asks for more. */
#define UF_ATLAS_W 1024
#define UF_ATLAS_H 1024

/* Largest pixel size that can be baked. Bounds the atlas, and bounds what a
 * bad layout computation can ask for. */
#define UF_MAX_PX 200.0f
#define UF_MIN_PX 6.0f

struct PsxUiFace {
    int   used;
    int   px;                 /* rounded pixel size this was baked at */
    int   family;             /* PSX_UI_FONT_* */
    int   ascent, descent, line;
    unsigned char   *atlas;   /* UF_ATLAS_W * UF_ATLAS_H coverage, 8-bit */
    stbtt_packedchar chars[UF_COUNT];
};

static struct PsxUiFace s_faces[UF_MAX_FACES];

/* One parsed stbtt_fontinfo per weight, kept because parsing is pure header
 * walking and there is no reason to redo it per bake. */
static stbtt_fontinfo s_info[3];
static int            s_info_ok[3];
static int            s_info_tried[3];

static const unsigned char *font_blob(int family, int *len)
{
    if (family == PSX_UI_FONT_ICONS) {
        *len = (int)sizeof PSX_UI_FONT_ICONS_TTF;
        return PSX_UI_FONT_ICONS_TTF;
    }
    if (family == PSX_UI_FONT_SEMIBOLD) {
        *len = (int)sizeof PSX_UI_FONT_SEMIBOLD_TTF;
        return PSX_UI_FONT_SEMIBOLD_TTF;
    }
    *len = (int)sizeof PSX_UI_FONT_REGULAR_TTF;
    return PSX_UI_FONT_REGULAR_TTF;
}

static int family_ok(int family)
{
    return family == PSX_UI_FONT_SEMIBOLD || family == PSX_UI_FONT_ICONS
               ? family : PSX_UI_FONT_REGULAR;
}

static stbtt_fontinfo *font_info(int family)
{
    int len = 0;
    const unsigned char *blob;
    family = family_ok(family);
    if (s_info_tried[family]) return s_info_ok[family] ? &s_info[family] : NULL;
    s_info_tried[family] = 1;
    blob = font_blob(family, &len);
    if (stbtt_InitFont(&s_info[family], blob,
                       stbtt_GetFontOffsetForIndex(blob, 0))) {
        s_info_ok[family] = 1;
        return &s_info[family];
    }
    return NULL;
}

static void face_free(struct PsxUiFace *f)
{
    if (f->atlas) free(f->atlas);
    memset(f, 0, sizeof *f);
}

void psx_ui_font_reset(void)
{
    int i;
    for (i = 0; i < UF_MAX_FACES; i++)
        if (s_faces[i].used) face_free(&s_faces[i]);
}

static int face_bake(struct PsxUiFace *f, int px, int family)
{
    stbtt_pack_context pc;
    stbtt_pack_range rng[2];
    stbtt_fontinfo *fi = font_info(family);
    int len = 0, a = 0, d = 0, gap = 0, nrange;
    float sc;

    if (!fi) return 0;
    f->atlas = (unsigned char *)malloc((size_t)UF_ATLAS_W * UF_ATLAS_H);
    if (!f->atlas) return 0;

    if (!stbtt_PackBegin(&pc, f->atlas, UF_ATLAS_W, UF_ATLAS_H, 0, 1, NULL)) {
        free(f->atlas);
        f->atlas = NULL;
        return 0;
    }
    /* No oversampling. Oversampled atlases must be sampled at subpixel offsets
     * to pay off, and this blitter places glyphs on whole pixels; asking for it
     * here would cost 4x the atlas for a blurrier result. */
    stbtt_PackSetOversampling(&pc, 1, 1);
    memset(rng, 0, sizeof rng);
    if (family == PSX_UI_FONT_ICONS) {
        rng[0].font_size                   = (float)px;
        rng[0].array_of_unicode_codepoints = (int *)UF_ICON_CP;
        rng[0].num_chars                   = UF_ICON_N;
        rng[0].chardata_for_range          = f->chars;
        nrange = 1;
    } else {
        rng[0].font_size                        = (float)px;
        rng[0].first_unicode_codepoint_in_range = UF_FIRST;
        rng[0].num_chars                        = UF_ASCII_N;
        rng[0].chardata_for_range               = f->chars;
        rng[1].font_size                        = (float)px;
        rng[1].array_of_unicode_codepoints      = (int *)UF_EXTRA;
        rng[1].num_chars                        = UF_EXTRA_N;
        rng[1].chardata_for_range               = f->chars + UF_ASCII_N;
        nrange = 2;
    }
    if (!stbtt_PackFontRanges(&pc, font_blob(family, &len), 0, rng, nrange)) {
        stbtt_PackEnd(&pc);
        free(f->atlas);
        f->atlas = NULL;
        return 0;
    }
    stbtt_PackEnd(&pc);

    sc = stbtt_ScaleForPixelHeight(fi, (float)px);
    stbtt_GetFontVMetrics(fi, &a, &d, &gap);
    f->ascent  = (int)(a * sc + 0.5f);
    f->descent = (int)(-d * sc + 0.5f);
    f->line    = (int)((a - d + gap) * sc + 0.5f);
    f->px      = px;
    f->family  = family;
    f->used    = 1;
    return 1;
}

const PsxUiFace *psx_ui_font_face(float px, int family)
{
    int i, want, free_slot = -1;

    family = family_ok(family);
    if (px < UF_MIN_PX) px = UF_MIN_PX;
    if (px > UF_MAX_PX) px = UF_MAX_PX;
    want = (int)(px + 0.5f);

    for (i = 0; i < UF_MAX_FACES; i++) {
        if (s_faces[i].used && s_faces[i].px == want &&
            s_faces[i].family == family)
            return &s_faces[i];
        if (!s_faces[i].used && free_slot < 0) free_slot = i;
    }
    /* Full. Drop everything rather than picking a victim: the only way to fill
     * every slot is a run of layout changes, after which the sizes in the
     * cache are the old ones, not the ones about to be asked for.
     *
     * This INVALIDATES every face pointer a caller is holding, which is why
     * UF_MAX_FACES is sized to the whole UI rather than to any one module. */
    if (free_slot < 0) {
        psx_ui_font_reset();
        free_slot = 0;
    }
    if (!face_bake(&s_faces[free_slot], want, family)) {
        face_free(&s_faces[free_slot]);
        return NULL;
    }
    return &s_faces[free_slot];
}

int psx_ui_font_ascent(const PsxUiFace *f)      { return f ? f->ascent : 0; }
int psx_ui_font_descent(const PsxUiFace *f)     { return f ? f->descent : 0; }
int psx_ui_font_line_height(const PsxUiFace *f) { return f ? f->line : 0; }

/* Minimal UTF-8 decode. Returns the codepoint and advances *s past it; an
 * ill-formed byte is consumed as U+FFFD so a bad string cannot loop forever. */
static unsigned uf_next(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    unsigned c = *p++;
    int extra = 0;
    if (c < 0x80) { *s = (const char *)p; return c; }
    if ((c & 0xE0) == 0xC0) { c &= 0x1F; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { c &= 0x0F; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { c &= 0x07; extra = 3; }
    else { *s = (const char *)p; return 0xFFFD; }
    while (extra--) {
        if ((*p & 0xC0) != 0x80) { *s = (const char *)p; return 0xFFFD; }
        c = (c << 6) | (unsigned)(*p++ & 0x3F);
    }
    *s = (const char *)p;
    return c;
}

/* The packed record for a codepoint. For a text face this falls back to '?',
 * so an unexpected character is visible rather than an invisible gap. For the
 * icon face there is no sensible substitute, so an unknown codepoint returns
 * NULL and draws nothing -- a tofu box in a menu bar is worse than a gap. */
static const stbtt_packedchar *uf_glyph(const struct PsxUiFace *f, unsigned cp)
{
    int i;
    if (f->family == PSX_UI_FONT_ICONS) {
        for (i = 0; i < UF_ICON_N; i++)
            if ((unsigned)UF_ICON_CP[i] == cp) return &f->chars[i];
        return NULL;
    }
    if (cp >= UF_FIRST && cp <= UF_LAST) return &f->chars[cp - UF_FIRST];
    for (i = 0; i < UF_EXTRA_N; i++)
        if ((unsigned)UF_EXTRA[i] == cp) return &f->chars[UF_ASCII_N + i];
    return &f->chars['?' - UF_FIRST];
}

char *psx_ui_font_utf8(unsigned cp, char *out)
{
    int n = 0;
    if (!out) return out;
    if (cp < 0x80) out[n++] = (char)cp;
    else if (cp < 0x800) {
        out[n++] = (char)(0xC0 | (cp >> 6));
        out[n++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out[n++] = (char)(0xE0 | (cp >> 12));
        out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[n++] = (char)(0x80 | (cp & 0x3F));
    } else {
        out[n++] = (char)(0xF0 | (cp >> 18));
        out[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[n++] = (char)(0x80 | (cp & 0x3F));
    }
    out[n] = '\0';
    return out;
}

int psx_ui_font_text_w_n(const PsxUiFace *f, const char *utf8, int bytes)
{
    const char *s = utf8, *end;
    float x = 0.0f;
    if (!f || !utf8) return 0;
    end = utf8 + (bytes < 0 ? (int)strlen(utf8) : bytes);
    while (s < end) {
        unsigned cp = uf_next(&s);
        const stbtt_packedchar *g = uf_glyph(f, cp);
        if (g) x += g->xadvance;
    }
    return (int)(x + 0.5f);
}

int psx_ui_font_text_w(const PsxUiFace *f, const char *utf8)
{
    return psx_ui_font_text_w_n(f, utf8, -1);
}

/* Blend one coverage value over a destination pixel. The canvas is premultiply-
 * free straight ARGB that the presenter blends with SRC_ALPHA/ONE_MINUS, so
 * both colour and alpha have to be composited here, not just colour: text drawn
 * over the transparent part of the canvas must carry its own alpha out or it
 * vanishes at present time. */
static void uf_blend(uint32_t *px, uint32_t argb, unsigned cov)
{
    uint32_t d = *px;
    unsigned sa = ((argb >> 24) & 0xFFu) * cov / 255u;
    unsigned da = (d >> 24) & 0xFFu;
    unsigned oa, i;
    unsigned sc[3], dc[3], oc[3];

    if (!sa) return;
    if (sa == 255u) { *px = 0xFF000000u | (argb & 0x00FFFFFFu); return; }

    sc[0] = (argb >> 16) & 0xFFu; sc[1] = (argb >> 8) & 0xFFu; sc[2] = argb & 0xFFu;
    dc[0] = (d >> 16) & 0xFFu;    dc[1] = (d >> 8) & 0xFFu;    dc[2] = d & 0xFFu;

    oa = sa + da * (255u - sa) / 255u;
    if (!oa) { *px = 0; return; }
    for (i = 0; i < 3; i++)
        oc[i] = (sc[i] * sa + dc[i] * da * (255u - sa) / 255u) / oa;

    *px = (oa << 24) | (oc[0] << 16) | (oc[1] << 8) | oc[2];
}

int psx_ui_font_draw(uint32_t *dst, int dst_w, int dst_h,
                     int x, int y, const char *utf8,
                     uint32_t argb, const PsxUiFace *f)
{
    const char *s = utf8;
    float pen;

    if (!dst || !f || !utf8 || dst_w <= 0 || dst_h <= 0) return x;
    pen = (float)x;

    while (*s) {
        unsigned cp = uf_next(&s);
        const stbtt_packedchar *g = uf_glyph(f, cp);
        int gw, gh, gx, gy, row, col;
        if (!g) continue;                  /* icon face, codepoint not in it */
        gw = g->x1 - g->x0;
        gh = g->y1 - g->y0;
        gx = (int)(pen + g->xoff + 0.5f);
        gy = y + (int)(g->yoff + 0.5f);

        pen += g->xadvance;
        if (gw <= 0 || gh <= 0) continue;              /* space, and friends */
        for (row = 0; row < gh; row++) {
            int dy = gy + row;
            const unsigned char *src;
            uint32_t *out;
            if ((unsigned)dy >= (unsigned)dst_h) continue;
            src = f->atlas + (size_t)(g->y0 + row) * UF_ATLAS_W + g->x0;
            out = dst + (size_t)dy * dst_w;
            for (col = 0; col < gw; col++) {
                int dx = gx + col;
                unsigned cov = src[col];
                if (!cov || (unsigned)dx >= (unsigned)dst_w) continue;
                uf_blend(&out[dx], argb, cov);
            }
        }
    }
    return (int)(pen + 0.5f);
}
