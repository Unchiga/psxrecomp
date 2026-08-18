/* Host audio output -- see psx_host_audio.h.
 *
 * Three layers meet here. spu.c renders at the PSX's 44.1 kHz; psx_sdl_audio is
 * the SDL2/SDL3 compatibility shim below; this file is the part in between that
 * decides what actually reaches the device -- the resampling/rate-control
 * bridge, the pump that feeds it, and the gate that mutes or discards output
 * during turbo loads.
 *
 * The gate is the subtle part. sdl_audio_update() is the sole authority on
 * whether a pump emits, discards, or does not run, and the mid-frame pump
 * mirrors its last decision rather than deciding for itself -- pumping
 * unconditionally would push real audio through a turbo-load mute and defeat
 * the whole model, which depends on voice positions freezing so music resumes
 * where it left off instead of replaying time-compressed.
 */

#include "psx_host_audio.h"

#include "psx_sdl.h"
#include "psx_sdl_audio.h"
#include "audio_trace.h"
#include "host_osd.h"
#include "psx_netplay.h"
#include "recomp_audio_drc.h"
#include "spu.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Guest time moved discontinuously; the pump rebases instead of reporting the
 * gap as underruns. Owned here, poked through the two reset entry points. */
static int g_audio_cycle_resync = 0;
static int sdl_audio_fadein_left = 0;

/* Turbo-load observability, defined with the other turbo counters in main.cpp
 * and read by the debug server; incremented here because this is where the
 * sink actually discards frames. */
extern uint64_t g_turbo_audio_sink_frames;
extern int      g_turbo_audio_sink_active;

static SDL_AudioDeviceID sdl_audio_device;
static int16_t       sdl_audio_buf[2048 * 2];

/* DRC bridge. Producer (sdl_audio_pump) runs on the main loop thread under
 * SDL_LockAudioDevice; consumer (sdl_drc_callback) runs on the SDL audio
 * thread. */
static rab_bridge s_drc;
static bool       s_drc_ready = false;

/* Observability + A/B. PSXRECOMP_AUDIO_LEGACY=1 keeps the historical push model
 * (SDL_QueueAudio, no bridge) so the underrun baseline can be measured against
 * the bridge from a single build; default (unset) = bridge/pull model. Output
 * health is surfaced through the audio_stats TCP command (no stderr probe). */
static bool audio_legacy_mode(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("PSXRECOMP_AUDIO_LEGACY"); v = (e && *e && *e != '0') ? 1 : 0; }
    return v != 0;
}
/* Legacy-mode underrun counter: incremented when the SDL queue is found empty
 * at pump time (the device was silence-filling = an audible gap). */
static uint64_t g_legacy_underruns = 0;
/* Set at unmute so the pump resyncs its bridge-underrun baseline past the
 * intentional mute-drain instead of reporting it as gaps. */
int g_audio_unmute_resync = 0;
/* Actual device rate the host opened at (bridge mode may differ from 44100;
 * the T3 tap ring runs at this rate and its WAV dump must say so). */
int g_audio_host_rate = 44100;

static void sdl_drc_callback(void* user, Uint8* stream, int len) {
    (void)user;
    if (!s_drc_ready) { memset(stream, 0, (size_t)len); return; }
    int frames = len / (int)(2 * sizeof(int16_t)); /* stereo S16 */
    rab_pull(&s_drc, (int16_t*)stream, frames);
    /* T3 tap in bridge mode: the exact device-rate bytes the host consumes.
     * This callback is the tap's single writer while the bridge is active. */
    audio_trace_pcm(AUDIO_TAP_HOST, (const int16_t*)stream,
                    frames);
}
static void sdl_audio_gain_ramp(int16_t* buf, int frames, float g0, float g1) {
    if (frames <= 0) return;
    const float step = (g1 - g0) / (float)frames;
    float g = g0;
    for (int f = 0; f < frames; f++, g += step) {
        buf[f * 2 + 0] = (int16_t)((float)buf[f * 2 + 0] * g);
        buf[f * 2 + 1] = (int16_t)((float)buf[f * 2 + 1] * g);
    }
}

/* Fade-in state: samples of rising ramp still to apply after an unmute.
 * Consumed by sdl_audio_pump across however many pump calls it spans.
 * MUST fit sdl_audio_buf (2048 frames): the fade-out tail renders this many
 * frames in one spu_render call. 1764 frames = 40 ms.
 * sdl_audio_fadein_left is declared with present_session_reset. */
static const int sdl_audio_fade_samples = 44100 * 40 / 1000;  /* 40 ms */

static void sdl_audio_pump(int discard_output) {
    /* Guest-cycle SPU advance must not depend on host audio device/backpressure.
     * Win↔Linux rollback forked on aux/spu when one peer skipped spu_render
     * (queue full / !drc / no device) while the other kept advancing. */
    const uint32_t bytes_per_frame = sizeof(int16_t) * 2u;
    const bool legacy = audio_legacy_mode();
    const int netplay = psx_netplay_active();
    static int had_audio = 0;
    uint32_t queued = 0;   /* RENDER event b: bytes (legacy) / fill ms (bridge) */
    int host_queue_ok = 0;

    if (discard_output) {
        /* Host-only sink: skip all queue/bridge interaction, but continue to
         * the guest-cycle sample budget and spu_render below. */
    } else if (sdl_audio_device && legacy) {
        /* Historical push model + baseline measurement: a drained (==0) queue
         * means the device was silence-filling since the last pump = a gap.
         * Only meaningful after the first audio has been queued. */
        const uint32_t max_queue_bytes = 44100u * bytes_per_frame / 5u;
        queued = psx_sdl_audio_queued_size(sdl_audio_device);
        if (queued == 0 && had_audio) {
            g_legacy_underruns++;
            audio_trace_event(AUDIO_EV_UNDERRUN, 0, 0);
        }
        if (queued > max_queue_bytes) {
            audio_trace_event(AUDIO_EV_PUMP_SKIP, queued, 0);
            /* Still advance SPU below; only skip host enqueue. */
        } else {
            host_queue_ok = 1;
        }
    } else if (sdl_audio_device && !legacy && s_drc_ready) {
        /* Surface bridge underruns (counted on the SDL audio thread) into the
         * event ring from this thread — the event ring is single-writer.
         * Across a turbo mute the ring intentionally runs dry (the pump stops
         * while the callback keeps pulling); those dry pulls are the mute,
         * not gaps — resync past them instead of reporting them. */
        rab_stats st;
        rab_get_stats(&s_drc, &st);
        static uint64_t prev_underruns = 0;
        extern int g_audio_unmute_resync;
        if (g_audio_unmute_resync) {
            prev_underruns = st.underrun_events;
            g_audio_unmute_resync = 0;
        } else if (st.underrun_events > prev_underruns) {
            audio_trace_event(AUDIO_EV_UNDERRUN,
                              (uint32_t)(st.underrun_events - prev_underruns), 1);
            prev_underruns = st.underrun_events;
        }
        queued = (uint32_t)st.last_fill_ms;
        host_queue_ok = 1;
    }

    /* Faithful sample budget: the SPU is clocked by the GUEST, not by host
     * presents. 33.8688 MHz / 44100 Hz = exactly 768 guest cycles per output
     * frame, so production tracks guest time precisely — including the real
     * NTSC 59.94 Hz vblank — instead of assuming 60.00 Hz per present, which
     * built in a systematic -0.1% production deficit (measured: 43950/s
     * produced vs 44100/s consumed = recurring ring underruns no +/-0.5%
     * DRC trim could absorb during jitter spikes). */
    extern uint64_t psx_cycle_count;
    static uint64_t last_cycles = 0;
    static uint64_t cycle_carry = 0;
    const uint64_t now_cycles = psx_cycle_count;
    if (g_audio_cycle_resync) {
        last_cycles = now_cycles;
        cycle_carry = 0;
        g_audio_cycle_resync = 0;
        if (legacy && sdl_audio_device)
            psx_sdl_audio_clear(sdl_audio_device);
        else
            g_audio_unmute_resync = 1; /* skip mute-drain underrun reports */
        return;
    }
    if (last_cycles == 0) last_cycles = now_cycles;
    uint64_t delta = (now_cycles - last_cycles) + cycle_carry;
    last_cycles = now_cycles;
    int frames = (int)(delta / 768u);
    cycle_carry = delta % 768u;
    if (frames <= 0) return;
    if (frames > 2048 && !netplay) {
        /* Offline: a burst beyond one buffer (e.g. right after an unmute or a
         * long stall) renders one full buffer and DROPs the remainder of the
         * debt — mute-model freeze rather than time-compressing a backlog.
         * Netplay never drops: both peers must consume the same guest debt. */
        frames = 2048;
        cycle_carry = 0;
    }

    audio_trace_event(AUDIO_EV_RENDER, (uint32_t)frames, queued);

    /* Catch up in ≤2048-frame chunks (sdl_audio_buf capacity). Only the last
     * chunk may be handed to the host queue — earlier chunks advance state
     * only (avoids dumping seconds of catch-up into the device ring). */
    int remaining = frames;
    int host_frames = 0;
    while (remaining > 0) {
        const int chunk = remaining > 2048 ? 2048 : remaining;
        spu_render(sdl_audio_buf, chunk);
        remaining -= chunk;
        host_frames = chunk;
        if (discard_output) {
            g_turbo_audio_sink_frames += (uint64_t)chunk;
            audio_trace_event(AUDIO_EV_SINK_DROP, (uint32_t)chunk, 0);
        }
    }
    if (discard_output)
        return;
    if (!host_queue_ok || !sdl_audio_device)
        return;

    frames = host_frames;
    if (sdl_audio_fadein_left > 0) {
        const float g0 = 1.0f - (float)sdl_audio_fadein_left
                                / (float)sdl_audio_fade_samples;
        int ramp = sdl_audio_fadein_left < frames ? sdl_audio_fadein_left : frames;
        const float g1 = 1.0f - (float)(sdl_audio_fadein_left - ramp)
                                / (float)sdl_audio_fade_samples;
        sdl_audio_gain_ramp(sdl_audio_buf, ramp, g0, g1);
        sdl_audio_fadein_left -= ramp;
    }
    /* Host master volume (launcher / numpad +/-). Applied after fade so mute
     * edges stay continuous and volume steps take effect immediately. */
    {
        const int vol = host_volume_get();
        if (vol < 100) {
            const float g = (float)vol / 100.0f;
            sdl_audio_gain_ramp(sdl_audio_buf, frames, g, g);
        }
    }
    if (legacy) {
        /* T3 tap: the exact post-fade bytes handed to the host audio queue. */
        audio_trace_pcm(AUDIO_TAP_HOST, sdl_audio_buf, frames);
        psx_sdl_audio_queue(sdl_audio_device, sdl_audio_buf, (uint32_t)frames * bytes_per_frame);
        had_audio = 1;
    } else {
        /* Hand to the bridge (band-limited resample + DRC) instead of
         * SDL_QueueAudio. Lock guards the SPSC ring against the pull callback.
         * The T3 tap moves to sdl_drc_callback: what the device actually
         * receives is the bridge's device-rate output, not this buffer. */
        psx_sdl_audio_lock(sdl_audio_device);
        rab_push(&s_drc, sdl_audio_buf, frames);
        psx_sdl_audio_unlock(sdl_audio_device);
    }
}

/* Audio gating across turbo-loads transitions.
 *
 * The mute model stays: during turbo the guest runs at host speed, so
 * rendered SPU audio is time-compressed garble — we stop pumping, the queue
 * drains, voice positions freeze, and music resumes in place afterward.
 * What changes is the EDGES:
 *   - entering turbo: render one short tail of the current voice state,
 *     ramp it to silence, and queue it — the drain ends in a fade instead
 *     of a hard cut;
 *   - leaving turbo: hold the mute for a short hangover first (loads often
 *     re-trigger within a few frames; without the debounce the mute would
 *     flicker audibly), then resume pumping with a rising ramp applied
 *     across the first ~50 ms of samples (sdl_audio_pump above). */
/* Bridge/legacy output health, surfaced through the audio_stats TCP command
 * (debug_server.c) — no stderr probe; rule 3. */
int psx_audio_out_stats(double *fill_ms, double *target_ms,
                                   uint64_t *underruns,
                                   uint64_t *overflow_drops, double *correction,
                                   int *legacy, int *host_rate)
{
    *legacy = audio_legacy_mode() ? 1 : 0;
    *host_rate = g_audio_host_rate;
    if (*legacy || !s_drc_ready) {
        *fill_ms = sdl_audio_device
                   ? (double)psx_sdl_audio_queued_size(sdl_audio_device)
                     / (44100.0 * 4.0) * 1000.0
                   : 0.0;
        *target_ms = 0.0; /* push queue — no DRC fill target */
        *underruns = g_legacy_underruns;
        *overflow_drops = 0;
        *correction = 0.0;
        return sdl_audio_device != 0;
    }
    rab_stats st;
    rab_get_stats(&s_drc, &st);
    *fill_ms = st.last_fill_ms;
    *target_ms = s_drc.cfg.target_ms;
    *underruns = st.underrun_events;
    *overflow_drops = st.overflow_drops;
    *correction = st.last_correction;
    return 1;
}


/* Audio gate state, shared with the mid-frame (VBlank-edge) pump.
 *
 * sdl_audio_update() below is the sole authority on whether a pump should emit
 * audio, discard it, or not run at all. The mid-frame pump exists to keep SPU
 * time flowing across guest busy-waits that never present a frame, but it must
 * NOT bypass that authority: pumping unconditionally would push real audio
 * during a turbo-load hard mute, defeating the mute model (the queue is
 * supposed to drain and voice positions freeze in place, so music resumes where
 * it left off rather than replaying time-compressed garble), and would emit to
 * the device during the discard-only turbo sink.
 *
 * So the mid-frame pump mirrors whatever the last frame decided. */
typedef enum { AUDIO_GATE_NORMAL = 0, AUDIO_GATE_MUTED = 1,
               AUDIO_GATE_SINK = 2 } AudioGate;
static AudioGate s_audio_gate = AUDIO_GATE_NORMAL;

/* Invoked from the guest-derived VBlank edge (interrupts.c). */
void sdl_audio_pump_midframe(void) {
    /* Device optional: SPU still advances from guest cycles (netplay-safe). */
    switch (s_audio_gate) {
    case AUDIO_GATE_MUTED:
        /* Deliberately nothing. This preserves the existing, user-validated
         * freeze-in-place mute semantics exactly. NOTE a real tension here: the
         * SPU is autonomous on hardware and never freezes, so a game that
         * busy-waits on an SPU-generated condition *during* a turbo load would
         * still stall. No title in our suite is known to do that; recording it
         * rather than guessing a fix that would change validated mute audio. */
        return;
    case AUDIO_GATE_SINK:
        sdl_audio_pump(1);   /* advance SPU time, discard output */
        return;
    case AUDIO_GATE_NORMAL:
    default:
        sdl_audio_pump(0);
        return;
    }
}

void sdl_audio_update(int hard_mute_active, int turbo_sink_active) {
    {   /* Tag audio events with the vblank frame counter. */
        extern uint64_t s_frame_count;
        audio_trace_note_frame((uint32_t)s_frame_count);
    }
    const int HANGOVER_FRAMES = 8;  /* ~133 ms at 60 fps */
    static int muted = 0;
    static int hangover = 0;
    static int sink_was_active = 0;

    if (hard_mute_active) {
        if (!muted) {
            int tail = sdl_audio_fade_samples;
            const int buf_cap = (int)(sizeof(sdl_audio_buf) / (2 * sizeof(int16_t)));
            if (tail > buf_cap) tail = buf_cap;
            /* An unmute ramp may still be in progress; start the down-ramp
             * from its current gain so the edge stays continuous. */
            const float vol = (float)host_volume_get() / 100.0f;
            const float g0 = (1.0f - (float)sdl_audio_fadein_left
                                     / (float)sdl_audio_fade_samples) * vol;
            sdl_audio_fadein_left = 0;
            spu_render(sdl_audio_buf, tail);
            sdl_audio_gain_ramp(sdl_audio_buf, tail, g0, 0.0f);
            audio_trace_event(AUDIO_EV_MUTE, (uint32_t)tail, 0);
            if (sdl_audio_device && audio_legacy_mode()) {
                audio_trace_pcm(AUDIO_TAP_HOST, sdl_audio_buf, tail);
                psx_sdl_audio_queue(sdl_audio_device, sdl_audio_buf, (uint32_t)tail * sizeof(int16_t) * 2u);
            } else if (sdl_audio_device && s_drc_ready) {
                psx_sdl_audio_lock(sdl_audio_device);
                rab_push(&s_drc, sdl_audio_buf, tail);
                psx_sdl_audio_unlock(sdl_audio_device);
            }
            muted = 1;
        }
        s_audio_gate = AUDIO_GATE_MUTED;
        hangover = HANGOVER_FRAMES;
        return;
    }
    if (muted) {
        /* Still inside the post-mute hangover: the gate stays MUTED so the
         * mid-frame pump does not sneak audio out ahead of the unmute ramp. */
        if (hangover > 0) { hangover--; return; }
        muted = 0;
        sdl_audio_fadein_left = sdl_audio_fade_samples;
        audio_trace_event(AUDIO_EV_UNMUTE, (uint32_t)sdl_audio_fadein_left, 0);
        extern int g_audio_unmute_resync;
        g_audio_unmute_resync = 1;
    }
    if (turbo_sink_active) {
        if (!sink_was_active) {
            sink_was_active = 1;
            audio_trace_event(AUDIO_EV_MUTE, 0, 2); /* b=2: discard-only sink */
        }
        g_turbo_audio_sink_active = 1;
        s_audio_gate = AUDIO_GATE_SINK;
        sdl_audio_pump(1);
        return;
    }
    g_turbo_audio_sink_active = 0;
    if (sink_was_active) {
        sink_was_active = 0;
        sdl_audio_fadein_left = sdl_audio_fade_samples;
        audio_trace_event(AUDIO_EV_UNMUTE,
                          (uint32_t)sdl_audio_fadein_left, 2);
        g_audio_unmute_resync = 1;
    }
    s_audio_gate = AUDIO_GATE_NORMAL;
    sdl_audio_pump(0);
}

/* Open the host device and, unless legacy push mode is selected, the resampling
 * bridge behind it. Returns the rate actually opened, or 0 if the device did
 * not open -- callers carry on regardless, because SPU advance is budgeted in
 * guest cycles and must not depend on a working host device. */
int psx_host_audio_open(int want_freq) {
    PsxSdlAudioSpec want, have;
    int legacy;
    memset(&want, 0, sizeof(want));
    memset(&have, 0, sizeof(have));
    want.freq = want_freq;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    legacy = audio_legacy_mode() ? 1 : 0;
    want.allow_frequency_change = legacy ? 0 : 1;
    if (!legacy)
        want.callback = sdl_drc_callback;  /* pull model: bridge resamples + DRC */
    sdl_audio_device = psx_sdl_audio_open(&want, &have);
    if (!sdl_audio_device)
        return 0;
    if (!legacy) {
        rab_config cfg;
        rab_config_defaults(&cfg);
        cfg.channels    = 2;
        cfg.source_rate = 44100.0;            /* SPU render rate */
        cfg.host_rate   = (double)have.freq;  /* actual device rate */
        if (rab_init(&s_drc, &cfg) == 0) s_drc_ready = true;
    }
    g_audio_host_rate = have.freq;
    audio_trace_set_tap_rate(AUDIO_TAP_HOST, (uint32_t)have.freq);
    (void)psx_sdl_audio_resume(sdl_audio_device);
    return have.freq;
}

/* Stop the pull callback before freeing what it reads. Safe to call twice and
 * safe when nothing opened. */
void psx_host_audio_close(void) {
    if (sdl_audio_device) {
        psx_sdl_audio_clear(sdl_audio_device);
        psx_sdl_audio_close(sdl_audio_device);
        sdl_audio_device = 0;
    }
    if (s_drc_ready) { rab_free(&s_drc); s_drc_ready = false; }
}

/* A savestate load or a session reset moves guest time discontinuously; the
 * pump rebases its cycle accounting on the next call rather than emitting the
 * gap as underruns. */
void psx_host_audio_request_resync(void) { g_audio_cycle_resync = 1; }
void psx_host_audio_reset_fadein(void)   { sdl_audio_fadein_left = 0; }
