/* Post-load freeze probe -- see psx_post_load_probe.h.
 *
 * Armed by PSX_POST_LOAD_PROBE=1 and off otherwise. It exists because a
 * save-state restore hands the guest a fully-formed machine mid-frame, and when
 * that went wrong the symptom was a hang with nothing to look at: the guest
 * parked in a BIOS spin with every subsystem individually looking fine. So for
 * a few hundred vblanks after a load this records the whole cross-section --
 * live PC and return address, the interrupt path, CD state, vblank
 * raise/deliver/ack, dirty-interp counters, GPU rect state -- and reports the
 * PCs a stall settled on, which is the part that actually names the culprit.
 */

#include "psx_post_load_probe.h"

#include "psx_sdl.h"
#include "cdrom.h"
#include "cpu_state.h"
#include "dirty_ram_interp.h"
#include "gpu.h"
#include "gpu_gl_renderer.h"
#include "interrupts.h"
#include "load_accel.h"
#include "overlay_loader.h"
#include "psx_cycles.h"
#include "psx_scheduler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* After LOADED: optional freeze probe (PSX_POST_LOAD_PROBE=1). Off by default. */
static int      s_post_load_probe_enabled = -1; /* -1 = unread env */
static int      s_post_load_probe_left = 0;
static int      s_post_load_probe_i = 0;
static uint64_t s_post_load_probe_gp00 = 0;
static uint64_t s_post_load_probe_skip_tot = 0;
static uint64_t s_post_load_probe_swap_tot = 0;
static uint64_t s_post_load_probe_dirty_tot = 0;
static uint64_t s_post_load_probe_gp0_tot = 0;
static uint64_t s_post_load_probe_vb_raise0 = 0;
static uint64_t s_post_load_probe_vb_deliv0 = 0;
static uint64_t s_post_load_probe_vb_ack0 = 0;
static uint64_t s_post_load_probe_dirty_blks0 = 0;
static uint64_t s_post_load_probe_chk_entry0 = 0;
static uint64_t s_post_load_probe_chk_fsr0 = 0;
static uint64_t s_post_load_probe_chk_fnone0 = 0;
static uint64_t s_post_load_probe_chk_mid0 = 0;
static uint64_t s_post_load_probe_chk_eval0 = 0;
static uint64_t s_post_load_probe_chk_deliv0 = 0;
static uint64_t s_post_load_probe_cyc0 = 0;
static uint64_t s_post_load_probe_ci_unit0 = 0;
static uint64_t s_post_load_probe_ci_supp0 = 0;
static uint64_t s_post_load_probe_ci_none0 = 0;
static uint64_t s_post_load_probe_ci_sr0 = 0;
static uint64_t s_post_load_probe_ci_deliv0 = 0;
static uint64_t s_post_load_probe_ci_enter0 = 0;
static uint64_t s_post_load_probe_adv_calls0 = 0;
static uint64_t s_post_load_probe_adv_sum0 = 0;
static uint64_t s_post_load_probe_svc0 = 0;
static uint64_t s_post_load_probe_dirty_insns0 = 0;
static uint64_t s_post_load_probe_dirty_pump0 = 0;
static uint64_t s_post_load_probe_stores0 = 0;
static uint64_t s_post_load_probe_idle_n0 = 0;
static uint64_t s_post_load_probe_idle_cyc0 = 0;
static uint64_t s_post_load_probe_hz_hits0 = 0;
static uint64_t s_post_load_probe_hz_cyc0 = 0;
static Uint64   s_post_load_probe_host_t0 = 0;
static int      s_post_load_probe_stall_run = 0;
static int      s_post_load_probe_in_stall = 0;
#define POST_LOAD_STALL_PC_CAP 8
static uint32_t s_stall_pc[POST_LOAD_STALL_PC_CAP];
static uint32_t s_stall_pc_n[POST_LOAD_STALL_PC_CAP];
static int      s_stall_pc_used = 0;
#define POST_LOAD_LIVE_PC_CAP 8
static uint32_t s_live_pc[POST_LOAD_LIVE_PC_CAP];
static uint32_t s_live_pc_n[POST_LOAD_LIVE_PC_CAP];
static int      s_live_pc_used = 0;
static int post_load_probe_env_on(void) {
    if (s_post_load_probe_enabled < 0) {
        const char *e = getenv("PSX_POST_LOAD_PROBE");
        s_post_load_probe_enabled = (e && e[0] == '1') ? 1 : 0;
    }
    return s_post_load_probe_enabled;
}
static void post_load_probe_stall_pc_note(uint32_t pc) {
    if (!pc) return;
    for (int i = 0; i < s_stall_pc_used; i++) {
        if (s_stall_pc[i] == pc) {
            if (s_stall_pc_n[i] < 0xffffffffu) s_stall_pc_n[i]++;
            return;
        }
    }
    if (s_stall_pc_used >= POST_LOAD_STALL_PC_CAP) return;
    s_stall_pc[s_stall_pc_used] = pc;
    s_stall_pc_n[s_stall_pc_used] = 1;
    s_stall_pc_used++;
}

static void post_load_probe_stall_pc_dump(const char *why) {
    if (s_stall_pc_used <= 0) return;
    fprintf(stderr, "post_load_probe stall_pcs (%s):", why);
    for (int i = 0; i < s_stall_pc_used; i++) {
        fprintf(stderr, " 0x%08X×%u",
                     (unsigned)s_stall_pc[i], (unsigned)s_stall_pc_n[i]);
    }
    fprintf(stderr, "\n");
}

static void post_load_probe_live_pc_note(uint32_t pc) {
    if (!pc) return;
    for (int i = 0; i < s_live_pc_used; i++) {
        if (s_live_pc[i] == pc) {
            if (s_live_pc_n[i] < 0xffffffffu) s_live_pc_n[i]++;
            return;
        }
    }
    if (s_live_pc_used >= POST_LOAD_LIVE_PC_CAP) return;
    s_live_pc[s_live_pc_used] = pc;
    s_live_pc_n[s_live_pc_used] = 1;
    s_live_pc_used++;
}

static void post_load_probe_live_pc_dump(const char *why) {
    if (s_live_pc_used <= 0) return;
    fprintf(stderr, "post_load_probe live_pcs (%s):", why);
    for (int i = 0; i < s_live_pc_used; i++) {
        fprintf(stderr, " 0x%08X×%u",
                     (unsigned)s_live_pc[i], (unsigned)s_live_pc_n[i]);
    }
    fprintf(stderr, "\n");
}

void post_load_probe_arm(void) {
    if (!post_load_probe_env_on()) {
        s_post_load_probe_left = 0;
        g_plp_cycle_diag = 0;
        return;
    }
    extern uint64_t g_vblank_raise_count, g_vblank_deliver_count, g_vblank_ack_count;
    extern uint64_t g_dirty_ram_blocks_run;
    extern uint64_t g_dirty_ram_insns_run;
    extern uint64_t g_dirty_pump_count;
    extern uint64_t g_guest_store_count;
    uint64_t e = 0, fsr = 0, fn = 0, mid = 0, ev = 0, id = 0;
    psx_interrupt_check_path_diag(&e, &fsr, &fn, &mid, &ev, &id);
    uint64_t ci_u = 0, ci_s = 0, ci_n = 0, ci_sr = 0, ci_d = 0, ci_e = 0;
    overlay_loader_get_ci_skip_diag(&ci_u, &ci_s, &ci_n, &ci_sr, &ci_d, &ci_e);
    uint64_t hz_h = 0, hz_c = 0;
    psx_vsync_query_hle_horizon_totals(&hz_h, &hz_c);
    g_plp_cycle_diag = 1;
    g_plp_adv_calls = 0;
    g_plp_adv_max_chunk = 0;
    g_plp_adv_sum = 0;
    g_plp_svc_calls = 0;
    s_post_load_probe_left = 300;
    s_post_load_probe_i = 0;
    s_post_load_probe_gp00 = gpu_get_gp0_count();
    s_post_load_probe_skip_tot = 0;
    s_post_load_probe_swap_tot = 0;
    s_post_load_probe_dirty_tot = 0;
    s_post_load_probe_gp0_tot = 0;
    s_post_load_probe_vb_raise0 = g_vblank_raise_count;
    s_post_load_probe_vb_deliv0 = g_vblank_deliver_count;
    s_post_load_probe_vb_ack0 = g_vblank_ack_count;
    s_post_load_probe_dirty_blks0 = g_dirty_ram_blocks_run;
    s_post_load_probe_chk_entry0 = e;
    s_post_load_probe_chk_fsr0 = fsr;
    s_post_load_probe_chk_fnone0 = fn;
    s_post_load_probe_chk_mid0 = mid;
    s_post_load_probe_chk_eval0 = ev;
    s_post_load_probe_chk_deliv0 = id;
    s_post_load_probe_cyc0 = psx_get_cycle_count();
    s_post_load_probe_ci_unit0 = ci_u;
    s_post_load_probe_ci_supp0 = ci_s;
    s_post_load_probe_ci_none0 = ci_n;
    s_post_load_probe_ci_sr0 = ci_sr;
    s_post_load_probe_ci_deliv0 = ci_d;
    s_post_load_probe_ci_enter0 = ci_e;
    s_post_load_probe_adv_calls0 = 0;
    s_post_load_probe_adv_sum0 = 0;
    s_post_load_probe_svc0 = 0;
    s_post_load_probe_dirty_insns0 = g_dirty_ram_insns_run;
    s_post_load_probe_dirty_pump0 = g_dirty_pump_count;
    s_post_load_probe_stores0 = g_guest_store_count;
    s_post_load_probe_idle_n0 = g_idle_skip_count;
    s_post_load_probe_idle_cyc0 = g_idle_skip_cycles;
    s_post_load_probe_hz_hits0 = hz_h;
    s_post_load_probe_hz_cyc0 = hz_c;
    s_post_load_probe_host_t0 = SDL_GetPerformanceCounter();
    s_post_load_probe_stall_run = 0;
    s_post_load_probe_in_stall = 0;
    s_stall_pc_used = 0;
    s_live_pc_used = 0;
    gl_renderer_present_probe_reset();
    fprintf(stderr,
                 "savestate: post_load_probe armed (300 vblanks; "
                 "PSX_POST_LOAD_PROBE=1; live_pc/ci/adv diag on)\n");
}

void post_load_probe_on_vblank(int turbo_active, int present_reached) {
    if (s_post_load_probe_left <= 0) return;
    s_post_load_probe_i++;
    s_post_load_probe_left--;

    uint64_t skip = 0, swap = 0, dirty_marks = 0;
    int force_left = 0;
    gl_renderer_present_probe_take(&skip, &swap, &dirty_marks, &force_left);
    s_post_load_probe_skip_tot += skip;
    s_post_load_probe_swap_tot += swap;
    s_post_load_probe_dirty_tot += dirty_marks;

    const uint64_t gp0_now = gpu_get_gp0_count();
    const uint64_t gp0_delta = gp0_now - s_post_load_probe_gp00;
    s_post_load_probe_gp00 = gp0_now;
    s_post_load_probe_gp0_tot += gp0_delta;

    extern uint64_t g_vblank_raise_count, g_vblank_deliver_count, g_vblank_ack_count;
    extern uint64_t g_dirty_ram_blocks_run;
    extern uint32_t i_stat, i_mask;
    extern CPUState *debug_cpu_ptr;
    const uint64_t vb_r = g_vblank_raise_count - s_post_load_probe_vb_raise0;
    const uint64_t vb_d = g_vblank_deliver_count - s_post_load_probe_vb_deliv0;
    const uint64_t vb_a = g_vblank_ack_count - s_post_load_probe_vb_ack0;
    s_post_load_probe_vb_raise0 = g_vblank_raise_count;
    s_post_load_probe_vb_deliv0 = g_vblank_deliver_count;
    s_post_load_probe_vb_ack0 = g_vblank_ack_count;
    const uint64_t dirty_blks = g_dirty_ram_blocks_run - s_post_load_probe_dirty_blks0;
    s_post_load_probe_dirty_blks0 = g_dirty_ram_blocks_run;

    uint32_t tcb = 0, gp_a = 0, gp_b = 0, gp_reg = 0;
    uint32_t sr = 0, cause = 0;
    int iec = 0, im2 = 0;
    if (debug_cpu_ptr) {
        gp_reg = debug_cpu_ptr->gpr[28];
        tcb = psx_sched_current_tcb(debug_cpu_ptr);
        sr = debug_cpu_ptr->cop0[12];    /* COP0 Status */
        cause = debug_cpu_ptr->cop0[13]; /* COP0 Cause */
        iec = (sr & 0x1u) ? 1 : 0;
        im2 = (sr & (1u << 10)) ? 1 : 0;
        /* func_8004FD14 frame-counter compare at 0x800501E8. */
        if (debug_cpu_ptr->read_word) {
            gp_a = debug_cpu_ptr->read_word(gp_reg + 2552u);
            gp_b = debug_cpu_ptr->read_word(gp_reg + 2632u);
        }
    }
    /* Hot-path check_interrupts attribution (not delivery_needed — that only
     * samples at present time and was misleading). */
    uint64_t chk_e = 0, chk_fsr = 0, chk_fn = 0, chk_mid = 0, chk_ev = 0, chk_id = 0;
    psx_interrupt_check_path_diag(&chk_e, &chk_fsr, &chk_fn, &chk_mid, &chk_ev, &chk_id);
    const uint64_t d_entry = chk_e - s_post_load_probe_chk_entry0;
    const uint64_t d_fsr = chk_fsr - s_post_load_probe_chk_fsr0;
    const uint64_t d_fnone = chk_fn - s_post_load_probe_chk_fnone0;
    const uint64_t d_mid = chk_mid - s_post_load_probe_chk_mid0;
    const uint64_t d_eval = chk_ev - s_post_load_probe_chk_eval0;
    const uint64_t d_irqd = chk_id - s_post_load_probe_chk_deliv0;
    s_post_load_probe_chk_entry0 = chk_e;
    s_post_load_probe_chk_fsr0 = chk_fsr;
    s_post_load_probe_chk_fnone0 = chk_fn;
    s_post_load_probe_chk_mid0 = chk_mid;
    s_post_load_probe_chk_eval0 = chk_ev;
    s_post_load_probe_chk_deliv0 = chk_id;
    const uint64_t cyc_now = psx_get_cycle_count();
    const uint64_t d_cyc = cyc_now - s_post_load_probe_cyc0;
    s_post_load_probe_cyc0 = cyc_now;

    uint64_t ci_u = 0, ci_s = 0, ci_n = 0, ci_sr = 0, ci_d = 0, ci_e = 0;
    overlay_loader_get_ci_skip_diag(&ci_u, &ci_s, &ci_n, &ci_sr, &ci_d, &ci_e);
    const uint64_t d_ci_unit = ci_u - s_post_load_probe_ci_unit0;
    const uint64_t d_ci_supp = ci_s - s_post_load_probe_ci_supp0;
    const uint64_t d_ci_none = ci_n - s_post_load_probe_ci_none0;
    const uint64_t d_ci_sr = ci_sr - s_post_load_probe_ci_sr0;
    const uint64_t d_ci_deliv = ci_d - s_post_load_probe_ci_deliv0;
    const uint64_t d_ci_enter = ci_e - s_post_load_probe_ci_enter0;
    s_post_load_probe_ci_unit0 = ci_u;
    s_post_load_probe_ci_supp0 = ci_s;
    s_post_load_probe_ci_none0 = ci_n;
    s_post_load_probe_ci_sr0 = ci_sr;
    s_post_load_probe_ci_deliv0 = ci_d;
    s_post_load_probe_ci_enter0 = ci_e;

    const uint64_t adv_calls = g_plp_adv_calls;
    const uint64_t adv_sum = g_plp_adv_sum;
    const uint32_t adv_max = g_plp_adv_max_chunk;
    const uint64_t svc_calls = g_plp_svc_calls;
    const uint64_t d_adv_calls = adv_calls - s_post_load_probe_adv_calls0;
    const uint64_t d_adv_sum = adv_sum - s_post_load_probe_adv_sum0;
    const uint64_t d_svc = svc_calls - s_post_load_probe_svc0;
    s_post_load_probe_adv_calls0 = adv_calls;
    s_post_load_probe_adv_sum0 = adv_sum;
    s_post_load_probe_svc0 = svc_calls;
    g_plp_adv_max_chunk = 0; /* per-frame max */

    extern uint64_t g_dirty_ram_insns_run;
    extern uint64_t g_dirty_pump_count;
    extern uint64_t g_guest_store_count;
    const uint64_t d_dirty_insns = g_dirty_ram_insns_run - s_post_load_probe_dirty_insns0;
    const uint64_t d_dirty_pump = g_dirty_pump_count - s_post_load_probe_dirty_pump0;
    const uint64_t d_stores = g_guest_store_count - s_post_load_probe_stores0;
    s_post_load_probe_dirty_insns0 = g_dirty_ram_insns_run;
    s_post_load_probe_dirty_pump0 = g_dirty_pump_count;
    s_post_load_probe_stores0 = g_guest_store_count;

    const uint64_t d_idle_n = g_idle_skip_count - s_post_load_probe_idle_n0;
    const uint64_t d_idle_cyc = g_idle_skip_cycles - s_post_load_probe_idle_cyc0;
    s_post_load_probe_idle_n0 = g_idle_skip_count;
    s_post_load_probe_idle_cyc0 = g_idle_skip_cycles;

    uint64_t hz_h = 0, hz_c = 0;
    psx_vsync_query_hle_horizon_totals(&hz_h, &hz_c);
    const uint64_t d_hz_hits = hz_h - s_post_load_probe_hz_hits0;
    const uint64_t d_hz_cyc = hz_c - s_post_load_probe_hz_cyc0;
    s_post_load_probe_hz_hits0 = hz_h;
    s_post_load_probe_hz_cyc0 = hz_c;

    const Uint64 host_now = SDL_GetPerformanceCounter();
    const Uint64 host_freq = SDL_GetPerformanceFrequency();
    const double host_ms = (host_freq > 0)
        ? (1000.0 * (double)(host_now - s_post_load_probe_host_t0) /
           (double)host_freq)
        : 0.0;
    s_post_load_probe_host_t0 = host_now;

    GpuDisplayInfo di;
    gpu_get_display_info(&di);
    const int rect_dirty = (di.width > 0 && di.height > 0)
        ? gl_renderer_present_rect_dirty((int)di.display_x, (int)di.display_y,
                                         (int)di.width, (int)di.height)
        : 0;

    CDROMDebugState cd;
    cdrom_debug_snapshot(&cd);
    const int cd_wait = cdrom_savestate_cd_wait_active();
    const int boost_left = cdrom_savestate_boost_vblanks_remaining();
    const int xa = cdrom_xa_stream_active();

    const uint32_t irq_pc = psx_last_irq_check_pc();
    const uint32_t resume_pc = psx_compiled_irq_resume_pc();
    extern uint32_t g_debug_current_func_addr;
    extern uint32_t g_debug_last_store_pc;
    extern int g_psx_dispatch_depth;
    const uint32_t func = g_debug_current_func_addr;
    const uint32_t store_pc = g_debug_last_store_pc;
    const uint32_t live_pc = debug_cpu_ptr ? debug_cpu_ptr->pc : 0u;
    const uint32_t live_ra = debug_cpu_ptr ? debug_cpu_ptr->gpr[31] : 0u;
    const int unit_depth = overlay_loader_call_unit_depth();
    const int disp_depth = g_psx_dispatch_depth;
    int cooldown_left = 0;
    int in_exc = 0;
    psx_get_freeze_diag(NULL, NULL, &in_exc, &cooldown_left, NULL, NULL);

    const int stalled = (gp0_delta == 0);
    if (stalled) {
        s_post_load_probe_stall_run++;
        s_post_load_probe_in_stall = 1;
        post_load_probe_stall_pc_note(irq_pc ? irq_pc : resume_pc);
        post_load_probe_stall_pc_note(func);
        post_load_probe_live_pc_note(live_pc);
    } else if (s_post_load_probe_in_stall) {
        fprintf(stderr,
                     "post_load_probe STALL_END at #%d after %d vblanks "
                     "(irq_pc=0x%08X resume=0x%08X func=0x%08X "
                     "live=0x%08X ra=0x%08X unit=%d disp=%d)\n",
                     s_post_load_probe_i, s_post_load_probe_stall_run,
                     (unsigned)irq_pc, (unsigned)resume_pc, (unsigned)func,
                     (unsigned)live_pc, (unsigned)live_ra,
                     unit_depth, disp_depth);
        post_load_probe_stall_pc_dump("end");
        post_load_probe_live_pc_dump("end");
        s_post_load_probe_in_stall = 0;
        s_post_load_probe_stall_run = 0;
        s_stall_pc_used = 0;
        s_live_pc_used = 0;
    }

    /* Dense samples during soft-stall; otherwise first 32 + every 15. */
    const int log_line =
        stalled ||
        (s_post_load_probe_i <= 32) ||
        (s_post_load_probe_i % 15 == 0) ||
        (s_post_load_probe_left == 0);
    if (log_line) {
        fprintf(stderr,
            "post_load_probe #%d: live=0x%08X ra=0x%08X "
            "irq_pc=0x%08X resume=0x%08X func=0x%08X "
            "store=0x%08X idle=0x%08X unit=%d disp=%d turbo=%d reached=%d "
            "swap=%llu skip=%llu dirty_marks=%llu force=%d rect_dirty=%d "
            "fb=%ux%u@(%u,%u) dis=%d d24=%d gp0=%llu "
            "cd(pend=%d cmd=0x%02X dly=%d read=%d rdly=%d xa=%d wait=%d boost=%d) "
            "exc=%d cool=%d istat=0x%X imask=0x%X "
            "vb(r=%llu d=%llu a=%llu) tcb=0x%08X "
            "gp9f8=%d gpA48=%d dirty_blks=%llu dins=%llu dpump=%llu stores=%llu "
            "sr=0x%08X iec=%d im2=%d cause=0x%08X cyc=%llu host_ms=%.2f "
            "chk(e=%llu fsr=%llu fn=%llu mid=%llu eval=%llu irq=%llu) "
            "ci(unit=%llu supp=%llu none=%llu sr=%llu deliv=%llu enter=%llu) "
            "adv(n=%llu sum=%llu max=%u svc=%llu) "
            "idle_skip(n=%llu cyc=%llu) hz(n=%llu cyc=%llu)\n",
            s_post_load_probe_i,
            (unsigned)live_pc, (unsigned)live_ra,
            (unsigned)irq_pc, (unsigned)resume_pc, (unsigned)func,
            (unsigned)store_pc, (unsigned)g_idle_skip_last_pc,
            unit_depth, disp_depth,
            turbo_active, present_reached,
            (unsigned long long)swap, (unsigned long long)skip,
            (unsigned long long)dirty_marks, force_left, rect_dirty,
            (unsigned)di.width, (unsigned)di.height,
            (unsigned)di.display_x, (unsigned)di.display_y,
            di.disabled ? 1 : 0, di.depth24 ? 1 : 0,
            (unsigned long long)gp0_delta,
            cd.pending_pending, (unsigned)cd.pending_cmd, cd.pending_delay,
            cd.reading, cd.read_delay, xa, cd_wait, boost_left,
            in_exc, cooldown_left, (unsigned)i_stat, (unsigned)i_mask,
            (unsigned long long)vb_r, (unsigned long long)vb_d,
            (unsigned long long)vb_a, (unsigned)tcb,
            (int)gp_a, (int)gp_b, (unsigned long long)dirty_blks,
            (unsigned long long)d_dirty_insns, (unsigned long long)d_dirty_pump,
            (unsigned long long)d_stores,
            (unsigned)sr, iec, im2, (unsigned)cause,
            (unsigned long long)d_cyc, host_ms,
            (unsigned long long)d_entry, (unsigned long long)d_fsr,
            (unsigned long long)d_fnone, (unsigned long long)d_mid,
            (unsigned long long)d_eval, (unsigned long long)d_irqd,
            (unsigned long long)d_ci_unit, (unsigned long long)d_ci_supp,
            (unsigned long long)d_ci_none, (unsigned long long)d_ci_sr,
            (unsigned long long)d_ci_deliv, (unsigned long long)d_ci_enter,
            (unsigned long long)d_adv_calls, (unsigned long long)d_adv_sum,
            (unsigned)adv_max, (unsigned long long)d_svc,
            (unsigned long long)d_idle_n, (unsigned long long)d_idle_cyc,
            (unsigned long long)d_hz_hits, (unsigned long long)d_hz_cyc);
    }
    if (s_post_load_probe_left == 0) {
        if (s_post_load_probe_in_stall) {
            post_load_probe_stall_pc_dump("done-still-stalled");
            post_load_probe_live_pc_dump("done-still-stalled");
        }
        g_plp_cycle_diag = 0;
        fprintf(stderr,
            "post_load_probe DONE: n=%d swap_tot=%llu skip_tot=%llu "
            "dirty_tot=%llu gp0_tot=%llu\n",
            s_post_load_probe_i,
            (unsigned long long)s_post_load_probe_swap_tot,
            (unsigned long long)s_post_load_probe_skip_tot,
            (unsigned long long)s_post_load_probe_dirty_tot,
            (unsigned long long)s_post_load_probe_gp0_tot);
    }
}
