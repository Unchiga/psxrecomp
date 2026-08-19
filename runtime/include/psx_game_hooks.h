/* psx_game_hooks.h — a title's own start-up and per-frame work.
 *
 * The runtime carries no game-specific code, so it cannot name a title's
 * functions. A title registers what it needs to run instead, from a
 * PSX_MOD_CONSTRUCTOR in its own sources (see mod_plugins.h), and the runtime
 * calls whatever was registered at the two points that matter.
 *
 * This is deliberately NOT the mod-plugin API next door. Those callbacks fire
 * only when a .psxmod package selects them by id, which is right for optional,
 * player-toggleable packages and wrong for a feature that is simply part of
 * the build.
 *
 * Registration happens during static initialisation, so hooks are in place
 * before main() runs and before any settings file is read.
 */
#ifndef PSX_GAME_HOOKS_H
#define PSX_GAME_HOOKS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*PsxGameHook)(void);

/* Runs once, after the guest and its devices are up and the game may be
 * touched. Use for anything that has to arm against a running machine. */
int psx_game_add_start_hook(PsxGameHook fn);

/* Runs once per presented frame, on the emulator thread. Keep it cheap: a
 * hook that does nothing when its feature is off costs a predictable call. */
int psx_game_add_frame_hook(PsxGameHook fn);

/* Runs once per simulated vblank, on the emulator thread, before the debug
 * server records the frame. This is the guest's own cadence, not the host
 * window's: a sampler that wants its entries stamped with the same frame
 * number the display and trace rings use must run here, not in a frame hook.
 * Read that number with debug_server_frame_number(). */
int psx_game_add_vblank_hook(PsxGameHook fn);

/* Called by the runtime. Not for titles. */
void psx_game_run_start_hooks(void);
void psx_game_run_frame_hooks(void);
void psx_game_run_vblank_hooks(void);

#ifdef __cplusplus
}
#endif

#endif /* PSX_GAME_HOOKS_H */
