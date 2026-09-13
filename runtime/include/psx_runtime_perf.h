/* Runtime cadence probe: a production-safe view of where a frame's wall clock
 * goes -- guest work, the pacer, overlay autocapture, the code-provider poll --
 * next to the audio, GL upload and dirty-interp counters for the same window.
 *
 * PSX_RUNTIME_PERF_DIAG enables it, PSX_RUNTIME_PERF_DIAG_MS sets the report
 * interval, and PSX_RUNTIME_PERF_BENCH=first:last reports once over a frame
 * window instead of continuously. */
#ifndef PSX_RUNTIME_PERF_H
#define PSX_RUNTIME_PERF_H

#include <stdint.h>

/* Frame boundaries. _begin also performs the one-time environment read, so
 * there is no separate init to forget. */
void runtime_perf_frame_begin();
void runtime_perf_frame_end();

/* Emit a report if the interval has elapsed; a cheap no-op when disabled. */
void runtime_perf_diag_tick();

/* Time one section of the frame. _begin returns 0 when the probe is off and
 * _end treats 0 as "not timing", so callers need no conditionals of their own.
 *
 * The section is NAMED rather than passed as a pointer into the probe's state.
 * Callers used to hand over &g_runtime_perf.pacer_ticks, which put the struct's
 * layout in every caller's hands and was the only thing forcing that state to
 * be visible outside this module. */
enum {
    PSX_PERF_PACER = 0,        /* wall-clock frame pacing waits */
    PSX_PERF_AUTOCAPTURE,      /* overlay autocapture */
    PSX_PERF_PROVIDER_POLL     /* code-provider background poll */
};
uint64_t runtime_perf_section_begin();
void     runtime_perf_section_end(uint64_t start, int section);

#endif /* PSX_RUNTIME_PERF_H */
