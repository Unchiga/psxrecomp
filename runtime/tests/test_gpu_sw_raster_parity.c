/* Deterministic raster corpus. Compare complete VRAM/mirror/dirty-row hashes
 * against the pre-optimization renderer; no game assets or SDL required. */
#include <stdio.h>
#ifndef RASTER_SOURCE
#define RASTER_SOURCE "../src/gpu_sw_renderer.c"
#endif
#include RASTER_SOURCE

int g_ws_bd_stretch_on, g_ws_bd_stretch_pct;
int psx_ws_prim_in_backdrop(void) { return 0; }
static uint16_t vram[1024 * 512];
static uint32_t random_state;
static uint32_t next_random(void) {
    random_state = random_state * 1664525u + 1013904223u;
    return random_state;
}
static uint64_t hash_bytes(uint64_t hash, const void *ptr, size_t len) {
    const uint8_t *bytes = ptr;
    while (len--) { hash ^= *bytes++; hash *= UINT64_C(1099511628211); }
    return hash;
}
int main(void) {
    for (int scale = 1; scale <= 4; scale++) {
        for (int filter = 0; filter <= 1; filter++) {
            for (int mode = 0; mode < 8; mode++) {
                random_state = 0x73931u;
                sw_renderer_init(vram);
                sw_renderer_set_scale(scale);
                for (size_t n = 0; n < sizeof(vram) / sizeof(vram[0]); n++)
                    vram[n] = (uint16_t)next_random();
                g_clip_x1 = 7; g_clip_x2 = 180;
                g_clip_y1 = 3; g_clip_y2 = 130;
                sw_wide_configure(mode & 4 ? 426 : 0, 53);
                if (mode & 4) sw_wide_set_target(0);
                sw_set_texture_filter(filter);
                sw_set_texture_window(mode & 1 ? 0x15A3u : 0);
                sw_set_semi_transparency(mode & 4, mode & 3);
                sw_set_mask_bits(mode & 1, mode & 2);
                sw_set_color_modulation(12, 20, 27, mode & 1);
                gpu_vram_dirty_set_tracking(1);
                gpu_vram_dirty_clear();
                sw_fill_rect(1020, 510, 19, 7, 0x3721);
                sw_fill_rect(-3, -4, 1041, 9, 0x8524);
                for (int i = 0; i < 12; i++) {
                    int x = (int)(next_random() % 180) - 20;
                    int y = (int)(next_random() % 130) - 20;
                    uint16_t page = (uint16_t)((i % 3) << 7);
                    sw_draw_flat_rect(x, y, 68, 41, 0x4371);
                    sw_draw_textured_rect(x, y, 73, 39, 231, 244, 32, 200, page);
                    sw_draw_textured_rect_scaled(x, y, 79, 47, 250, 246,
                                                i & 1 ? 12 : 281, 283,
                                                32, 200, page);
                    sw_draw_flat_triangle(x, y, x + 67, y + 17, x + 11, y + 61, 0x3271);
                    sw_draw_gouraud_triangle(x, y, 0x127F, x + 57, y + 3, 0x7310,
                                             x + 19, y + 59, 0x3361);
                    sw_draw_textured_triangle(x, y, 0, 0, x + 49, y + 5, 63, 0,
                                              x + 13, y + 57, 0, 63, 32, 200, page);
                    sw_draw_shaded_textured_triangle(x, y, 0, 0, 0x406080,
                        x + 51, y + 7, 63, 0, 0x804060,
                        x + 9, y + 43, 0, 63, 0x608040, 32, 200, page, mode & 1);
                }
                uint64_t hash = hash_bytes(UINT64_C(14695981039346656037), vram, sizeof(vram));
                if (g_hr) hash = hash_bytes(hash, g_hr, (size_t)g_hr_w * g_hr_h * sizeof(*g_hr));
                if (g_wide_cur) hash = hash_bytes(hash, g_wide_cur,
                    (size_t)g_wide_w * scale * VRAM_HEIGHT * scale * sizeof(*g_wide_cur));
                hash = hash_bytes(hash, gpu_vram_dirty_mask(), 64);
                printf("%d %d %d %016llx\n", scale, filter, mode, (unsigned long long)hash);
            }
        }
    }
    sw_renderer_set_scale(1);
    return 0;
}
