/* Host audio output: the SDL device, the resampling/rate-control bridge that
 * feeds it, and the gate that mutes or discards output during turbo loads.
 *
 * Sits between spu.c (which renders at 44.1 kHz) and psx_sdl_audio (the
 * SDL2/SDL3 compatibility shim). */
#ifndef PSX_HOST_AUDIO_H
#define PSX_HOST_AUDIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the rate the device actually opened at, or 0 on failure. Failure is
 * not fatal and callers should continue: SPU advance is budgeted in guest
 * cycles, so the emulated machine runs identically with no host device. */
int  psx_host_audio_open(int want_freq);
void psx_host_audio_close(void);

/* Per-frame gate + pump. hard_mute_active and turbo_sink_active come from the
 * frontend's load-acceleration state, which this module cannot see. */
void sdl_audio_update(int hard_mute_active, int turbo_sink_active);

/* Guest-VBlank-edge pump, registered with psx_set_midframe_audio_pump so SPU
 * time keeps flowing across guest busy-waits that never present a frame.
 * Mirrors the gate decision from the last sdl_audio_update(). */
void sdl_audio_pump_midframe(void);

/* Guest time moved discontinuously (savestate load, session reset): rebase the
 * pump's cycle accounting instead of reporting the gap as underruns. */
void psx_host_audio_request_resync(void);
void psx_host_audio_reset_fadein(void);

/* Output health for the audio_stats TCP command. */
int psx_audio_out_stats(double *fill_ms, double *target_ms,
                        uint64_t *underruns, uint64_t *overflow_drops,
                        double *correction, int *legacy, int *host_rate);

/* Rate the device opened at; the T3 tap ring runs at this rate. */
extern int g_audio_host_rate;

#ifdef __cplusplus
}
#endif

#endif /* PSX_HOST_AUDIO_H */
