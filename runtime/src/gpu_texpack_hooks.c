/* gpu_texpack_hooks.c — see gpu_texpack_hooks.h. */

#include "gpu_texpack_hooks.h"
#include <string.h>

/* One registration, not a list like psx_game_hooks': exactly one HD
 * texture-pack implementation can be active in a process (it owns a single
 * shared GPU atlas), so there is nothing to fan out to multiple listeners
 * the way start/frame/vblank hooks do. Zero-initialised, so every member is
 * NULL until a title registers -- the gpu_texpack_* wrappers below all
 * check before calling through. */
static GpuTexpackHooks s_hooks;

void gpu_texpack_set_hooks(const GpuTexpackHooks *hooks) {
    if (hooks) s_hooks = *hooks;
    else       memset(&s_hooks, 0, sizeof s_hooks);
}

void gpu_texpack_set_vram(const uint16_t *vram) {
    if (s_hooks.set_vram) s_hooks.set_vram(vram);
}

void gpu_texpack_on_upload(int x, int y, int w, int h, const uint16_t *data) {
    if (s_hooks.on_upload) s_hooks.on_upload(x, y, w, h, data);
}

int gpu_texpack_on_draw(int base_x, int base_y, int depth, int clut_x, int clut_y,
                        const int lim[4], const int twin[4], const int dst[4],
                        GpuTexpackHit *out) {
    if (!s_hooks.on_draw) return 0;
    return s_hooks.on_draw(base_x, base_y, depth, clut_x, clut_y, lim, twin, dst, out);
}

void gpu_texpack_invalidate_rect(int x, int y, int w, int h) {
    if (s_hooks.invalidate_rect) s_hooks.invalidate_rect(x, y, w, h);
}

void gpu_texpack_note_copy(int sx, int sy, int w, int h, int dx, int dy) {
    if (s_hooks.note_copy) s_hooks.note_copy(sx, sy, w, h, dx, dy);
}

int gpu_texpack_atlas_dim(void) {
    return s_hooks.atlas_dim ? s_hooks.atlas_dim() : 0;
}

int gpu_texpack_take_pending(int *x, int *y, int *w, int *h, const uint8_t **rgba) {
    if (!s_hooks.take_pending) return 0;
    return s_hooks.take_pending(x, y, w, h, rgba);
}

void gpu_texpack_debug_note_prim(int rawtex, const float col[3]) {
    if (s_hooks.debug_note_prim) s_hooks.debug_note_prim(rawtex, col);
}
