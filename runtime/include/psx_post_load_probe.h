/* Post-load freeze probe: a dense per-vblank cross-section of the machine for a
 * few hundred frames after a save-state restore.
 *
 * Opt-in with PSX_POST_LOAD_PROBE=1; inert otherwise, so it costs one branch
 * per vblank in a normal run. Reports to stderr rather than the debug server
 * because it has to survive the case it was built for -- a guest that stops
 * responding -- and because the interesting output is the stall-PC histogram
 * it prints once, not a state anyone can poll for afterwards. */
#ifndef PSX_POST_LOAD_PROBE_H
#define PSX_POST_LOAD_PROBE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Start a probe window. Called after a successful save-state restore; a no-op
 * unless the environment variable is set. */
void post_load_probe_arm(void);

/* One vblank's sample. turbo_active / present_reached come from the frontend
 * because the probe cannot see them: they are properties of the host's pacing
 * decision for this frame, not of the emulated machine. */
void post_load_probe_on_vblank(int turbo_active, int present_reached);

#ifdef __cplusplus
}
#endif

#endif /* PSX_POST_LOAD_PROBE_H */
