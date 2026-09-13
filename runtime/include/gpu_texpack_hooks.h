/* gpu_texpack_hooks.h — a title's own HD texture-replacement engine, called
 * back into the same way psx_game_hooks.h lets a title run its own start-up
 * and per-frame work.
 *
 * The runtime carries no game-specific code, so it cannot name a title's
 * texture_pack.c functions directly -- gpu_render.c and gpu_gl_renderer.c
 * used to `#include "texture_pack.h"` and call texpack_on_upload/on_draw/
 * take_pending/set_vram/invalidate_rect/note_copy/atlas_dim/
 * debug_note_prim straight through. That header and every one of those
 * symbols exist only in this title's own src/texture_pack.c, which the
 * superproject's CMakeLists.txt compiles only when game C is present
 * (YGOFM_HAS_GAME_C) -- so a build without game C (the release setup host,
 * `-DPSXRECOMP_FORCE_SETUP_HOST=ON`, or any other title linking this
 * runtime) failed to link with undefined references to every one of them.
 * Found live 2026-09-14, reviewing the paired PR that introduced this
 * feature.
 *
 * The fix is the same shape as psx_game_hooks: a title registers a table of
 * callbacks from its own PSX_MOD_CONSTRUCTOR (see mod_plugins.h), and the
 * runtime calls through the gpu_texpack_* wrapper functions below, which are
 * safe to call whether or not anything is registered -- every member
 * defaults to NULL/no-op, so a build that never registers hooks (no game C,
 * or a title that simply doesn't want this feature) links and runs exactly
 * as if this file did not exist.
 *
 * GpuTexpackHit is TexPackHit's own shape, moved here rather than merely
 * mirrored: the ENGINE is what consumes it (gpu_gl_renderer.c reads its
 * fields straight into shader uniforms), so "what a replacement looks like"
 * is the engine's own contract, not any one title's matching
 * implementation's business to define. texture_pack.h includes this header
 * and aliases TexPackHit to it, so every existing game-side use of the name
 * TexPackHit keeps compiling unchanged. */
#ifndef GPU_TEXPACK_HOOKS_H
#define GPU_TEXPACK_HOOKS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Where a primitive should read its replacement, in atlas texels. See
 * TexPackHit's own comment in texture_pack.h for the full explanation of
 * each field and of `mode` -- unchanged by the move, just relocated. */
typedef struct {
    float atlas_x, atlas_y;
    float org_u, org_v;
    float scale;
    int   mode;
} GpuTexpackHit;

typedef struct {
    void (*set_vram)(const uint16_t *vram);
    void (*on_upload)(int x, int y, int w, int h, const uint16_t *data);
    /* Same parameter shape as texpack_on_draw() -- see texture_pack.h for
     * what base_x/base_y/depth/clut_x/clut_y/lim/twin/dst each mean. */
    int  (*on_draw)(int base_x, int base_y, int depth, int clut_x, int clut_y,
                    const int lim[4], const int twin[4], const int dst[4],
                    GpuTexpackHit *out);
    void (*invalidate_rect)(int x, int y, int w, int h);
    void (*note_copy)(int sx, int sy, int w, int h, int dx, int dy);
    int  (*atlas_dim)(void);
    int  (*take_pending)(int *x, int *y, int *w, int *h, const uint8_t **rgba);
    void (*debug_note_prim)(int rawtex, const float col[3]);
} GpuTexpackHooks;

/* Registration, called by the title (once, from a PSX_MOD_CONSTRUCTOR).
 * Any member left NULL simply never fires -- a title need not implement
 * all of them (though the real texture_pack.c does). */
void gpu_texpack_set_hooks(const GpuTexpackHooks *hooks);

/* Called by the runtime. Every one of these is safe to call whether or not
 * a title has registered hooks: on_draw/take_pending report "no
 * replacement" (0) and atlas_dim reports 0 (no atlas) when nothing is
 * registered; the rest are no-ops. */
void gpu_texpack_set_vram(const uint16_t *vram);
void gpu_texpack_on_upload(int x, int y, int w, int h, const uint16_t *data);
int  gpu_texpack_on_draw(int base_x, int base_y, int depth, int clut_x, int clut_y,
                         const int lim[4], const int twin[4], const int dst[4],
                         GpuTexpackHit *out);
void gpu_texpack_invalidate_rect(int x, int y, int w, int h);
void gpu_texpack_note_copy(int sx, int sy, int w, int h, int dx, int dy);
int  gpu_texpack_atlas_dim(void);
int  gpu_texpack_take_pending(int *x, int *y, int *w, int *h, const uint8_t **rgba);
void gpu_texpack_debug_note_prim(int rawtex, const float col[3]);

#ifdef __cplusplus
}
#endif

#endif /* GPU_TEXPACK_HOOKS_H */
