/* Runtime cadence probe -- see psx_runtime_perf.h.
 *
 * Opt-in via PSX_RUNTIME_PERF_DIAG. Unlike the TCP debug build this adds no
 * per-block or per-instruction recording, which is what lets it ship in a
 * release binary: with the probe off the whole file costs one predictable
 * branch per vblank, so a player reporting stutter can be asked to set an
 * environment variable rather than run a special build.
 */

#include "psx_runtime_perf.h"

#include "psx_sdl.h"
#include "audio_trace.h"
#include "dirty_ram_interp.h"
#include "gpu_gl_renderer.h"
#include "overlay_loader.h"
#include "overlay_capture.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

/* Defined elsewhere and sampled here for the report: the frame counter and the
 * dirty-interp totals come from other translation units, and the audio health
 * belongs to main.cpp's output state (it is also the audio_stats TCP surface). */
extern "C" {
    extern uint64_t s_frame_count;
    int psx_audio_out_stats(double *fill_ms, double *target_ms,
                            uint64_t *underruns, uint64_t *overflow_drops,
                            double *correction, int *legacy, int *host_rate);
}

/* Lightweight production-safe cadence probe. Unlike the TCP debug build this
 * adds no per-block or per-instruction recording. All counter/timer work below
 * is opt-in via PSX_RUNTIME_PERF_DIAG; the normal Release path pays only the
 * existing once-per-vblank disabled checks. */
struct RuntimePerfSnapshot {
    uint64_t counter = 0;
    uint64_t frame = 0;
    uint64_t guest_work_ticks = 0;
    uint64_t pacer_ticks = 0;
    uint64_t autocapture_ticks = 0;
    uint64_t provider_poll_ticks = 0;
    uint64_t dirty_insns = 0;
    uint64_t dirty_dispatches = 0;
    uint32_t overlay_loads = 0;
    uint32_t overlay_invalidations = 0;
    uint32_t overlay_unregistered = 0;
    uint64_t overlay_native = 0;
    uint64_t overlay_interp = 0;
    uint64_t overlay_stale = 0;
    uint32_t overlay_revalidations = 0;
    uint32_t overlay_hot_native_pc = 0;
    uint64_t overlay_hot_native_calls = 0;
    uint64_t overlay_shadow_calls = 0;
    uint64_t overlay_shadow_divergences = 0;
    uint32_t overlay_first_divergence_pc = 0;
    uint32_t capture_triggers = 0;
    uint64_t capture_last_dispatch_delta = 0;
    int capture_overlays = 0;
};

struct RuntimePerfState {
    bool initialized = false;
    bool enabled = false;
    uint32_t interval_ms = 5000;
    uint64_t frequency = 0;
    uint64_t last_frame_exit_counter = 0;
    uint64_t guest_work_ticks = 0;
    uint64_t pacer_ticks = 0;
    uint64_t autocapture_ticks = 0;
    uint64_t provider_poll_ticks = 0;
    bool bench_configured = false;
    bool bench_started = false;
    bool bench_reported = false;
    uint64_t bench_start_frame = 0;
    uint64_t bench_end_frame = 0;
    RuntimePerfSnapshot bench_start;
};

static RuntimePerfState g_runtime_perf;

static bool runtime_perf_parse_window(const char *text, uint64_t *start,
                                      uint64_t *end) {
    if (!text || !text[0]) return false;
    char *middle = nullptr;
    unsigned long long first = std::strtoull(text, &middle, 10);
    if (middle == text || *middle != ':') return false;
    char *tail = nullptr;
    unsigned long long last = std::strtoull(middle + 1, &tail, 10);
    if (tail == middle + 1 || *tail != '\0' || first == 0 || last <= first)
        return false;
    *start = (uint64_t)first;
    *end = (uint64_t)last;
    return true;
}

static void runtime_perf_init() {
    if (g_runtime_perf.initialized) return;
    g_runtime_perf.initialized = true;
    const char *enabled = std::getenv("PSX_RUNTIME_PERF_DIAG");
    g_runtime_perf.enabled = enabled && enabled[0] && enabled[0] != '0';
    if (!g_runtime_perf.enabled) return;

    g_runtime_perf.frequency = SDL_GetPerformanceFrequency();
    if (!g_runtime_perf.frequency) g_runtime_perf.frequency = 1;
    if (const char *interval = std::getenv("PSX_RUNTIME_PERF_DIAG_MS")) {
        char *tail = nullptr;
        long requested = std::strtol(interval, &tail, 10);
        if (tail != interval && *tail == '\0') {
            if (requested < 250) requested = 250;
            if (requested > 600000) requested = 600000;
            g_runtime_perf.interval_ms = (uint32_t)requested;
        } else {
            std::fprintf(stderr,
                "psxrecomp: ignoring invalid PSX_RUNTIME_PERF_DIAG_MS='%s'\n",
                interval);
        }
    }
    if (const char *window = std::getenv("PSX_BENCH_WINDOW")) {
        if (runtime_perf_parse_window(window,
                                      &g_runtime_perf.bench_start_frame,
                                      &g_runtime_perf.bench_end_frame)) {
            g_runtime_perf.bench_configured = true;
        } else {
            std::fprintf(stderr,
                "psxrecomp: ignoring invalid PSX_BENCH_WINDOW='%s' "
                "(expected start:end, start >= 1, end > start)\n", window);
        }
    }
    std::fprintf(stdout, "psxrecomp: runtime perf diagnostics enabled: interval=%u ms",
                 g_runtime_perf.interval_ms);
    if (g_runtime_perf.bench_configured)
        std::fprintf(stdout, ", bench=%llu:%llu",
                     (unsigned long long)g_runtime_perf.bench_start_frame,
                     (unsigned long long)g_runtime_perf.bench_end_frame);
    std::fprintf(stdout, "\n");
    std::fflush(stdout);
}

static RuntimePerfSnapshot runtime_perf_snapshot(uint64_t now) {
    RuntimePerfSnapshot s;
    s.counter = now;
    s.frame = s_frame_count;
    s.guest_work_ticks = g_runtime_perf.guest_work_ticks;
    s.pacer_ticks = g_runtime_perf.pacer_ticks;
    s.autocapture_ticks = g_runtime_perf.autocapture_ticks;
    s.provider_poll_ticks = g_runtime_perf.provider_poll_ticks;
    s.dirty_insns = g_dirty_ram_insns_run;
    s.dirty_dispatches = g_dirty_window_dispatches;
    overlay_loader_get_counters(&s.overlay_loads, &s.overlay_invalidations,
                                &s.overlay_unregistered, &s.overlay_native,
                                &s.overlay_interp, &s.overlay_stale,
                                nullptr, nullptr, nullptr, nullptr,
                                &s.overlay_revalidations);
    overlay_loader_take_hot_native(&s.overlay_hot_native_pc,
                                   &s.overlay_hot_native_calls);
    overlay_loader_get_shadow_summary(&s.overlay_shadow_calls,
                                      &s.overlay_shadow_divergences,
                                      &s.overlay_first_divergence_pc);
    int capture_enabled = 0;
    overlay_autocapture_get_status(&capture_enabled, &s.capture_triggers,
                                   &s.capture_last_dispatch_delta);
    s.capture_overlays = overlay_capture_count();
    return s;
}

static double runtime_perf_ticks_ms(uint64_t ticks) {
    return (double)ticks * 1000.0 / (double)g_runtime_perf.frequency;
}

static void runtime_perf_bench_tick(uint64_t now) {
    if (!g_runtime_perf.bench_configured || g_runtime_perf.bench_reported) return;
    if (!g_runtime_perf.bench_started) {
        if (s_frame_count == g_runtime_perf.bench_start_frame) {
            g_runtime_perf.bench_start = runtime_perf_snapshot(now);
            g_runtime_perf.bench_started = true;
        }
        return;
    }
    if (s_frame_count != g_runtime_perf.bench_end_frame) return;

    RuntimePerfSnapshot end = runtime_perf_snapshot(now);
    const RuntimePerfSnapshot &start = g_runtime_perf.bench_start;
    std::fprintf(stdout,
        "[BENCH] window=%llu:%llu frames=%llu wall_ms=%.3f "
        "guest_work_ms=%.3f pacer_ms=%.3f autocapture_main_ms=%.3f "
        "provider_poll_ms=%.3f "
        "dirty_insns=+%llu dirty_dispatches=+%llu "
        "overlay_native=+%llu overlay_interp=+%llu overlay_loads=+%u "
        "overlay_invalidations=+%u overlay_unregistered=+%u "
        "overlay_stale=+%llu overlay_revalidations=+%u "
        "capture_triggers=+%u capture_overlays=+%d "
        "capture_last_dispatch_delta=%llu\n",
        (unsigned long long)start.frame, (unsigned long long)end.frame,
        (unsigned long long)(end.frame - start.frame),
        runtime_perf_ticks_ms(end.counter - start.counter),
        runtime_perf_ticks_ms(end.guest_work_ticks - start.guest_work_ticks),
        runtime_perf_ticks_ms(end.pacer_ticks - start.pacer_ticks),
        runtime_perf_ticks_ms(end.autocapture_ticks - start.autocapture_ticks),
        runtime_perf_ticks_ms(end.provider_poll_ticks - start.provider_poll_ticks),
        (unsigned long long)(end.dirty_insns - start.dirty_insns),
        (unsigned long long)(end.dirty_dispatches - start.dirty_dispatches),
        (unsigned long long)(end.overlay_native - start.overlay_native),
        (unsigned long long)(end.overlay_interp - start.overlay_interp),
        end.overlay_loads - start.overlay_loads,
        end.overlay_invalidations - start.overlay_invalidations,
        end.overlay_unregistered - start.overlay_unregistered,
        (unsigned long long)(end.overlay_stale - start.overlay_stale),
        end.overlay_revalidations - start.overlay_revalidations,
        end.capture_triggers - start.capture_triggers,
        end.capture_overlays - start.capture_overlays,
        (unsigned long long)end.capture_last_dispatch_delta);
    std::fflush(stdout);
    g_runtime_perf.bench_reported = true;
}

void runtime_perf_frame_begin() {
    runtime_perf_init();
    if (!g_runtime_perf.enabled) return;
    uint64_t now = SDL_GetPerformanceCounter();
    if (g_runtime_perf.last_frame_exit_counter &&
        now >= g_runtime_perf.last_frame_exit_counter) {
        g_runtime_perf.guest_work_ticks +=
            now - g_runtime_perf.last_frame_exit_counter;
    }
    runtime_perf_bench_tick(now);
}

void runtime_perf_frame_end() {
    if (!g_runtime_perf.enabled) return;
    g_runtime_perf.last_frame_exit_counter = SDL_GetPerformanceCounter();
}

uint64_t runtime_perf_section_begin() {
    return g_runtime_perf.enabled ? SDL_GetPerformanceCounter() : 0;
}

void runtime_perf_section_end(uint64_t start, int section) {
    if (!start) return;
    uint64_t *total;
    switch (section) {
        case PSX_PERF_PACER:         total = &g_runtime_perf.pacer_ticks; break;
        case PSX_PERF_AUTOCAPTURE:   total = &g_runtime_perf.autocapture_ticks; break;
        case PSX_PERF_PROVIDER_POLL: total = &g_runtime_perf.provider_poll_ticks; break;
        default: return;
    }
    uint64_t end = SDL_GetPerformanceCounter();
    if (end >= start) *total += end - start;
}

void runtime_perf_diag_tick() {
    static bool have_last = false;
    static RuntimePerfSnapshot last;
    static uint64_t last_spu = 0, last_underruns = 0, last_overflows = 0;
    static uint64_t last_up[6] = {0};
    static uint64_t last_overlay_load_us = 0;
    if (!g_runtime_perf.enabled) return;

    uint64_t now = SDL_GetPerformanceCounter();
    const uint64_t interval_ticks =
        g_runtime_perf.frequency * (uint64_t)g_runtime_perf.interval_ms / 1000u;
    if (have_last && now - last.counter < interval_ticks) return;

    RuntimePerfSnapshot current = runtime_perf_snapshot(now);
    AudioTraceStats audio;
    audio_trace_get_stats(&audio);
    double fill_ms = 0.0, target_ms = 0.0, correction = 0.0;
    uint64_t underruns = 0, overflows = 0;
    int legacy = 0, host_rate = 0;
    psx_audio_out_stats(&fill_ms, &target_ms, &underruns, &overflows, &correction,
                        &legacy, &host_rate);
    (void)target_ms;
    uint64_t up[6] = {0};
    gl_renderer_runtime_diag(up);
    uint64_t overlay_load_us = 0, overlay_load_max_us = 0, overlay_load_last_us = 0;
    overlay_loader_get_load_timing(&overlay_load_us, &overlay_load_max_us,
                                   &overlay_load_last_us);
    if (!have_last) {
        last = current;
        last_spu = audio.tap_frames[AUDIO_TAP_SPU_OUT];
        last_underruns = underruns;
        last_overflows = overflows;
        last_overlay_load_us = overlay_load_us;
        for (int i = 0; i < 6; i++) last_up[i] = up[i];
        have_last = true;
        return;
    }

    const double dt = (double)(current.counter - last.counter) /
                      (double)g_runtime_perf.frequency;
    std::fprintf(stdout,
        "psxrecomp: runtime cadence: guest=%.2f Hz, spu=%.1f Hz, "
        "audio_fill=%.1f ms, underruns=+%llu, overflows=+%llu, corr=%+.5f; "
        "GL upload=%.1f calls/s %.1f rect/s %.2f Mpix/s, "
        "cpu=%.1f tex=%.1f draw=%.1f ms/s; "
        "work guest=%.1f pacer=%.1f autocapture=%.1f provider_poll=%.1f ms/s, "
        "dirty=%.0f insn/s %.0f dispatch/s; "
        "overlay native=+%llu interp=+%llu hot_native=0x%08X/+%llu "
        "shadow=+%llu div=+%llu first_div=0x%08X "
        "loads=+%u revalidations=+%u "
        "load_wall=%.1f ms max=%.1f last=%.1f ms; "
        "capture triggers=+%u overlays=+%d last_dispatch_delta=%llu\n",
        (double)(current.frame - last.frame) / dt,
        (double)(audio.tap_frames[AUDIO_TAP_SPU_OUT] - last_spu) / dt,
        fill_ms, (unsigned long long)(underruns - last_underruns),
        (unsigned long long)(overflows - last_overflows), correction,
        (double)(up[0] - last_up[0]) / dt,
        (double)(up[1] - last_up[1]) / dt,
        (double)(up[2] - last_up[2]) / dt / 1.0e6,
        (double)(up[3] - last_up[3]) * 1000.0 /
            (double)g_runtime_perf.frequency / dt,
        (double)(up[4] - last_up[4]) * 1000.0 /
            (double)g_runtime_perf.frequency / dt,
        (double)(up[5] - last_up[5]) * 1000.0 /
            (double)g_runtime_perf.frequency / dt,
        runtime_perf_ticks_ms(current.guest_work_ticks - last.guest_work_ticks) / dt,
        runtime_perf_ticks_ms(current.pacer_ticks - last.pacer_ticks) / dt,
        runtime_perf_ticks_ms(current.autocapture_ticks - last.autocapture_ticks) / dt,
        runtime_perf_ticks_ms(current.provider_poll_ticks - last.provider_poll_ticks) / dt,
        (double)(current.dirty_insns - last.dirty_insns) / dt,
        (double)(current.dirty_dispatches - last.dirty_dispatches) / dt,
        (unsigned long long)(current.overlay_native - last.overlay_native),
        (unsigned long long)(current.overlay_interp - last.overlay_interp),
        current.overlay_hot_native_pc,
        (unsigned long long)current.overlay_hot_native_calls,
        (unsigned long long)(current.overlay_shadow_calls - last.overlay_shadow_calls),
        (unsigned long long)(current.overlay_shadow_divergences -
                             last.overlay_shadow_divergences),
        current.overlay_first_divergence_pc,
        current.overlay_loads - last.overlay_loads,
        current.overlay_revalidations - last.overlay_revalidations,
        (double)(overlay_load_us - last_overlay_load_us) / 1000.0,
        (double)overlay_load_max_us / 1000.0,
        (double)overlay_load_last_us / 1000.0,
        current.capture_triggers - last.capture_triggers,
        current.capture_overlays - last.capture_overlays,
        (unsigned long long)current.capture_last_dispatch_delta);
    std::fflush(stdout);
    last = current;
    last_spu = audio.tap_frames[AUDIO_TAP_SPU_OUT];
    last_underruns = underruns;
    last_overflows = overflows;
    last_overlay_load_us = overlay_load_us;
    for (int i = 0; i < 6; i++) last_up[i] = up[i];
}
