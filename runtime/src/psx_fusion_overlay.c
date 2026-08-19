/* psx_fusion_overlay.c — the fusion assistant's line of text above the hand.
 *
 * See psx_fusion_overlay.h. This file owns two things: turning a card id into
 * readable text, and putting pixels in a canvas.
 *
 * ---- card names ------------------------------------------------------------
 *
 * The name blob is dictionary-compressed and does not hold ASCII. Its bytes are
 * the text engine's own character codes, a frequency-ordered alphabet where
 * SPACE is 0 and 'e' is 1, and 0xFF terminates. The table below is that
 * alphabet; it was derived by aligning the blob against a published id->name
 * list across all 722 cards and taking the per-code majority, so every code it
 * claims is backed by many names rather than one guess. Codes no card name
 * uses are left 0 and render as nothing.
 *
 *     name bytes at  0x801D0000 + u16[0x801D5800 + id*2]
 *
 * psx_card_drops.c takes the other route for the results page — it copies
 * these bytes verbatim into the game's own text engine, which needs no
 * alphabet at all. That is not available here: the engine draws where its
 * widgets say, and this line is somewhere no widget exists.
 */

#include "psx_fusion_overlay.h"

#include <stdio.h>
#include <string.h>

#include "mod_plugins.h"
#include "psx_fusion_assist.h"
#include "psx_fusion_db.h"
#include "psx_fusion_font.h"

#define PSX_FUSION_NAME_PTRS 0x801D5800u
#define PSX_FUSION_NAME_BASE 0x801D0000u
#define PSX_FUSION_NAME_END  0xFFu

static const char k_charset[0x80] = {
    /* 0x00 */ ' ', 'e', 't', 'a', 'o', 'i', 'n', 's',
    /* 0x08 */ 'r', 'h', 'l', '.', 'd', 'u', 'm', 'c',
    /* 0x10 */ 'g', 'y', 'w', 'f', 'p', 'b', 'k',   0,
    /* 0x18 */ 'A', 'v', 'I','\'', 'T', 'S', 'M', ',',
    /* 0x20 */ 'D', 'O', 'W', 'H', 'Y', 'E', 'R',   0,
    /* 0x28 */   0, 'G', 'L', 'C', 'N', 'B',   0, 'P',
    /* 0x30 */ '-', 'F', 'z', 'K', 'j', 'U', 'x', 'q',
    /* 0x38 */ '0', 'V', '2', 'J', '#', '1', 'Q', 'Z',
    /* 0x40 */   0, '3',   0, '&',   0, '7',   0,   0,
};

/* Layout, guest pixels. Above the hand, left-aligned like the card-name bar
 * underneath it. Tunable live: this sits against the game's own art. */
#define FO_W        320
#define FO_H        12
#define FO_X        0
#define FO_Y        124   /* fitted against the hand's top edge on screen */
#define FO_TEXT_X   16

static int s_x       = FO_X;
static int s_y       = FO_Y;
static int s_text_x  = FO_TEXT_X;
static int s_enabled = 1;

static uint32_t s_canvas[FO_W * FO_H];
static char     s_text[64];
static char     s_drawn[64];
static int      s_have_content;

/* Decode a card's name into `out` as ASCII. Returns the length. */
static int card_name(uint16_t id, char *out, int cap)
{
    int n = 0;
    if (id < 1 || id > PSX_FUSION_CARD_ID_MAX || cap <= 0) {
        if (cap > 0) out[0] = 0;
        return 0;
    }
    const uint32_t off =
        psx_mod_read_half(PSX_FUSION_NAME_PTRS + (uint32_t)id * 2u);
    uint32_t p = PSX_FUSION_NAME_BASE + off;
    for (int i = 0; i < cap - 1; i++) {
        const uint8_t b = psx_mod_read_byte(p + (uint32_t)i);
        if (b == PSX_FUSION_NAME_END) break;
        const char c = (b < 0x80u) ? k_charset[b] : 0;
        if (c) out[n++] = c;
    }
    out[n] = 0;
    return n;
}

/* Build the line this frame should show. Empty means "draw nothing". */
static void compose(char *out, int cap)
{
    out[0] = 0;
    if (!s_enabled || !psx_fusion_db_ready()) return;

    PsxFusionCard hand[PSX_FUSION_HAND_MAX];
    if (psx_fusion_assist_hand(hand, PSX_FUSION_HAND_MAX) < 2) return;

    /* Picked cards win: the player is mid-decision and wants THIS answer, not
     * a suggestion. One pick has no fusion in it yet, so the hint stands until
     * there are two. */
    PsxFusionCard steps[PSX_FUSION_HAND_MAX];
    uint16_t chain = 0;
    const int picked = psx_fusion_assist_chain(steps, PSX_FUSION_HAND_MAX,
                                               &chain);
    uint16_t show = 0;
    if (picked >= 2) {
        show = chain;
    } else {
        psx_fusion_assist_best(&show, NULL, NULL, NULL, NULL, 0);
    }
    if (!show) return;

    char name[40];
    card_name(show, name, sizeof name);
    if (!name[0]) return;

    int atk = 0, def = 0;
    psx_fusion_db_stats(show, &atk, &def, NULL);
    snprintf(out, (size_t)cap, "%s %d/%d", name, atk, def);
}

static void put_glyph(int cell, int x)
{
    const PsxFusionFont *f = &psx_fusion_font;
    if (cell < 0 || cell >= PSX_FUSION_FONT_CELLS) return;
    const uint8_t *g = f->px + (size_t)cell * (size_t)f->w * (size_t)f->h;
    for (int row = 0; row < f->h && row < FO_H; row++) {
        for (int col = 0; col < f->w; col++) {
            const int dx = x + col;
            if (dx < 0 || dx >= FO_W) continue;
            const uint8_t v = g[row * f->w + col];
            if (!v) continue;
            /* The stored 4-bit value IS the brightness the game draws: 1 is
             * the dark outline, 15 the white core. Scaling it to 8 bits
             * reproduces the same anti-aliased text. */
            const uint32_t l = (uint32_t)v * 17u;
            s_canvas[row * FO_W + dx] = 0xFF000000u | (l << 16) | (l << 8) | l;
        }
    }
}

static void redraw(void)
{
    memset(s_canvas, 0, sizeof s_canvas);
    s_have_content = 0;
    int x = s_text_x;
    for (const char *p = s_drawn; *p; p++) {
        if (x >= FO_W) break;
        const int cell = psx_fusion_font_cell((unsigned char)*p);
        if (cell >= 0) { put_glyph(cell, x); s_have_content = 1; }
        x += psx_fusion_font.w;
    }
}

void psx_fusion_overlay_tick(void)
{
    compose(s_text, sizeof s_text);
    if (strcmp(s_text, s_drawn) == 0) return;   /* nothing changed */
    memcpy(s_drawn, s_text, sizeof s_drawn);
    redraw();
}

int psx_fusion_overlay_image(const uint32_t **pixels, int *w, int *h)
{
    if (!s_have_content) return 0;
    if (pixels) *pixels = s_canvas;
    if (w) *w = FO_W;
    if (h) *h = FO_H;
    return 1;
}

void psx_fusion_overlay_origin(int *x, int *y)
{
    if (x) *x = s_x;
    if (y) *y = s_y;
}

int psx_fusion_overlay_needs_present(void) { return s_have_content; }

void psx_fusion_overlay_tune(int x, int y, int text_x, int enabled)
{
    if (x       != PSX_FUSION_OVERLAY_KEEP) s_x       = x;
    if (y       != PSX_FUSION_OVERLAY_KEEP) s_y       = y;
    if (text_x  != PSX_FUSION_OVERLAY_KEEP) s_text_x  = text_x;
    if (enabled != PSX_FUSION_OVERLAY_KEEP) s_enabled = enabled ? 1 : 0;
    s_drawn[0] = 0;          /* force a re-rasterise at the new geometry */
    psx_fusion_overlay_tick();
}

void psx_fusion_overlay_tune_get(int *x, int *y, int *text_x, int *enabled)
{
    if (x)       *x       = s_x;
    if (y)       *y       = s_y;
    if (text_x)  *text_x  = s_text_x;
    if (enabled) *enabled = s_enabled;
}

const char *psx_fusion_overlay_text(void) { return s_drawn; }
