/* Render a host overlay to a PNG, with no window, no GL and no guest.
 *
 * WHY THIS EXISTS. The debug server's `screenshot` resolves native 15-bit VRAM
 * and `screenshot_hires` the supersampled mirror; both run before anything is
 * composited on top, so NEITHER can see these -- they are host overlays
 * blended into the present. That leaves "does it look right" answerable only
 * by asking a human to look at the window, which is not a check anyone can run
 * in a loop while tuning a layout.
 *
 * This drives the real modules -- the same psx_video_menu.c, host_osd.c and
 * psx_savestate_menu.c the runtime links, the same rasterisers, the same
 * canvases -- and writes exactly the ARGB image the renderers composite. What
 * it cannot show is the blend against live gameplay underneath, which is why
 * it draws onto a checkerboard: anything that reads as opaque here is opaque
 * there.
 *
 *   menu_preview [--menu|--toast|--slots] <out.png> [width height] [a] [b]
 *
 * --menu   the default, and what a bare <out.png> still means. `a` is a
 *          top-level index (0 FILE .. 6 MODS), -1 for the collapsed bar; `b`
 *          is the highlighted row. Rows registered by a title are absent,
 *          because no title is loaded.
 * --toast  `a` is the message, quoted:
 *            menu_preview --toast out.png 1920 1080 "Saved slot 3"
 * --slots  `a` is the selected slot.
 *
 * The save-state browser reads slot files through savestate.c and key names
 * through host_keymap.c, neither of which exists without a running emulator.
 * Both are stubbed at the bottom of this file with a deliberately MIXED state
 * -- some slots written, some empty -- because a browser previewing an empty
 * disk exercises one of its two row states and none of its thumbnail path.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_keymap.h"
#include "host_osd.h"
#include "psx_savestate_menu.h"
#include "psx_video_menu.h"
#include "savestate.h"

/* ---- minimal PNG writer --------------------------------------------------
 * Stored (uncompressed) deflate blocks, so there is no zlib dependency and no
 * second vendored library for a developer tool. A few hundred KB per preview
 * is irrelevant next to not having a picture at all. */

static unsigned long crc_table[256];
static int crc_ready;

static void crc_init(void)
{
    unsigned long c;
    int n, k;
    for (n = 0; n < 256; n++) {
        c = (unsigned long)n;
        for (k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320UL ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
    crc_ready = 1;
}

static unsigned long crc_upd(unsigned long c, const unsigned char *b, size_t n)
{
    size_t i;
    if (!crc_ready) crc_init();
    for (i = 0; i < n; i++) c = crc_table[(c ^ b[i]) & 0xFF] ^ (c >> 8);
    return c;
}

static void put_be32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);  p[3] = (unsigned char)v;
}

static void chunk(FILE *f, const char *tag, const unsigned char *data, size_t n)
{
    unsigned char hdr[8];
    unsigned char crcb[4];
    unsigned long c;
    put_be32(hdr, (unsigned long)n);
    memcpy(hdr + 4, tag, 4);
    fwrite(hdr, 1, 8, f);
    if (n) fwrite(data, 1, n, f);
    c = crc_upd(0xFFFFFFFFUL, hdr + 4, 4);
    if (n) c = crc_upd(c, data, n);
    put_be32(crcb, c ^ 0xFFFFFFFFUL);
    fwrite(crcb, 1, 4, f);
}

static int write_png(const char *path, const unsigned char *rgb, int w, int h)
{
    static const unsigned char sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    unsigned char ihdr[13];
    unsigned char *raw, *z;
    size_t raw_len = (size_t)h * ((size_t)w * 3 + 1), zi = 0, off = 0;
    unsigned long adler_a = 1, adler_b = 0;
    size_t i, blocks;
    FILE *f = fopen(path, "wb");
    if (!f) return 0;

    fwrite(sig, 1, 8, f);
    put_be32(ihdr, (unsigned long)w);
    put_be32(ihdr + 4, (unsigned long)h);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    chunk(f, "IHDR", ihdr, 13);

    raw = (unsigned char *)malloc(raw_len);
    if (!raw) { fclose(f); return 0; }
    for (i = 0; i < (size_t)h; i++) {
        raw[off++] = 0;                                   /* filter: none */
        memcpy(raw + off, rgb + i * (size_t)w * 3, (size_t)w * 3);
        off += (size_t)w * 3;
    }
    for (i = 0; i < raw_len; i++) {
        adler_a = (adler_a + raw[i]) % 65521UL;
        adler_b = (adler_b + adler_a) % 65521UL;
    }

    blocks = raw_len / 65535 + 1;
    z = (unsigned char *)malloc(raw_len + blocks * 5 + 6 + 8);
    if (!z) { free(raw); fclose(f); return 0; }
    z[zi++] = 0x78; z[zi++] = 0x01;
    off = 0;
    while (off < raw_len) {
        size_t n = raw_len - off;
        int last;
        if (n > 65535) n = 65535;
        last = (off + n >= raw_len);
        z[zi++] = (unsigned char)last;
        z[zi++] = (unsigned char)(n & 0xFF);
        z[zi++] = (unsigned char)(n >> 8);
        z[zi++] = (unsigned char)(~n & 0xFF);
        z[zi++] = (unsigned char)((~n >> 8) & 0xFF);
        memcpy(z + zi, raw + off, n);
        zi += n;
        off += n;
    }
    put_be32(z + zi, (adler_b << 16) | adler_a);
    zi += 4;
    chunk(f, "IDAT", z, zi);
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(z);
    free(raw);
    return 1;
}

/* ---- preview -------------------------------------------------------------- */

/* Flatten the overlay onto a checkerboard. The canvas is straight ARGB with
 * real transparency; composited onto flat black, a translucent panel and an
 * opaque one look identical, which is exactly the thing worth seeing. */
static void composite(const uint32_t *src, int w, int h, unsigned char *out)
{
    int x, y;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            uint32_t p = src[(size_t)y * w + x];
            unsigned a = (p >> 24) & 0xFF;
            unsigned bg = (((x >> 4) ^ (y >> 4)) & 1) ? 0x4A : 0x38;
            unsigned c[3];
            int i;
            c[0] = (p >> 16) & 0xFF; c[1] = (p >> 8) & 0xFF; c[2] = p & 0xFF;
            for (i = 0; i < 3; i++)
                out[((size_t)y * w + x) * 3 + i] =
                    (unsigned char)((c[i] * a + bg * (255 - a)) / 255);
        }
    }
}

/* ---- the three subjects --------------------------------------------------- */

static int preview_menu(int w, int h, int menu, int row,
                        const uint32_t **px, int *ow, int *oh)
{
    PsxVideoMenuState st;
    int ui, i;

    memset(&st, 0, sizeof st);
    st.scaling = PSX_VM_SCALING_INTEGER;
    st.filter = PSX_VM_FILTER_LINEAR;
    st.texture_filter = PSX_VM_FILTER_NEAREST;
    st.screen = PSX_VM_SCREEN_WINDOWED;
    st.windowed_scale = PSX_VM_WINDOWED_SCALE_DEFAULT;
    st.vsync = PSX_VM_VSYNC_OFF;
    st.supersampling = 1;
    st.speed = PSX_VM_SPEED_DEFAULT;
    st.fast_loads = PSX_VM_LOADS_OFF;
    st.vol_master = 100; st.vol_music = 80; st.vol_sound = 100;
    st.speed_governor = 0;
    st.update_check = 1;
    st.renderer = PSX_VM_RENDERER_UNSET;

    psx_video_menu_init(&st);
    ui = psx_video_menu_ui_scale(w, h);
    psx_video_menu_set_layout(w / ui, h / ui, ui);

    if (menu >= 0) {
        /* Drive it the way a player would rather than poking state: the module
         * has no setter for "open menu N", and adding one for a preview would
         * be a test-only path through code the player never takes. */
        PsxVideoMenuDebug dbg;
        psx_video_menu_toggle();   /* F10: hides -- the bar starts visible */
        psx_video_menu_toggle();   /* F10 again: visible AND expanded */
        /* Walk RIGHT from wherever init left the cursor rather than assuming
         * it starts at zero, and stop when the snapshot says we arrived --
         * the menu count is not exported and titles can add their own. */
        for (i = 0; i < 32; i++) {
            psx_video_menu_debug_snapshot(&dbg);
            if (dbg.menu == menu) break;
            psx_video_menu_handle_key(1073741903);   /* RIGHT */
        }
        for (i = 0; i < row; i++)  psx_video_menu_handle_key(1073741905); /* DOWN */
    }

    psx_video_menu_prepare();
    if (!psx_video_menu_overlay_image_ro(px, ow, oh) || !*px) {
        fprintf(stderr, "menu produced no canvas\n");
        return 0;
    }
    printf("menu: ui scale %d\n", ui);
    return 1;
}

static int preview_toast(int w, int h, const char *msg,
                         const uint32_t **px, int *ow, int *oh)
{
    host_osd_set_layout(w, h);
    host_osd_push(msg, 60000);
    if (!host_osd_image(px, ow, oh) || !*px) {
        fprintf(stderr, "toast produced no image\n");
        return 0;
    }
    printf("toast: on a %dx%d surface\n", w, h);
    return 1;
}

static int preview_slots(int w, int h, int slot,
                         const uint32_t **px, int *ow, int *oh)
{
    /* The bar height this panel insets its header by comes from the menu, and
     * the menu has to be initialised for that number to mean anything. */
    PsxVideoMenuState st;
    memset(&st, 0, sizeof st);
    psx_video_menu_init(&st);

    psx_savestate_menu_set_layout(w, h);
    psx_savestate_menu_set_state(1, slot);
    if (!psx_savestate_menu_overlay_image(px, ow, oh) || !*px) {
        fprintf(stderr, "slot browser produced no canvas\n");
        return 0;
    }
    printf("slots: selected %d\n", slot);
    return 1;
}

int main(int argc, char **argv)
{
    const uint32_t *px = NULL;
    unsigned char *rgb;
    const char *out;
    const char *msg = "Speed eased to 1x to keep audio clean";
    enum { WHAT_MENU, WHAT_TOAST, WHAT_SLOTS } what = WHAT_MENU;
    int w = 1920, h = 1080, a = 2, b = 0, ow = 0, oh = 0, arg = 1, ok;

    if (arg < argc && argv[arg][0] == '-' && argv[arg][1] == '-') {
        if (!strcmp(argv[arg], "--toast")) what = WHAT_TOAST;
        else if (!strcmp(argv[arg], "--slots")) what = WHAT_SLOTS;
        else if (strcmp(argv[arg], "--menu")) {
            fprintf(stderr, "unknown mode %s\n", argv[arg]);
            return 2;
        }
        arg++;
    }
    out = (arg < argc) ? argv[arg++] : "menu_preview.png";
    if (arg + 1 < argc) { w = atoi(argv[arg]); h = atoi(argv[arg + 1]); arg += 2; }
    if (arg < argc) {
        if (what == WHAT_TOAST) msg = argv[arg];
        else a = atoi(argv[arg]);
        arg++;
    }
    if (arg < argc) b = atoi(argv[arg]);
    if (w < 160 || h < 120) { fprintf(stderr, "bad size\n"); return 2; }

    switch (what) {
    case WHAT_TOAST: ok = preview_toast(w, h, msg, &px, &ow, &oh); break;
    case WHAT_SLOTS: ok = preview_slots(w, h, a, &px, &ow, &oh); break;
    default:         ok = preview_menu(w, h, a, b, &px, &ow, &oh); break;
    }
    if (!ok) return 1;

    rgb = (unsigned char *)malloc((size_t)ow * oh * 3);
    if (!rgb) return 1;
    composite(px, ow, oh, rgb);
    if (!write_png(out, rgb, ow, oh)) {
        fprintf(stderr, "cannot write %s\n", out);
        return 1;
    }
    printf("%s  %dx%d\n", out, ow, oh);
    free(rgb);
    return 0;
}

/* ---- stubs for what a preview has no emulator to provide ------------------ */

const char *host_keymap_label(HostKeymapAction action, char *out, size_t cap)
{
    (void)action;
    snprintf(out, cap, "F7");
    return out;
}

int savestate_slot_mtime(int slot, int64_t *out_time)
{
    if (slot % 3 == 2) return 0;                 /* every third slot empty */
    if (out_time) *out_time = 1735689600 + (int64_t)slot * 86400;
    return 1;
}

int savestate_read_thumb(int slot, uint32_t *out_argb, int w, int h)
{
    int x, y;
    if (slot % 3 == 2 || !out_argb || w < 2 || h < 2) return 0;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            /* Structure in both axes: a flat fill cannot show whether the
             * scaler is sampling where it thinks it is. */
            const unsigned r = (unsigned)(x * 255 / (w - 1));
            const unsigned g = (unsigned)(y * 255 / (h - 1));
            const unsigned bl = (((x >> 3) ^ (y >> 3)) & 1) ? 200u : 60u;
            out_argb[y * w + x] = 0xFF000000u | (r << 16) | (g << 8) | bl;
        }
    }
    return 1;
}

/* host_osd.c reaches for this from host_osd_needs_present, which no preview
 * calls -- but the reference is in the object either way. */
int psx_rewind_needs_present(void) { return 0; }

/* SDL supplies the clock in a real build. The toast's own expiry is the only
 * thing that reads it, and a preview lives for exactly one image. */
unsigned long long SDL_GetTicks(void) { return 0; }
