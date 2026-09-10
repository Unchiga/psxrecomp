/* Debug-server commands: emulated hardware state.
 *
 * The read-only window onto the machine -- guest RAM, GPU, DMA, CD-ROM, SIO,
 * SPU, GTE, timers and interrupts -- plus the few writes that exist for
 * poking state during an investigation.
 *
 * These handlers own no state of their own. Each one reads a subsystem through
 * its normal header and formats the answer, which is why they can live apart
 * from the server core: the only thing they share with it is the small helper
 * seam in debug_server_internal.h.
 */

#include "debug_cmds_hardware.h"
#include "debug_server_internal.h"

#include "psx_sdl.h"
#include "psx_host_input.h"
#include "cpu_state.h"
#include "dma.h"
#include "gpu.h"
#include "gpu_render.h"
#include "pgxp.h"
#include "cdrom.h"
#include "sio.h"
#include "memcard.h"
#include "spu.h"
#include "audio_trace.h"
#include "mdec.h"
#include "interrupts.h"
#include "psx_cycles.h"
#include "timers.h"
#include "load_accel.h"

/* Same externs the server core declares: memory.c and interrupts.c publish
 * these without a header the C side can include. */
extern uint32_t i_stat;
extern uint32_t i_mask;
extern uint32_t psx_read_word(uint32_t addr);
extern void     psx_write_word(uint32_t addr, uint32_t val);
extern uint8_t  psx_read_byte(uint32_t addr);
extern void     psx_write_byte(uint32_t addr, uint8_t val);

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void handle_ram_dump_file(int id, const char *json)
{
    char buf[32], path[512];
    uint32_t addr = 0x80000000u;
    if (json_get_str(json, "addr", buf, sizeof(buf))) addr = hex_to_u32(buf);
    int len = json_get_int(json, "len", 0x200000);
    if (len < 1) len = 1;
    if (len > 0x200000) len = 0x200000;
    if (!json_get_str(json, "path", path, sizeof(path))) {
        send_err(id, "missing path"); return;
    }
    FILE *f = fopen(path, "wb");
    if (!f) { send_err(id, "cannot open file"); return; }
    uint8_t *tmp = (uint8_t *)malloc((size_t)len);
    if (!tmp) { fclose(f); send_err(id, "alloc failed"); return; }
    for (int i = 0; i < len; i++)
        tmp[i] = psx_read_byte(addr + (uint32_t)i);
    size_t wrote = fwrite(tmp, 1, (size_t)len, f);
    free(tmp);
    fclose(f);
    if (wrote != (size_t)len) { send_err(id, "short write"); return; }
    send_fmt("{\"id\":%d,\"ok\":true,\"addr\":\"0x%08X\",\"len\":%d,\"path\":\"%s\"}",
             id, addr, len, path);
}

void handle_read_ram(int id, const char *json)
{
    char addr_str[32];
    if (!json_get_str(json, "addr", addr_str, sizeof(addr_str))) {
        send_err(id, "missing addr"); return;
    }
    uint32_t addr = hex_to_u32(addr_str);
    int len = json_get_int(json, "len", 1);
    if (len < 1) len = 1;
    /* Effectively the entire 2 MB RAM in one shot.  Response uses a heap-
     * sized envelope so we don't truncate. */
    if (len > 0x200000) len = 0x200000;

    /* Heap buffer for hex chars + JSON envelope.  Each byte = 2 hex chars. */
    size_t env = 256;
    size_t total = (size_t)len * 2 + env;
    char *out = (char *)malloc(total);
    if (!out) { send_err(id, "alloc failed"); return; }
    int hdr = snprintf(out, env,
                       "{\"id\":%d,\"ok\":true,\"addr\":\"0x%08X\",\"len\":%d,\"hex\":\"",
                       id, addr, len);
    char *hex = out + hdr;
    /* Nibble-table encode: snprintf per byte costs seconds for a 2 MB
     * read, which stalls the main-thread-pumped server (and the SDL
     * event loop) long enough to look like a wedge. */
    static const char H[] = "0123456789abcdef";
    for (int i = 0; i < len; i++) {
        uint8_t b = psx_read_byte(addr + (uint32_t)i);
        hex[(size_t)i * 2]     = H[b >> 4];
        hex[(size_t)i * 2 + 1] = H[b & 0xF];
    }
    char *tail = hex + (size_t)len * 2;
    memcpy(tail, "\"}", 3);
    debug_server_send_line(out);
    free(out);
}

/* "dump_ram" is an alias of "read_ram".  The old implementation answered a
 * single request with one response line per 256-byte chunk; any client that
 * follows the one-request/one-response protocol left the extra lines unread,
 * the socket send buffer filled, the main-thread-pumped server blocked, and
 * the freeze watchdog killed the process.  One request, one response. */

void handle_write_ram(int id, const char *json)
{
    char addr_str[32], val_str[32];
    if (!json_get_str(json, "addr", addr_str, sizeof(addr_str))) {
        send_err(id, "missing addr"); return;
    }
    if (!json_get_str(json, "val", val_str, sizeof(val_str))) {
        send_err(id, "missing val"); return;
    }
    uint32_t addr = hex_to_u32(addr_str);
    uint8_t val = (uint8_t)hex_to_u32(val_str);
    psx_write_byte(addr, val);
    send_ok(id);
}

/* write_mem addr=<hex> hex=<byte string> — write a BLOB in one command.
 *
 * write_ram pokes one byte and fill_ram writes one repeated byte, so restoring
 * arbitrary content meant a command per byte. That is not merely verbose: the
 * server is pumped from the emu thread and serves one command per connection,
 * so each round trip costs a whole frame — a 2.6 KB restore ran for 20 seconds
 * and could not keep up with a running guest. This writes the whole blob inside
 * one service slot.
 *
 * Bounded to main RAM, like fill_ram, so it cannot be aimed at MMIO. */
static int write_mem_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void handle_write_mem(int id, const char *json)
{
    char addr_str[32];
    if (!json_get_str(json, "addr", addr_str, sizeof(addr_str))) {
        send_err(id, "missing addr"); return;
    }
    /* The blob can be large; take it straight out of the request text rather
     * than copying it into a bounded stack buffer first. */
    const char *key = strstr(json, "\"hex\"");
    if (!key) { send_err(id, "missing hex"); return; }
    const char *p = strchr(key + 5, '"');
    if (!p) { send_err(id, "missing hex"); return; }
    p++;
    const char *end = strchr(p, '"');
    if (!end) { send_err(id, "missing hex"); return; }

    const size_t digits = (size_t)(end - p);
    if (digits == 0 || (digits & 1u)) { send_err(id, "hex must be whole bytes"); return; }
    const uint32_t len = (uint32_t)(digits / 2u);

    const uint32_t addr = hex_to_u32(addr_str);
    const uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys + len > 0x200000u) { send_err(id, "range outside main RAM"); return; }

    for (uint32_t i = 0; i < len; i++) {
        int hi = write_mem_nibble(p[i * 2u]);
        int lo = write_mem_nibble(p[i * 2u + 1u]);
        if (hi < 0 || lo < 0) { send_err(id, "bad hex digit"); return; }
        psx_write_byte(addr + i, (uint8_t)((hi << 4) | lo));
    }
    send_fmt("{\"id\":%d,\"ok\":true,\"addr\":\"0x%08X\",\"len\":%u}",
             id, addr, (unsigned)len);
}

/* geom_correction — is [video] geometry_correction / perspective_texturing
 * actually doing anything on THIS title?
 *
 * Both enhancements are silent no-ops on content they cannot prove is
 * projected geometry: a vertex whose sub-pixel fraction was never cached, or a
 * packet whose position words lack full GTE projection provenance, simply draws
 * the faithful way. So "enabled" alone tells you nothing — these counters are
 * how you tell an engaged correction from an inert one. Both are free-running
 * totals; sample twice and diff for a per-window rate. */
void handle_geom_correction(int id, const char *json)
{
    /* Optional "geometry" / "perspective" args flip the enhancement live and
     * then report, so an A/B is two commands on one scene instead of two runs
     * on two scenes. Requests are serviced on the emu thread, so this cannot
     * race the frame it is changing. Omit both to only read. */
    int want_geom = json_get_int(json, "geometry", -1);
    int want_persp = json_get_int(json, "perspective", -1);
    if (want_geom >= 0 || want_persp >= 0)
        psx_host_set_geometry_enhancements(want_geom, want_persp);
    /* The miss split is the diagnostic that matters. The position table is
     * exact (one slot per reachable SXY, no hashing), so "unrecorded" means no
     * projection was EVER cached at that screen position — a real coverage gap
     * in the tracking, not a cache artifact. A high unrecorded share means the
     * game's vertex path never reaches us in a matchable form, which only
     * full value propagation can fix; a high ambiguous share instead means
     * distinct vertices are landing on the same pixel. */
    uint32_t lookups = 0, hits = 0, unrec = 0, ambig = 0;
    gte_geometry_correction_stats(&lookups, &hits, &unrec, &ambig);
    uint32_t pa = 0, pns = 0, pnp = 0, pz = 0;
    gpu_texture_correction_stats(&pa, &pns, &pnp, &pz);
    /* PGXP dataflow census (per-vertex): the primary provenance source.
     * dataflow_hit is the number that had to move — the G1.9 gate is a
     * dataflow-hit share dramatically above the 5.2% the position table
     * measured on its own. value_mismatch counts shadows that were present
     * but described a different word (stale = provenance hole to hunt). */
    PGXPStats ps;
    pgxp_get_stats(&ps);
    send_fmt("{\"id\":%d,\"ok\":true,"
             "\"geometry_correction\":%d,"
             "\"perspective_texturing\":%d,"
             "\"geometry_vertex_hits\":%u,"
             "\"perspective_triangles\":%u,"
             "\"lookups\":%u,\"miss_unrecorded\":%u,\"miss_ambiguous\":%u,"
             "\"persp_attempts\":%u,\"persp_no_source\":%u,"
             "\"persp_no_provenance\":%u,\"persp_zero_z\":%u,"
             "\"pgxp\":{\"enabled\":%d,\"cpu_mode\":%d,\"tolerance\":%.3f,"
             "\"lookups\":%llu,\"dataflow_hit\":%llu,\"fallback_hit\":%llu,"
             "\"native\":%llu,\"value_mismatch\":%llu,\"trunc_reject\":%llu,"
             "\"tolerance_reject\":%llu,\"w_valid\":%llu,"
             "\"produced\":%llu,\"swc2_stores\":%llu}}",
             id,
             gte_geometry_correction_enabled(),
             gpu_texture_correction_enabled(),
             (unsigned)hits,
             (unsigned)gpu_texture_correction_hits(),
             (unsigned)lookups, (unsigned)unrec, (unsigned)ambig,
             (unsigned)pa, (unsigned)pns, (unsigned)pnp, (unsigned)pz,
             pgxp_enabled(), pgxp_cpu_mode(), (double)pgxp_tolerance(),
             (unsigned long long)ps.lookups,
             (unsigned long long)ps.dataflow_hit,
             (unsigned long long)ps.fallback_hit,
             (unsigned long long)ps.native,
             (unsigned long long)ps.value_mismatch,
             (unsigned long long)ps.trunc_reject,
             (unsigned long long)ps.tolerance_reject,
             (unsigned long long)ps.w_valid,
             (unsigned long long)ps.produced,
             (unsigned long long)ps.swc2_stores);
}

/* pgxp — live-tune the value-propagation engine for one-toggle isolation runs
 * without a rebuild: {"cmd":"pgxp","cpu_mode":0|1,"tolerance":F}. Fields are
 * optional; the reply echoes the resulting state (same shape as
 * geom_correction's "pgxp" object, flattened). */
void handle_pgxp(int id, const char *json)
{
    /* Live toggles for the one-toggle-at-a-time A/B protocol (ENHANCEMENTS.md
     * G1.6 method rule): same scene, flip one knob, screenshot_hires. */
    int geom = json_get_int(json, "geometry", -1);
    int tex = json_get_int(json, "texture", -1);
    if (geom >= 0 || tex >= 0)
        psx_host_set_geometry_enhancements(geom, tex);
    int cm = json_get_int(json, "cpu_mode", -1);
    if (cm >= 0)
        pgxp_set_cpu_mode(cm != 0);
    /* tolerance is fractional (sub-pixel), so scan it directly — json_get_int
     * would truncate 0.5 to 0. */
    const char *p = strstr(json, "\"tolerance\"");
    if (p) {
        p += 11;
        while (*p == ' ' || *p == ':' || *p == '"') p++;
        if (*p == '-' || (*p >= '0' && *p <= '9') || *p == '.')
            pgxp_set_tolerance((float)strtod(p, NULL));
    }
    send_fmt("{\"id\":%d,\"ok\":true,\"enabled\":%d,\"cpu_mode\":%d,"
             "\"tolerance\":%.3f,\"suppress\":%u,\"active\":%d}",
             id, pgxp_enabled(), pgxp_cpu_mode(), (double)pgxp_tolerance(),
             (unsigned)pgxp_test_suppress_depth(), pgxp_test_active());
}

/* fast_loads — set/read the disc-load acceleration level (GAME > FAST LOADING).
 * Exists so a load window can be timed at each step in one sitting; pair it
 * with cdrom_bursts, which reports the per-burst sector count and wall ms. */
void handle_fast_loads(int id, const char *json)
{
    int level = json_get_int(json, "level", -1);
    if (level >= 0 && level <= 2)
        psx_host_set_fast_loads(level);
    /* Report BOTH divisors, never the parsed argument. The original form
     * echoed level/divisor = -1 when called with no argument, which reads as
     * "fast loading is off" — and was recorded as exactly that in ISSUES.md on
     * a machine running instant loads. Reporting only the LIVE divisor has the
     * same failure mode for a different reason: boot deliberately runs at 1x
     * until cdrom_notify_game_started latches the configured value, so an
     * instant setup reads "authentic" for the whole boot. `mode` therefore
     * describes the CONFIGURED setting; `active_now` says whether it has been
     * latched yet. */
    int divisor = cdrom_get_speed_divisor();
    int game_divisor = cdrom_get_game_speed_divisor();
    send_fmt("{\"id\":%d,\"ok\":true,\"requested_level\":%d,"
             "\"divisor\":%d,\"game_divisor\":%d,\"mode\":\"%s\","
             "\"active_now\":%s,\"boot_phase\":%s,\"instant_budget\":%d}",
             id, level, divisor, game_divisor,
             game_divisor == 0 ? "instant"
                               : (game_divisor == 1 ? "authentic" : "fast"),
             divisor == game_divisor ? "true" : "false",
             divisor != game_divisor ? "true" : "false",
             cdrom_get_instant_rate());
}

void handle_gpu_state(int id, const char *json)
{
    (void)json;
    GpuDisplayInfo di;
    gpu_get_display_info(&di);
    uint32_t gpustat = gpu_read_gpustat();
    uint32_t hx1 = 0, hx2 = 0, hy1 = 0, hy2 = 0, hr1 = 0, hr2 = 0;
    gpu_get_crtc_debug(&hx1, &hx2, &hy1, &hy2, &hr1, &hr2);

    GpuDrawArea da;
    gpu_get_draw_area(&da);
    uint64_t nop, fill, draw, env, copy;
    gpu_get_gp0_stats(&nop, &fill, &draw, &env, &copy);
    int split_active = 0, split_left_age = 0, split_right_age = 0;
    gpu_vertical_split_debug(&split_active, &split_left_age, &split_right_age);
    GpuWsDebug ws;
    gpu_ws_get_debug(&ws);
    send_fmt("{\"id\":%d,\"ok\":true,"
             "\"display_x\":%d,\"display_y\":%d,"
             "\"width\":%d,\"height\":%d,"
             "\"depth\":%d,\"depth24\":%d,"
             "\"disabled\":%d,"
             "\"h_display\":[%u,%u],\"v_display\":[%u,%u],"
             "\"hres1\":%u,\"hres2\":%u,"
             "\"gpustat\":\"0x%08X\","
             "\"gp0_writes\":%llu,"
             "\"gp0_nop\":%llu,\"gp0_fill\":%llu,\"gp0_draw\":%llu,\"gp0_env\":%llu,\"gp0_copy\":%llu,"
             "\"draw_area\":[%u,%u,%u,%u],"
             "\"draw_offset\":[%d,%d],"
             "\"vertical_split\":{\"active\":%d,\"left_age\":%d,\"right_age\":%d},"
             "\"ws\":{\"configured\":%d,\"active\":%d,\"game_mode\":%d,"
             "\"present_native_43\":%d,\"x_margin\":%d,"
             "\"activation_margin\":%d,\"squash\":[%d,%d],"
             "\"mode\":%d,\"nw_extra\":%d,"
             "\"cur_frame\":%llu,\"last_tag_frame\":%u,\"last_3d_frame\":%u,"
             "\"gte_verts\":%u,\"last_world3d_frame\":%u,"
             "\"ovh_prims\":%u,\"last_ovh_frame\":%u,"
             "\"auto_ui\":{\"configured\":%d,\"dense\":%d,\"ot_rank\":%u,"
             "\"candidates\":%llu,\"transforms\":%llu},"
             "\"aspect_cone\":{\"calls\":%llu,\"identity_43\":%llu,"
             "\"vanilla_keep\":%llu,\"visible_keep\":%llu,"
             "\"guard_keep\":%llu,\"hysteresis_keep\":%llu,"
             "\"outside_reject\":%llu,\"queue_reject\":%llu,"
             "\"queue_highwater\":[%u,%u,%u]},"
             "\"terrain_angle\":{\"calls\":%llu,\"identity_43\":%llu,"
             "\"max_vanilla\":%u,\"max_widened\":%u}}}",
             id, di.display_x, di.display_y,
             di.width, di.height,
             di.depth24 ? 24 : 15, di.depth24,
             di.disabled,
             hx1, hx2, hy1, hy2, hr1, hr2,
             gpustat,
             (unsigned long long)gpu_get_gp0_count(),
             (unsigned long long)nop, (unsigned long long)fill,
             (unsigned long long)draw, (unsigned long long)env,
             (unsigned long long)copy,
             da.left, da.top, da.right, da.bottom,
             da.offset_x, da.offset_y,
             split_active, split_left_age, split_right_age,
             ws.configured, ws.active, ws.game_mode,
             ws.present_native_43, ws.x_margin, ws.activation_margin,
             ws.xnum, ws.xden,
             ws.mode, ws.nw_extra,
             (unsigned long long)ws.cur_frame, ws.last_tag_frame,
              ws.last_3d_frame, ws.gte_verts, ws.last_world3d_frame,
              ws.ovh_prims, ws.last_ovh_frame,
              ws.auto_ui_squash, ws.auto_ui_dense, ws.auto_ui_ot_rank,
              (unsigned long long)ws.auto_ui_candidates,
              (unsigned long long)ws.auto_ui_transforms,
              (unsigned long long)ws.aspect_cone_calls,
             (unsigned long long)ws.aspect_cone_43_identity,
             (unsigned long long)ws.aspect_cone_vanilla_keep,
             (unsigned long long)ws.aspect_cone_visible_keep,
             (unsigned long long)ws.aspect_cone_guard_keep,
             (unsigned long long)ws.aspect_cone_hysteresis_keep,
             (unsigned long long)ws.aspect_cone_outside_reject,
             (unsigned long long)ws.aspect_cone_queue_reject,
             ws.aspect_cone_queue_highwater[0],
             ws.aspect_cone_queue_highwater[1],
             ws.aspect_cone_queue_highwater[2],
             (unsigned long long)ws.angle_calls,
             (unsigned long long)ws.angle_43_identity,
             ws.angle_max_vanilla, ws.angle_max_widened);
}

void handle_ws_aspect_cone_site(int id, const char *json)
{
    char addr_str[32];
    if (!json_get_str(json, "address", addr_str, sizeof(addr_str))) {
        send_err(id, "missing address");
        return;
    }
    GpuWsAspectConeSiteDebug site;
    if (!gpu_ws_get_aspect_cone_site_debug(hex_to_u32(addr_str), &site)) {
        send_err(id, "aspect-cone site not configured");
        return;
    }
    send_fmt("{\"id\":%d,\"ok\":true,\"address\":\"0x%08X\","
             "\"calls\":%llu,\"identity_43\":%llu,"
             "\"vanilla_keep\":%llu,\"visible_keep\":%llu,"
             "\"guard_keep\":%llu,\"hysteresis_keep\":%llu,"
             "\"outside_reject\":%llu,\"queue_reject\":%llu}",
             id, site.address,
             (unsigned long long)site.calls,
             (unsigned long long)site.identity_43,
             (unsigned long long)site.vanilla_keep,
             (unsigned long long)site.visible_keep,
             (unsigned long long)site.guard_keep,
             (unsigned long long)site.hysteresis_keep,
             (unsigned long long)site.outside_reject,
             (unsigned long long)site.queue_reject);
}

/* fill_ram addr=<hex> len=<n> val=<byte> — set a run of bytes to one value.
 *
 * write_ram pokes a SINGLE byte, so clearing a structure meant one command per
 * byte: zeroing the 722-entry card trunk across its three copies is 2166 round
 * trips, each on its own connection. This is the bulk form. Bounded to main RAM
 * so it cannot be aimed at MMIO. */
void handle_fill_ram(int id, const char *json)
{
    char addr_str[32], val_str[32];
    if (!json_get_str(json, "addr", addr_str, sizeof(addr_str))) {
        send_err(id, "missing addr"); return;
    }
    uint32_t addr = hex_to_u32(addr_str);
    int len = json_get_int(json, "len", 0);
    if (len < 1) { send_err(id, "missing len"); return; }
    if (len > (1 << 20)) { send_err(id, "len too large"); return; }
    uint8_t val = 0;
    if (json_get_str(json, "val", val_str, sizeof(val_str)))
        val = (uint8_t)hex_to_u32(val_str);
    const uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys + (uint32_t)len > 0x200000u) {
        send_err(id, "range outside main RAM"); return;
    }
    for (int i = 0; i < len; i++)
        psx_write_byte(addr + (uint32_t)i, val);
    send_fmt("{\"id\":%d,\"ok\":true,\"addr\":\"0x%08X\",\"len\":%d,"
             "\"val\":%u}", id, addr, len, val);
}

/* unaligned_stats — how often did the interpreter wave through a data access
 * that real R3000A hardware would have faulted on?
 *
 * Zero unless PSX_RELAX_ALIGNMENT=1. Non-zero means the code being run is doing
 * something no stock PS1 game does, and the run is NOT hardware-faithful: it is
 * matching the compiled backend (and every mainstream emulator) instead. See
 * the header comment on interp_align_fault in dirty_ram_interp.c. */
void handle_unaligned_stats(int id, const char *json)
{
    (void)json;
    extern uint64_t g_interp_unaligned_load;
    extern uint64_t g_interp_unaligned_store;
    extern uint32_t g_interp_unaligned_last_pc;
    extern uint32_t g_interp_unaligned_last_addr;
    const char *env = getenv("PSX_RELAX_ALIGNMENT");
    send_fmt("{\"id\":%d,\"ok\":true,\"relaxed\":%d,"
             "\"loads\":%llu,\"stores\":%llu,"
             "\"last_pc\":\"0x%08X\",\"last_addr\":\"0x%08X\"}",
             id, (env && *env && *env != '0') ? 1 : 0,
             (unsigned long long)g_interp_unaligned_load,
             (unsigned long long)g_interp_unaligned_store,
             (unsigned)g_interp_unaligned_last_pc,
             (unsigned)g_interp_unaligned_last_addr);
}

void handle_mem_words(int id, const char *json)
{
    char addr_str[32];
    if (!json_get_str(json, "addr", addr_str, sizeof(addr_str))) {
        send_err(id, "missing addr");
        return;
    }

    uint32_t addr = hex_to_u32(addr_str);
    int count = json_get_int(json, "count", 16);
    if (count < 1) count = 1;
    if (count > 256) count = 256;

    size_t bufsz = 256u + (size_t)count * 32u;
    char *buf = (char *)malloc(bufsz);
    if (!buf) { send_err(id, "oom"); return; }

    size_t pos = 0;
    pos += snprintf(buf + pos, bufsz - pos,
                    "{\"id\":%d,\"ok\":true,\"addr\":\"0x%08X\",\"words\":[",
                    id, addr);
    for (int i = 0; i < count && pos < bufsz - 32; i++) {
        uint32_t a = addr + (uint32_t)i * 4u;
        uint32_t v = psx_read_word(a);
        pos += snprintf(buf + pos, bufsz - pos, "%s\"0x%08X\"",
                        i ? "," : "", v);
    }
    pos += snprintf(buf + pos, bufsz - pos, "]}");
    debug_server_send_line(buf);
    free(buf);
}

/* Precise-event-slicing validation: report the cycle distance to the next
 * deliverable interrupt, broken down per source. Compare against the live timer
 * counters / VBLANK pacing (timers_state, freeze_check) to validate
 * cycles_to_next_event before wiring it into the two-tier executor. UINT32_MAX
 * (4294967295) for a source means "no deliverable IRQ scheduled". */
void handle_cycles_to_next_event(int id, const char *json)
{
    (void)json;
    uint32_t agg = cycles_to_next_event();
    uint32_t t = timers_cycles_to_irq(i_mask);
    uint32_t c = cdrom_cycles_to_irq(i_mask);
    uint32_t d = dma_cycles_to_irq(i_mask);
    uint32_t s = sio_cycles_to_irq(i_mask);
    send_fmt("{\"id\":%d,\"ok\":true,"
             "\"i_stat\":\"0x%08X\",\"i_mask\":\"0x%08X\","
             "\"cycles_to_next_event\":%u,"
             "\"timers\":%u,\"cdrom\":%u,\"dma\":%u,\"sio\":%u}",
             id, i_stat, i_mask, agg, t, c, d, s);
}

void handle_irq_state(int id, const char *json)
{
    (void)json;
    send_fmt("{\"id\":%d,\"ok\":true,"
             "\"i_stat\":\"0x%08X\",\"i_mask\":\"0x%08X\","
             "\"pending\":\"0x%08X\","
             "\"cop0_sr\":\"0x%08X\","
             "\"IEc\":%d,\"IM2\":%d,\"BEV\":%d,"
             "\"dpcr\":\"0x%08X\",\"dicr\":\"0x%08X\"}",
             id, i_stat, i_mask, i_stat & i_mask,
             debug_server_cpu() ? debug_server_cpu()->cop0[12] : 0,
             debug_server_cpu() ? (debug_server_cpu()->cop0[12] & 1) : 0,
             debug_server_cpu() ? ((debug_server_cpu()->cop0[12] >> 10) & 1) : 0,
             debug_server_cpu() ? ((debug_server_cpu()->cop0[12] >> 22) & 1) : 0,
             dma_get_dpcr(), dma_get_dicr());
}

/* vblank_rate: report the ONE cycle-paced VBlank authority's raise/deliver
 * counts, the (normally-off) GPUSTAT-poll fallback raise count, and the
 * per-frame GP0(E5) draw-offset-Y range/count. Used to confirm the guest is
 * receiving exactly 60 VBlanks/s (not the ~96/s the stale poll fallback caused)
 * and to probe double-buffer draw-offset alternation. */
void handle_vblank_rate(int id, const char *json)
{
    (void)json;
    extern uint64_t g_vblank_raise_count, g_vblank_deliver_count;
    extern uint64_t g_pollhack_vblank_count;
    extern int32_t  g_doff_min_last, g_doff_max_last;
    extern uint32_t g_doff_cnt_last;
    send_fmt("{\"id\":%d,\"ok\":true,"
             "\"cycle_paced_raise\":%llu,"
             "\"delivered\":%llu,"
             "\"pollhack_raise\":%llu,"
             "\"doff_min\":%d,\"doff_max\":%d,\"doff_cnt\":%u}",
             id,
             (unsigned long long)g_vblank_raise_count,
             (unsigned long long)g_vblank_deliver_count,
             (unsigned long long)g_pollhack_vblank_count,
             g_doff_min_last, g_doff_max_last, g_doff_cnt_last);
}

void handle_timers_state(int id, const char *json)
{
    (void)json;
    uint16_t counter[3], target[3];
    uint32_t mode[3], frac[3];
    int32_t irq_line[3];
    timers_get_snapshot(counter, mode, target, irq_line, frac);
    send_fmt("{\"id\":%d,\"ok\":true,\"timers\":["
             "{\"ch\":0,\"counter\":%u,\"mode\":\"0x%04X\",\"target\":%u,"
             "\"irq_line\":%d,\"frac\":%u},"
             "{\"ch\":1,\"counter\":%u,\"mode\":\"0x%04X\",\"target\":%u,"
             "\"irq_line\":%d,\"frac\":%u},"
             "{\"ch\":2,\"counter\":%u,\"mode\":\"0x%04X\",\"target\":%u,"
             "\"irq_line\":%d,\"frac\":%u}]}",
             id,
             counter[0], mode[0], target[0], irq_line[0], frac[0],
             counter[1], mode[1], target[1], irq_line[1], frac[1],
             counter[2], mode[2], target[2], irq_line[2], frac[2]);
}

/* GPU opcode counter — defined in gpu.c */
static const char *cdrom_trace_kind_name(uint8_t kind)
{
    switch (kind) {
    case 'N': return "init";
    case 'C': return "cmd";
    case 'I': return "set_irq";
    case 'F': return "fire_irq";
    case 'f': return "irq_masked";
    case 'S': return "sector";
    case 's': return "sector_skip";
    case 'A': return "xa_audio";
    case 'a': return "xa_skip";
    case 'X': return "xa_unsupported";
    case 'O': return "overwrite";
    case 'R': return "read";
    case 'W': return "write";
    case 'D': return "dma";
    default: return "unknown";
    }
}

void handle_cdrom_state(int id, const char *json)
{
    (void)json;
    CDROMDebugState s;
    cdrom_debug_snapshot(&s);
/* Expand a uint64_t[6] counter into the six %llu arguments it feeds. */
#define CD_INT6(a) (unsigned long long)(a)[0], (unsigned long long)(a)[1], \
                   (unsigned long long)(a)[2], (unsigned long long)(a)[3], \
                   (unsigned long long)(a)[4], (unsigned long long)(a)[5]
    send_fmt("{\"id\":%d,\"ok\":true,"
             "\"seq\":%llu,\"has_disc\":%d,"
             "\"index\":%u,\"stat\":\"0x%02X\","
             "\"request\":\"0x%02X\","
             "\"irq_enable\":\"0x%02X\",\"irq_flag\":\"0x%02X\","
             "\"mode\":\"0x%02X\","
             "\"param_count\":%d,\"response_read\":%d,\"response_count\":%d,"
             "\"sector_available\":%d,\"sector_read_pos\":%d,\"sector_size\":%d,"
             "\"reading\":%d,\"read_msf\":[%d,%d,%d],"
             "\"read_cmd\":\"0x%02X\",\"read_delay\":%d,"
             "\"read_hold_cycles\":%llu,\"read_hold_events\":%llu,"
             "\"int1_pended\":%llu,\"int1_lost\":%llu,\"int1_pending_now\":%u,"
             "\"filter_file\":%u,\"filter_channel\":%u,\"muted\":%u,"
             "\"seek_msf\":[%u,%u,%u],"
             "\"pending\":{\"cmd\":\"0x%02X\",\"active\":%d,\"delay\":%d,\"phase\":%d},"
             "\"last_sector\":{\"lba\":%d,\"size\":%d,\"frame\":%u,"
             "\"mode\":\"0x%02X\",\"have_raw\":%u},"
             "\"speed_divisor\":%d,"
             "\"int_raised\":[%llu,%llu,%llu,%llu,%llu,%llu],"
             "\"int_presented\":[%llu,%llu,%llu,%llu,%llu,%llu],"
             "\"int_clobbered\":[%llu,%llu,%llu,%llu,%llu,%llu],"
             "\"int_lost_unseen\":[%llu,%llu,%llu,%llu,%llu,%llu],"
             "\"int_acked_unpresented\":[%llu,%llu,%llu,%llu,%llu,%llu],"
             "\"int_last_lost\":{\"old\":%u,\"new\":%u,\"gen\":%u},"
             "\"i_stat\":\"0x%08X\"}",
             id, (unsigned long long)s.seq, s.has_disc,
             s.index_reg, s.stat_reg, s.request_reg, s.irq_enable, s.irq_flag,
             s.mode_reg,
             s.param_count, s.response_read, s.response_count,
             s.sector_available, s.sector_read_pos, s.sector_size,
             s.reading, s.read_min, s.read_sec, s.read_sect,
             s.read_cmd, s.read_delay,
             (unsigned long long)s.read_hold_cycles,
             (unsigned long long)s.read_hold_events,
             (unsigned long long)s.int1_pended,
             (unsigned long long)s.int1_lost,
             s.int1_pending_now,
             s.filter_file, s.filter_channel, s.muted,
             s.seek_min, s.seek_sec, s.seek_sect,
             s.pending_cmd, s.pending_pending, s.pending_delay,
             s.pending_phase,
             s.last_sector_lba, s.last_sector_size, s.last_sector_frame,
             s.last_sector_mode, s.last_sector_have_raw,
             s.speed_divisor,
             CD_INT6(s.int_raised), CD_INT6(s.int_presented),
             CD_INT6(s.int_clobbered), CD_INT6(s.int_lost_unseen),
             CD_INT6(s.int_acked_unpresented),
             s.int_last_lost_old, s.int_last_lost_new, s.int_last_lost_gen,
             s.i_stat);
}

#undef CD_INT6

void handle_cdrom_sector_dump(int id, const char *json)
{
    int offset = json_get_int(json, "offset", 0);
    int len = json_get_int(json, "len", 128);
    if (offset < 0) offset = 0;
    if (len < 1) len = 1;
    if (len > 2340) len = 2340;

    uint8_t *bytes = (uint8_t *)malloc((size_t)len);
    if (!bytes) { send_err(id, "oom"); return; }

    CDROMSectorDebugState s;
    uint32_t got = cdrom_debug_copy_last_sector((uint32_t)offset,
                                                (uint32_t)len,
                                                bytes, &s);

    size_t bufsz = 512u + (size_t)got * 2u;
    char *buf = (char *)malloc(bufsz);
    if (!buf) {
        free(bytes);
        send_err(id, "oom");
        return;
    }

    size_t pos = 0;
    pos += snprintf(buf + pos, bufsz - pos,
                    "{\"id\":%d,\"ok\":true,"
                    "\"current\":{\"available\":%d,\"read_pos\":%d,\"size\":%d},"
                    "\"last\":{\"lba\":%d,\"size\":%d,\"frame\":%u,"
                    "\"mode\":\"0x%02X\",\"have_raw\":%u},"
                    "\"offset\":%d,\"len\":%u,\"hex\":\"",
                    id,
                    s.current_available, s.current_read_pos, s.current_size,
                    s.last_lba, s.last_size, s.last_frame,
                    s.last_mode, s.last_have_raw,
                    offset, got);
    for (uint32_t i = 0; i < got && pos + 3 < bufsz; i++) {
        pos += snprintf(buf + pos, bufsz - pos, "%02x", bytes[i]);
    }
    snprintf(buf + pos, bufsz - pos, "\"}");
    debug_server_send_line(buf);
    free(buf);
    free(bytes);
}

static void append_hex_bytes(char *buf, size_t bufsz, size_t *pos,
                             const uint8_t *bytes, uint32_t len)
{
    for (uint32_t i = 0; i < len && *pos + 3 < bufsz; i++) {
        *pos += snprintf(buf + *pos, bufsz - *pos, "%02x", bytes[i]);
    }
}

void handle_cdrom_sector_history(int id, const char *json)
{
    int count = json_get_int(json, "count", 64);
    if (count < 1) count = 1;
    if (count > CDROM_SECTOR_HISTORY_CAP) count = CDROM_SECTOR_HISTORY_CAP;

    int filter_lba = -1;
    char lba_str[32];
    if (json_get_str(json, "lba", lba_str, sizeof(lba_str))) {
        filter_lba = (int)hex_to_u32(lba_str);
    }

    const CDROMSectorHistoryEntry *entries = NULL;
    uint64_t total = cdrom_debug_get_sector_history(&entries);
    uint64_t oldest = (total > CDROM_SECTOR_HISTORY_CAP)
        ? total - CDROM_SECTOR_HISTORY_CAP : 0;

    size_t bufsz = 256u + (size_t)count * 760u;
    char *buf = (char *)malloc(bufsz);
    if (!buf) { send_err(id, "oom"); return; }

    size_t pos = 0;
    int emitted = 0;
    pos += snprintf(buf + pos, bufsz - pos,
                    "{\"id\":%d,\"ok\":true,\"total\":%llu,"
                    "\"oldest\":%llu,\"entries\":[",
                    id, (unsigned long long)total,
                    (unsigned long long)oldest);

    uint64_t seq = total;
    while (seq > oldest && emitted < count && pos < bufsz - 760) {
        seq--;
        const CDROMSectorHistoryEntry *e =
            &entries[seq % CDROM_SECTOR_HISTORY_CAP];
        if (e->seq != seq) continue;
        if (filter_lba >= 0 && e->lba != filter_lba) continue;

        pos += snprintf(buf + pos, bufsz - pos,
                        "%s{\"seq\":%llu,\"lba\":%d,\"size\":%d,"
                        "\"frame\":%u,\"mode\":\"0x%02X\","
                        "\"have_raw\":%u,\"raw_mode\":\"0x%02X\","
                        "\"xa_file\":%u,\"xa_channel\":%u,"
                        "\"xa_submode\":\"0x%02X\",\"xa_coding\":\"0x%02X\","
                        "\"data_delivered\":%u,\"xa_audio_delivered\":%u,"
                        "\"skip_reason\":%u,\"bytes_len\":%u,\"hex\":\"",
                        emitted ? "," : "",
                        (unsigned long long)e->seq, e->lba, e->size,
                        e->frame, e->mode, e->have_raw, e->raw_mode,
                        e->xa_file, e->xa_channel, e->xa_submode, e->xa_coding,
                        e->data_delivered, e->xa_audio_delivered,
                        e->skip_reason, e->bytes_len);
        append_hex_bytes(buf, bufsz, &pos, e->bytes, e->bytes_len);
        pos += snprintf(buf + pos, bufsz - pos, "\"}");
        emitted++;
    }

    pos += snprintf(buf + pos, bufsz - pos, "],\"emitted\":%d}", emitted);
    debug_server_send_line(buf);
    free(buf);
}

void handle_cdrom_sector_history_clear(int id, const char *json)
{
    (void)json;
    cdrom_debug_clear_sector_history();
    send_ok(id);
}

static const char *cdrom_command_kind_name(uint8_t kind)
{
    switch (kind) {
    case 'C': return "exec";
    case 'Q': return "queued";
    default: return "unknown";
    }
}

void handle_cdrom_command_history(int id, const char *json)
{
    int count = json_get_int(json, "count", 128);
    if (count < 1) count = 1;
    if (count > CDROM_COMMAND_HISTORY_CAP) count = CDROM_COMMAND_HISTORY_CAP;

    int frame_lo = json_get_int(json, "frame_lo", -1);
    int frame_hi = json_get_int(json, "frame_hi", -1);

    const CDROMCommandHistoryEntry *entries = NULL;
    uint64_t total = cdrom_debug_get_command_history(&entries);
    uint64_t oldest = (total > CDROM_COMMAND_HISTORY_CAP)
        ? total - CDROM_COMMAND_HISTORY_CAP : 0;

    size_t bufsz = 256u + (size_t)count * 640u;
    char *buf = (char *)malloc(bufsz);
    if (!buf) { send_err(id, "oom"); return; }

    size_t pos = 0;
    int emitted = 0;
    pos += snprintf(buf + pos, bufsz - pos,
                    "{\"id\":%d,\"ok\":true,\"total\":%llu,"
                    "\"oldest\":%llu,\"entries\":[",
                    id, (unsigned long long)total,
                    (unsigned long long)oldest);

    uint64_t seq = total;
    while (seq > oldest && emitted < count && pos < bufsz - 640) {
        seq--;
        const CDROMCommandHistoryEntry *e =
            &entries[seq % CDROM_COMMAND_HISTORY_CAP];
        if (e->seq != seq) continue;
        if (frame_lo >= 0 && (int)e->frame < frame_lo) continue;
        if (frame_hi >= 0 && (int)e->frame > frame_hi) continue;

        pos += snprintf(buf + pos, bufsz - pos,
                        "%s{\"seq\":%llu,\"frame\":%u,\"kind\":\"%s\","
                        "\"cmd\":\"0x%02X\",\"param_count\":%u,\"params\":[",
                        emitted ? "," : "",
                        (unsigned long long)e->seq, e->frame,
                        cdrom_command_kind_name(e->kind),
                        e->cmd, e->param_count);
        for (uint8_t i = 0; i < e->param_count && i < 16 && pos < bufsz - 96; i++) {
            pos += snprintf(buf + pos, bufsz - pos,
                            "%s\"0x%02X\"", i ? "," : "", e->params[i]);
        }
        pos += snprintf(buf + pos, bufsz - pos,
                        "],\"stat\":\"0x%02X\",\"request\":\"0x%02X\","
                        "\"irq_enable\":\"0x%02X\",\"irq_flag\":\"0x%02X\","
                        "\"mode\":\"0x%02X\",\"seek_msf\":[%u,%u,%u],"
                        "\"read_msf\":[%u,%u,%u],\"read_cmd\":\"0x%02X\","
                        "\"reading\":%u,\"pending_cmd\":\"0x%02X\","
                        "\"pending\":%u,\"queued_cmd\":\"0x%02X\","
                        "\"queued\":%u,\"func\":\"0x%08X\",\"pc\":\"0x%08X\","
                        "\"i_stat\":\"0x%08X\"}",
                        e->stat, e->request, e->irq_enable, e->irq_flag,
                        e->mode, e->seek_min, e->seek_sec, e->seek_sect,
                        e->read_min, e->read_sec, e->read_sect, e->read_cmd,
                        e->reading, e->pending_cmd, e->pending_pending,
                        e->queued_cmd, e->queued_pending, e->func, e->pc,
                        e->i_stat);
        emitted++;
    }

    pos += snprintf(buf + pos, bufsz - pos, "],\"emitted\":%d}", emitted);
    debug_server_send_line(buf);
    free(buf);
}

void handle_cdrom_command_history_clear(int id, const char *json)
{
    (void)json;
    cdrom_debug_clear_command_history();
    send_ok(id);
}

void handle_cdrom_trace_clear(int id, const char *json)
{
    (void)json;
    cdrom_debug_clear_trace();
    send_ok(id);
}

void handle_cdrom_trace_dump(int id, const char *json)
{
    int count = json_get_int(json, "count", 256);
    if (count < 1) count = 1;
    if (count > CDROM_TRACE_CAP) count = CDROM_TRACE_CAP;

    int frame_lo = json_get_int(json, "frame_lo", -1);
    int frame_hi = json_get_int(json, "frame_hi", -1);

    const CDROMTraceEntry *entries = NULL;
    uint64_t total = cdrom_debug_get_trace(&entries);
    uint64_t oldest = (total > CDROM_TRACE_CAP) ? total - CDROM_TRACE_CAP : 0;
    uint64_t start = (total > (uint64_t)count) ? total - (uint64_t)count : 0;
    if (start < oldest) start = oldest;

    size_t bufsz = 256u + (size_t)count * 360u;
    char *buf = (char *)malloc(bufsz);
    if (!buf) { send_err(id, "oom"); return; }

    size_t pos = 0;
    int emitted = 0;
    pos += snprintf(buf + pos, bufsz - pos,
                    "{\"id\":%d,\"ok\":true,\"total\":%llu,\"oldest\":%llu,\"entries\":[",
                    id, (unsigned long long)total, (unsigned long long)oldest);
    for (uint64_t seq = start; seq < total && pos < bufsz - 400; seq++) {
        const CDROMTraceEntry *e = &entries[seq % CDROM_TRACE_CAP];
        if (e->seq != seq) continue;
        if (frame_lo >= 0 && (int)e->frame < frame_lo) continue;
        if (frame_hi >= 0 && (int)e->frame > frame_hi) continue;
        pos += snprintf(buf + pos, bufsz - pos,
                        "%s{\"seq\":%llu,\"kind\":\"%s\",\"addr\":\"0x%08X\","
                        "\"val\":\"0x%08X\",\"w\":%u,\"func\":\"0x%08X\","
                        "\"pc\":\"0x%08X\",\"frame\":%u,\"i_stat\":\"0x%08X\","
                        "\"index\":%u,\"stat\":\"0x%02X\","
                        "\"request\":\"0x%02X\","
                        "\"irq_enable\":\"0x%02X\",\"irq_flag\":\"0x%02X\","
                        "\"mode\":\"0x%02X\","
                        "\"param\":%u,\"resp_read\":%u,\"resp_count\":%u,"
                        "\"sector_avail\":%u,\"sector_pos\":%d,\"sector_size\":%d,"
                        "\"pending_cmd\":\"0x%02X\",\"pending\":%u,"
                        "\"pending_delay\":%d,\"reading\":%u,"
                        "\"read_cmd\":\"0x%02X\",\"read_delay\":%d}",
                        emitted ? "," : "",
                        (unsigned long long)e->seq, cdrom_trace_kind_name(e->kind),
                        e->addr, e->val, (unsigned)e->width,
                        e->func, e->pc, e->frame, e->i_stat,
                        e->index_reg, e->stat_reg, e->request_reg, e->irq_enable, e->irq_flag,
                        e->mode_reg,
                        e->param_count, e->response_read, e->response_count,
                        e->sector_available, e->sector_read_pos, e->sector_size,
                        e->pending_cmd, e->pending_pending,
                        e->pending_delay, e->reading,
                        e->read_cmd, e->read_delay);
        emitted++;
    }
    pos += snprintf(buf + pos, bufsz - pos, "],\"emitted\":%d}", emitted);
    debug_server_send_line(buf);
    free(buf);
}

static const char *dma_trace_kind_name(uint32_t kind)
{
    switch (kind) {
    case 'S': return "start";
    case 'C': return "complete";
    case 'W': return "write";
    default: return "unknown";
    }
}

void handle_dma_state(int id, const char *json)
{
    (void)json;
    DMADebugState s;
    dma_debug_get_state(&s);

    char buf[2048];
    size_t pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos,
                    "{\"id\":%d,\"ok\":true,\"dpcr\":\"0x%08X\","
                    "\"dicr\":\"0x%08X\",\"channels\":[",
                    id, s.dpcr, s.dicr);
    for (int i = 0; i < 7 && pos < sizeof(buf) - 192; i++) {
        pos += snprintf(buf + pos, sizeof(buf) - pos,
                        "%s{\"ch\":%d,\"madr\":\"0x%08X\","
                        "\"bcr\":\"0x%08X\",\"chcr\":\"0x%08X\","
                        "\"active\":%u,\"remaining_words\":%u,"
                        "\"cycles_accum\":%u}",
                        i ? "," : "",
                        i, s.channels[i].madr, s.channels[i].bcr,
                        s.channels[i].chcr, s.channels[i].active,
                        s.channels[i].remaining_words,
                        s.channels[i].cycles_accum);
    }
    snprintf(buf + pos, sizeof(buf) - pos, "]}");
    debug_server_send_line(buf);
}

void handle_dma_trace_clear(int id, const char *json)
{
    (void)json;
    dma_debug_clear_trace();
    send_ok(id);
}

void handle_dma_trace_dump(int id, const char *json)
{
    int count = json_get_int(json, "count", 256);
    if (count < 1) count = 1;
    if (count > DMA_TRACE_CAP) count = DMA_TRACE_CAP;

    const DMATraceEntry *entries = NULL;
    uint64_t total = dma_debug_get_trace(&entries);
    uint64_t oldest = (total > DMA_TRACE_CAP) ? total - DMA_TRACE_CAP : 0;
    uint64_t start = (total > (uint64_t)count) ? total - (uint64_t)count : 0;
    if (start < oldest) start = oldest;

    size_t bufsz = 256u + (size_t)count * 512u;
    char *buf = (char *)malloc(bufsz);
    if (!buf) { send_err(id, "oom"); return; }

    size_t pos = 0;
    int emitted = 0;
    pos += snprintf(buf + pos, bufsz - pos,
                    "{\"id\":%d,\"ok\":true,\"total\":%llu,\"oldest\":%llu,\"entries\":[",
                    id, (unsigned long long)total, (unsigned long long)oldest);
    for (uint64_t seq = start; seq < total && pos < bufsz - 512; seq++) {
        const DMATraceEntry *e = &entries[seq % DMA_TRACE_CAP];
        if (e->seq != seq) continue;
        pos += snprintf(buf + pos, bufsz - pos,
                        "%s{\"seq\":%llu,\"frame\":%u,\"kind\":\"%s\",\"ch\":%u,"
                        "\"words\":%u,\"madr\":\"0x%08X\",\"bcr\":\"0x%08X\","
                        "\"chcr\":\"0x%08X\",\"dpcr\":\"0x%08X\","
                        "\"dicr_before\":\"0x%08X\",\"dicr_after\":\"0x%08X\","
                        "\"i_stat_before\":\"0x%08X\",\"i_stat_after\":\"0x%08X\","
                        "\"func\":\"0x%08X\",\"pc\":\"0x%08X\"}",
                        emitted ? "," : "",
                        (unsigned long long)e->seq, e->frame,
                        dma_trace_kind_name(e->kind), e->channel,
                        e->total_words, e->madr, e->bcr, e->chcr, e->dpcr,
                        e->dicr_before, e->dicr_after,
                        e->i_stat_before, e->i_stat_after,
                        e->func, e->pc);
        emitted++;
    }
    pos += snprintf(buf + pos, bufsz - pos, "],\"emitted\":%d}", emitted);
    debug_server_send_line(buf);
    free(buf);
}

static void append_word_array(char *buf, size_t bufsz, size_t *pos,
                              const uint32_t *words, int count)
{
    *pos += snprintf(buf + *pos, bufsz - *pos, "[");
    for (int i = 0; i < count && *pos < bufsz - 16; i++) {
        *pos += snprintf(buf + *pos, bufsz - *pos,
                         "%s\"0x%08X\"", i ? "," : "", words[i]);
    }
    *pos += snprintf(buf + *pos, bufsz - *pos, "]");
}

void handle_dma_cdrom_history(int id, const char *json)
{
    int count = json_get_int(json, "count", 256);
    if (count < 1) count = 1;
    if (count > DMA_CDROM_HISTORY_CAP) count = DMA_CDROM_HISTORY_CAP;

    int frame_lo = json_get_int(json, "frame_lo", -1);
    int frame_hi = json_get_int(json, "frame_hi", -1);
    int newest = json_get_int(json, "newest", 0) != 0;

    const DMACDROMHistoryEntry *entries = NULL;
    uint64_t total = dma_debug_get_cdrom_history(&entries);
    uint64_t oldest = (total > DMA_CDROM_HISTORY_CAP) ? total - DMA_CDROM_HISTORY_CAP : 0;

    size_t bufsz = 256u + (size_t)count * 1152u;
    char *buf = (char *)malloc(bufsz);
    if (!buf) { send_err(id, "oom"); return; }

    size_t pos = 0;
    int emitted = 0;
    pos += snprintf(buf + pos, bufsz - pos,
                    "{\"id\":%d,\"ok\":true,\"total\":%llu,\"oldest\":%llu,\"entries\":[",
                    id, (unsigned long long)total, (unsigned long long)oldest);

    uint64_t start = newest && total > (uint64_t)count ? total - (uint64_t)count : oldest;
    if (start < oldest) start = oldest;
    for (uint64_t seq = start; seq < total && emitted < count && pos < bufsz - 1152; seq++) {
        const DMACDROMHistoryEntry *e = &entries[seq % DMA_CDROM_HISTORY_CAP];
        if (e->seq != seq) continue;
        if (frame_lo >= 0 && (int)e->frame_start < frame_lo) continue;
        if (frame_hi >= 0 && (int)e->frame_start > frame_hi) continue;

        pos += snprintf(buf + pos, bufsz - pos,
                        "%s{\"seq\":%llu,\"frame_start\":%u,\"frame_end\":%u,"
                        "\"start_addr\":\"0x%08X\",\"final_addr\":\"0x%08X\","
                        "\"requested_words\":%u,\"moved_words\":%u,"
                        "\"bcr\":\"0x%08X\",\"chcr\":\"0x%08X\",\"dpcr\":\"0x%08X\","
                        "\"dicr_start\":\"0x%08X\",\"dicr_end\":\"0x%08X\","
                        "\"i_stat_start\":\"0x%08X\",\"i_stat_end\":\"0x%08X\","
                        "\"func\":\"0x%08X\",\"pc\":\"0x%08X\","
                        "\"lba\":%d,\"sector_size\":%d,"
                        "\"sector_read_pos_start\":%d,\"sector_read_pos_end\":%d,"
                        "\"mode\":\"0x%02X\",\"sector_available_start\":%u,"
                        "\"sector_available_end\":%u,\"completed\":%u,"
                        "\"first_words\":",
                        emitted ? "," : "",
                        (unsigned long long)e->seq, e->frame_start, e->frame_end,
                        e->start_addr, e->final_addr,
                        e->requested_words, e->moved_words,
                        e->bcr, e->chcr, e->dpcr,
                        e->dicr_start, e->dicr_end,
                        e->i_stat_start, e->i_stat_end,
                        e->func, e->pc,
                        e->lba, e->sector_size,
                        e->sector_read_pos_start, e->sector_read_pos_end,
                        e->mode, e->sector_available_start,
                        e->sector_available_end, e->completed);
        append_word_array(buf, bufsz, &pos, e->first_words, e->first_count);
        pos += snprintf(buf + pos, bufsz - pos, ",\"last_words\":");
        append_word_array(buf, bufsz, &pos, e->last_words, e->last_count);
        pos += snprintf(buf + pos, bufsz - pos, "}");
        emitted++;
    }

    pos += snprintf(buf + pos, bufsz - pos, "],\"emitted\":%d}", emitted);
    debug_server_send_line(buf);
    free(buf);
}

extern uint32_t gpu_get_opcode_count(uint8_t op);

extern int gpu_get_a0_count(void);
extern int gpu_get_a0_history(int index, int *x, int *y, int *w, int *h,
                              uint32_t *fw0, uint32_t *fw1, int *wcount);
extern int gpu_get_a0_extra(int index, uint32_t *func, uint32_t *sp, uint32_t *ra,
                            uint32_t *s1, uint32_t *stack10);
extern int gpu_get_a0_src(int index, uint32_t *s2, uint32_t *a0, uint32_t *a1, uint32_t *frame);

void handle_a0_history(int id, const char *json)
{
    (void)json;
    int count = gpu_get_a0_count();
    /* Use dynamic allocation for large output */
    int bufsz = 65536;
    char *buf = (char*)malloc(bufsz);
    if (!buf) { send_fmt("{\"id\":%d,\"ok\":false,\"error\":\"OOM\"}", id); return; }
    int pos = snprintf(buf, bufsz, "{\"id\":%d,\"ok\":true,\"count\":%d,\"uploads\":[", id, count);
    for (int i = 0; i < count && pos < bufsz - 500; i++) {
        int x, y, w, h, wcount;
        uint32_t fw0, fw1, func, sp, ra, s1, stk[10];
        uint32_t s2 = 0, a0r = 0, a1r = 0, aframe = 0;
        gpu_get_a0_history(i, &x, &y, &w, &h, &fw0, &fw1, &wcount);
        gpu_get_a0_extra(i, &func, &sp, &ra, &s1, stk);
        gpu_get_a0_src(i, &s2, &a0r, &a1r, &aframe);
        pos += snprintf(buf + pos, bufsz - pos,
            "%s{\"i\":%d,\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,"
            "\"fw0\":\"0x%08X\",\"fw1\":\"0x%08X\",\"words\":%d,"
            "\"func\":\"0x%08X\",\"sp\":\"0x%08X\",\"ra\":\"0x%08X\","
            "\"s1\":\"0x%08X\",\"s2\":\"0x%08X\",\"a0\":\"0x%08X\",\"a1\":\"0x%08X\",\"frame\":%u,"
            "\"stk\":[\"0x%08X\",\"0x%08X\",\"0x%08X\",\"0x%08X\","
            "\"0x%08X\",\"0x%08X\",\"0x%08X\",\"0x%08X\","
            "\"0x%08X\",\"0x%08X\"]}",
            i ? "," : "", i, x, y, w, h, fw0, fw1, wcount,
            func, sp, ra, s1, s2, a0r, a1r, aframe,
            stk[0], stk[1], stk[2], stk[3], stk[4], stk[5], stk[6], stk[7],
            stk[8], stk[9]);
    }
    pos += snprintf(buf + pos, bufsz - pos, "]}");
    send_fmt("%s", buf);
    free(buf);
}

extern int gpu_get_c0_count(void);
extern int gpu_get_c0_history(int index, int *x, int *y, int *w, int *h,
                              uint32_t *func, uint32_t *sp, uint32_t *s1,
                              uint32_t *fw0, uint32_t *fw1, int *rcount);

void handle_c0_history(int id, const char *json)
{
    (void)json;
    int count = gpu_get_c0_count();
    char buf[8192];
    int pos = snprintf(buf, sizeof(buf), "{\"id\":%d,\"ok\":true,\"count\":%d,\"reads\":[", id, count);
    for (int i = 0; i < count && pos < (int)sizeof(buf) - 300; i++) {
        int x, y, w, h, rcount;
        uint32_t func, sp, s1, fw0, fw1;
        gpu_get_c0_history(i, &x, &y, &w, &h, &func, &sp, &s1, &fw0, &fw1, &rcount);
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s{\"i\":%d,\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,"
            "\"func\":\"0x%08X\",\"sp\":\"0x%08X\",\"s1\":\"0x%08X\","
            "\"fw0\":\"0x%08X\",\"fw1\":\"0x%08X\",\"reads\":%d}",
            i ? "," : "", i, x, y, w, h, func, sp, s1, fw0, fw1, rcount);
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");
    send_fmt("%s", buf);
}

void handle_gpu_opcodes(int id, const char *json)
{
    (void)json;
    /* Report non-zero GP0 opcode counts */
    char buf[4096];
    int pos = snprintf(buf, sizeof(buf), "{\"id\":%d,\"ok\":true,\"opcodes\":{", id);
    int first = 1;
    for (int i = 0; i < 256; i++) {
        uint32_t cnt = gpu_get_opcode_count((uint8_t)i);
        if (cnt > 0) {
            pos += snprintf(buf + pos, sizeof(buf) - pos, "%s\"0x%02X\":%u",
                           first ? "" : ",", i, cnt);
            first = 0;
        }
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "}}");
    send_fmt("%s", buf);
}

void handle_gpu_ring_stats(int id, const char *json)
{
    (void)json;
    uint32_t oldest = 0, newest = 0;
    gpu_gp0_ring_frame_span(&oldest, &newest);
    send_fmt("{\"id\":%d,\"ok\":true,\"total\":%llu,\"capacity\":%u,"
             "\"max_words\":%u,\"oldest_frame\":%u,\"newest_frame\":%u}",
             id,
             (unsigned long long)gpu_gp0_ring_total(),
             gpu_gp0_ring_capacity(),
             gpu_gp0_ring_max_words(),
             oldest, newest);
}

void handle_gpu_frame_dump(int id, const char *json)
{
    int target = json_get_int(json, "frame", -1);
    if (target < 0) { send_err(id, "missing frame"); return; }
    int max_entries = json_get_int(json, "count", 8192);
    if (max_entries < 1)    max_entries = 1;
    if (max_entries > 65536) max_entries = 65536;

    GpuGp0RingEntry *entries = (GpuGp0RingEntry *)malloc(
        (size_t)max_entries * sizeof(GpuGp0RingEntry));
    if (!entries) { send_err(id, "alloc failed"); return; }

    int n = gpu_gp0_ring_dump_frame((uint32_t)target, entries, max_entries);

    /* ~190 bytes base + up to ~90 for the copy builder chain; budget conservatively. */
    size_t buf_sz = 256 + (size_t)n * 400u;
    char *buf = (char *)malloc(buf_sz);
    if (!buf) { free(entries); send_err(id, "alloc failed"); return; }

    size_t pos = (size_t)snprintf(buf, buf_sz,
        "{\"id\":%d,\"ok\":true,\"frame\":%u,\"count\":%d,\"max_words\":%u,\"entries\":[",
        id, (uint32_t)target, n, gpu_gp0_ring_max_words());

    for (int i = 0; i < n && pos < buf_sz - 256; i++) {
        const GpuGp0RingEntry *e = &entries[i];
        pos += (size_t)snprintf(buf + pos, buf_sz - pos,
            "%s{\"seq\":%u,\"op\":\"0x%02X\",\"n\":%u,"
            "\"src\":\"0x%08X\",\"ot\":%u,\"pc\":\"0x%08X\","
            "\"func\":\"0x%08X\",\"ra\":\"0x%08X\",\"w\":[",
            i ? "," : "", e->seq, e->opcode, e->n_words,
            e->src_addr, (unsigned)e->ot_rank, e->pc, e->func, e->ra);
        int show = e->n_words < GPU_GP0_RING_MAX_WORDS
                 ? e->n_words : GPU_GP0_RING_MAX_WORDS;
        for (int k = 0; k < show && pos < buf_sz - 32; k++) {
            pos += (size_t)snprintf(buf + pos, buf_sz - pos,
                "%s\"0x%08X\"", k ? "," : "", e->cmd[k]);
        }
        pos += (size_t)snprintf(buf + pos, buf_sz - pos, "]");
        if (e->opcode == 0x80) {
            pos += (size_t)snprintf(buf + pos, buf_sz - pos,
                ",\"csp\":\"0x%08X\",\"bld\":[", e->csp);
            for (int k = 0; k < 6 && e->bld[k] && pos < buf_sz - 32; k++)
                pos += (size_t)snprintf(buf + pos, buf_sz - pos,
                    "%s\"0x%08X\"", k ? "," : "", e->bld[k]);
            pos += (size_t)snprintf(buf + pos, buf_sz - pos, "]");
        }
        pos += (size_t)snprintf(buf + pos, buf_sz - pos, "}");
    }
    snprintf(buf + pos, buf_sz - pos, "]}");
    debug_server_send_line(buf);
    free(buf);
    free(entries);
}

void handle_capture_quads(int id, const char *json)
{
    (void)json;
    gpu_arm_shaded_quad_capture();
    send_ok(id);
}

void handle_get_quads(int id, const char *json)
{
    (void)json;
    const GpuSqCapEntry *entries;
    int count = gpu_get_shaded_quad_capture(&entries);
    char buf[8192];
    int pos = snprintf(buf, sizeof(buf), "{\"id\":%d,\"ok\":true,\"count\":%d,\"quads\":[", id, count);
    for (int i = 0; i < count && pos < (int)sizeof(buf) - 256; i++) {
        const GpuSqCapEntry *e = &entries[i];
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s{\"v\":[%d,%d,%d,%d,%d,%d,%d,%d],\"c\":[\"0x%06X\",\"0x%06X\",\"0x%06X\",\"0x%06X\"]}",
            i ? "," : "",
            e->vx[0], e->vy[0], e->vx[1], e->vy[1],
            e->vx[2], e->vy[2], e->vx[3], e->vy[3],
            e->color[0], e->color[1], e->color[2], e->color[3]);
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");
    send_fmt("%s", buf);
}

extern uint64_t gte_get_exec_count(void);

void handle_gte_state(int id, const char *json)
{
    (void)json;
    if (!debug_server_cpu()) { send_err(id, "no cpu"); return; }
    char buf[2048];
    int pos = snprintf(buf, sizeof(buf), "{\"id\":%d,\"ok\":true,\"gte_exec\":%llu,\"gte_ctrl\":[",
                       id, (unsigned long long)gte_get_exec_count());
    for (int i = 0; i < 32; i++) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%s\"0x%08X\"",
                       i ? "," : "", debug_server_cpu()->gte_ctrl[i]);
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "],\"gte_data\":[");
    for (int i = 0; i < 32; i++) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%s\"0x%08X\"",
                       i ? "," : "", debug_server_cpu()->gte_data[i]);
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");
    send_fmt("%s", buf);
}

/* Dump recent GTE RTPS/RTPT projections (inputs + outputs) from the always-on
 * GTE ring. {"cmd":"gte_ring_dump","count":N,"newest":1,"frame":F} — frame
 * optional (omit or -1 for all). Used to find flattened/degenerate character
 * projections and split game-code input bugs from GTE-math bugs. */
void handle_gte_ring_dump(int id, const char *json)
{
    extern unsigned long long gte_rtp_ring_total(void);
    extern int gte_rtp_ring_dump_json(char *out, int outsz, int max_count,
                                      int newest_first, long frame_filter);
    int count = json_get_int(json, "count", 64);
    if (count < 1) count = 1;
    if (count > 512) count = 512;
    int newest = json_get_int(json, "newest", 1) != 0;
    long frame = (long)json_get_int(json, "frame", -1);

    size_t BUF_SZ = 256u + (size_t)count * 720u;
    char *entries = (char *)malloc(BUF_SZ);
    char *reply   = (char *)malloc(BUF_SZ + 256u);
    if (!entries || !reply) { free(entries); free(reply); send_err(id, "oom"); return; }
    int n = gte_rtp_ring_dump_json(entries, (int)BUF_SZ, count, newest, frame);
    snprintf(reply, BUF_SZ + 256u,
             "{\"id\":%d,\"ok\":true,\"total\":%llu,\"emitted\":%d,\"entries\":[%s]}",
             id, gte_rtp_ring_total(), n, entries);
    debug_server_send_line(reply);
    free(entries); free(reply);
}

/* INTPL (vertex-lerp) ring: inputs (ir0 blend, in=IR1-3 pose A, fc=pose B)
 * and outputs (mac / out=IR1-3 / flag) per op. offset pages through the
 * matching entries after the frame filter, so a whole frame is reachable. */
void handle_gte_intpl_dump(int id, const char *json)
{
    extern unsigned long long gte_intpl_ring_total(void);
    extern int gte_intpl_ring_dump_json(char *out, int outsz, int max_count,
                                        int newest_first, long frame_filter,
                                        int offset);
    int count = json_get_int(json, "count", 64);
    if (count < 1) count = 1;
    if (count > 512) count = 512;
    int newest = json_get_int(json, "newest", 1) != 0;
    long frame = (long)json_get_int(json, "frame", -1);
    int offset = json_get_int(json, "offset", 0);
    if (offset < 0) offset = 0;

    size_t BUF_SZ = 256u + (size_t)count * 420u;
    char *entries = (char *)malloc(BUF_SZ);
    char *reply   = (char *)malloc(BUF_SZ + 256u);
    if (!entries || !reply) { free(entries); free(reply); send_err(id, "oom"); return; }
    int n = gte_intpl_ring_dump_json(entries, (int)BUF_SZ, count, newest, frame, offset);
    snprintf(reply, BUF_SZ + 256u,
             "{\"id\":%d,\"ok\":true,\"total\":%llu,\"emitted\":%d,\"offset\":%d,\"entries\":[%s]}",
             id, gte_intpl_ring_total(), n, offset, entries);
    debug_server_send_line(reply);
    free(entries); free(reply);
}

/* Per-frame GTE projection stats (nproj / nsat / nflat) over recent frames —
 * shows the alternating flat/normal render pattern. */
void handle_gte_frame_stats(int id, const char *json)
{
    extern int gte_fstat_dump_json(char *out, int outsz, int max_frames);
    int n = json_get_int(json, "frames", 120);
    if (n < 1) n = 1; if (n > 512) n = 512;
    size_t BUF = 256u + (size_t)n * 96u;
    char *body = (char *)malloc(BUF), *reply = (char *)malloc(BUF + 128u);
    if (!body || !reply) { free(body); free(reply); send_err(id, "oom"); return; }
    int emitted = gte_fstat_dump_json(body, (int)BUF, n);
    snprintf(reply, BUF + 128u, "{\"id\":%d,\"ok\":true,\"emitted\":%d,\"frames\":[%s]}",
             id, emitted, body);
    debug_server_send_line(reply); free(body); free(reply);
}

/* Latched degenerate (saturated-output) GTE projections with full inputs. */
void handle_gte_latch_dump(int id, const char *json)
{
    extern unsigned long long gte_latch_total(void);
    extern int gte_latch_dump_json(char *out, int outsz, int max_count);
    int n = json_get_int(json, "count", 64);
    if (n < 1) n = 1; if (n > 256) n = 256;
    size_t BUF = 256u + (size_t)n * 720u;
    char *body = (char *)malloc(BUF), *reply = (char *)malloc(BUF + 128u);
    if (!body || !reply) { free(body); free(reply); send_err(id, "oom"); return; }
    int emitted = gte_latch_dump_json(body, (int)BUF, n);
    snprintf(reply, BUF + 128u, "{\"id\":%d,\"ok\":true,\"latch_total\":%llu,\"emitted\":%d,\"entries\":[%s]}",
             id, gte_latch_total(), emitted, body);
    debug_server_send_line(reply); free(body); free(reply);
}

void handle_sio_state(int id, const char *json)
{
    (void)json;
    extern int sio_get_mc_probe_count(void);
    extern int sio_get_mc_ack_count(void);
    extern int sio_get_mc_cmd_count(void);
    extern int sio_get_mc_read_count(void);
    extern int sio_get_mc_read_done(void);
    extern uint32_t sio_get_mc_last_caller(void);
    extern int sio_get_mc_abort_count(void);
    extern int sio_get_mc_abort_state(void);
    extern uint16_t sio_get_mc_abort_ctrl(void);
    extern int sio_get_mc_max_state(void);
    extern int sio_get_tx_writes(void);
    extern int sio_get_tx_gated(void);
    extern uint16_t sio_get_last_ctrl_on_tx(void);
    send_fmt("{\"id\":%d,\"ok\":true,"
             "\"sio_stat\":\"0x%04X\","
             "\"sio_ctrl\":\"0x%04X\","
             "\"sio_rx\":\"0x%02X\","
             "\"pad_buttons\":\"0x%04X\","
             "\"mc_probes\":%d,"
             "\"mc_acks\":%d,"
             "\"mc_cmds\":%d,"
             "\"mc_reads\":%d,"
             "\"mc_read_done\":%d,"
             "\"mc_last_caller\":\"0x%08X\","
             "\"mc_aborts\":%d,"
             "\"mc_abort_state\":%d,"
             "\"mc_abort_ctrl\":\"0x%04X\","
             "\"mc_max_state\":%d,"
             "\"tx_writes\":%d,"
             "\"tx_gated\":%d,"
             "\"last_ctrl_on_tx\":\"0x%04X\"}",
             id,
             /* Side-effect-free peeks (sio_read pops the RX FIFO / clears ACK). */
             sio_peek_stat(),
             sio_peek_ctrl(),
             sio_peek_rx_data(),
             sio_get_pad_buttons(),
             sio_get_mc_probe_count(),
             sio_get_mc_ack_count(),
             sio_get_mc_cmd_count(),
             sio_get_mc_read_count(),
             sio_get_mc_read_done(),
             sio_get_mc_last_caller(),
             sio_get_mc_abort_count(),
             sio_get_mc_abort_state(),
             sio_get_mc_abort_ctrl(),
             sio_get_mc_max_state(),
             sio_get_tx_writes(),
             sio_get_tx_gated(),
             sio_get_last_ctrl_on_tx());
}

/* ---- Memory card disk-load status (per-slot) ---- */

void handle_mc_status(int id, const char *json)
{
    (void)json;
    const char *p0 = "", *p1 = "";
    char p0_json[1024], p1_json[1024];
    uint8_t m0[2] = {0,0}, m1[2] = {0,0};
    int pres0 = 0, pres1 = 0, dirty0 = 0, dirty1 = 0;
    memcard_debug_info(0, &p0, m0, &pres0, &dirty0);
    memcard_debug_info(1, &p1, m1, &pres1, &dirty1);
    json_escape_string(p0_json, sizeof(p0_json), p0);
    json_escape_string(p1_json, sizeof(p1_json), p1);
    send_fmt("{\"id\":%d,\"ok\":true,"
             "\"slot0\":{\"present\":%s,\"dirty\":%s,\"path\":\"%s\","
             "\"magic\":\"%c%c\",\"magic_hex\":\"%02X%02X\"},"
             "\"slot1\":{\"present\":%s,\"dirty\":%s,\"path\":\"%s\","
             "\"magic\":\"%c%c\",\"magic_hex\":\"%02X%02X\"}}",
             id,
             pres0 ? "true" : "false", dirty0 ? "true" : "false", p0_json,
             (m0[0] >= 0x20 && m0[0] < 0x7F) ? m0[0] : '?',
             (m0[1] >= 0x20 && m0[1] < 0x7F) ? m0[1] : '?',
             m0[0], m0[1],
             pres1 ? "true" : "false", dirty1 ? "true" : "false", p1_json,
             (m1[0] >= 0x20 && m1[0] < 0x7F) ? m1[0] : '?',
             (m1[1] >= 0x20 && m1[1] < 0x7F) ? m1[1] : '?',
             m1[0], m1[1]);
}

void handle_spu_status(int id, const char *json)
{
    (void)json;
    SpuDebugInfo info;
    spu_debug_info(&info);
    /* The DSP-fidelity state (issue #103: SPU IRQ, reverb, noise, sweeps) lives
     * in SpuGlobalState. Surfaced here so the whole SPU can be judged from one
     * always-on query — without it there is no way to tell whether the reverb
     * engine is actually stepping, whether the IRQ is armed, or which volume
     * registers are sweeping. */
    SpuGlobalState g;
    spu_get_global_state(&g);
    send_fmt("{\"id\":%d,\"ok\":true,"
             "\"ctrl\":\"0x%04X\",\"active_mask\":\"0x%06X\","
             "\"main_l\":%d,\"main_r\":%d,"
             "\"cd_l\":%d,\"cd_r\":%d,"
             "\"key_on_count\":%u,"
             "\"render_frames\":%llu,\"nonzero_frames\":%llu,"
             "\"last_peak\":%d,\"peak\":%d,"
             "\"cd_frames\":%u,\"cd_push_frames\":%llu,"
             "\"cd_overflow_frames\":%llu,\"cd_underflow_frames\":%llu,"
             "\"pmon\":\"0x%06X\",\"non\":\"0x%06X\",\"eon\":\"0x%06X\","
             "\"endx\":\"0x%06X\","
             "\"irq_flag\":%u,\"irq_addr\":\"0x%05X\","
             "\"reverb_on\":%u,\"reverb_mbase\":\"0x%05X\","
             "\"reverb_cur\":\"0x%05X\",\"capture_pos\":\"0x%03X\","
             "\"noise_lfsr\":\"0x%04X\","
             "\"sweep_l_mask\":\"0x%06X\",\"sweep_r_mask\":\"0x%06X\","
             "\"sweep_main\":%u,\"sfx_bus_mask\":\"0x%06X\"}",
             id,
             info.ctrl & 0xFFFFu,
             info.active_mask & 0xFFFFFFu,
             info.main_l,
             info.main_r,
             info.cd_l,
             info.cd_r,
             info.key_on_count,
             (unsigned long long)info.render_frames,
             (unsigned long long)info.nonzero_frames,
             info.last_peak,
             info.peak,
             info.cd_frames,
             (unsigned long long)info.cd_push_frames,
             (unsigned long long)info.cd_overflow_frames,
             (unsigned long long)info.cd_underflow_frames,
             g.pmon & 0xFFFFFFu,
             g.non  & 0xFFFFFFu,
             g.eon  & 0xFFFFFFu,
             g.endx & 0xFFFFFFu,
             (unsigned)g.irq_flag,
             g.irq_addr & 0xFFFFFu,
             (unsigned)g.reverb_on,
             g.reverb_mbase & 0xFFFFFu,
             g.reverb_cur & 0xFFFFFu,
             g.capture_pos & 0xFFFu,
             (unsigned)g.noise_lfsr,
             g.sweep_l_mask & 0xFFFFFFu,
             g.sweep_r_mask & 0xFFFFFFu,
             (unsigned)g.sweep_main,
             spu_get_sfx_bus_mask() & 0xFFFFFFu);
}

/* ---- Per-voice SPU snapshot. Mirrors fields the Beetle oracle exposes
 * via PS_SPU::GetRegister(GSREG_V0_*) so cross-process diff tooling sees
 * the same JSON schema on both port 4370 and 4380.
 *
 * Single-shot emission: assemble the entire response into a heap buffer
 * and fire one send_fmt. debug_server_send_line appends '\n' on every
 * call, so multi-call patterns produce multi-line garbage on the wire. */
void handle_spu_voices(int id, const char *json)
{
    (void)json;
    SpuGlobalState g;
    spu_get_global_state(&g);

    /* 24 voices x ~280 chars + header. Headroom matters: snprintf would silently
     * truncate mid-object and hand the caller unparseable JSON. */
    size_t cap = 16384;
    char *out = (char *)malloc(cap);
    if (!out) { send_fmt("{\"id\":%d,\"ok\":false,\"err\":\"alloc\"}", id); return; }
    size_t off = 0;
    int n = snprintf(out + off, cap - off,
        "{\"id\":%d,\"ok\":true,"
        "\"ctrl\":\"0x%04X\",\"main_l\":\"0x%04X\",\"main_r\":\"0x%04X\","
        "\"kon\":\"0x%06X\",\"koff\":\"0x%06X\","
        "\"pmon\":\"0x%06X\",\"non\":\"0x%06X\",\"eon\":\"0x%06X\","
        "\"endx\":\"0x%06X\",\"active_mask\":\"0x%06X\","
        "\"voices\":[",
        id,
        g.ctrl, g.main_vol_l, g.main_vol_r,
        g.kon_latch, g.koff_latch,
        g.pmon, g.non, g.eon,
        g.endx, g.active_mask);
    if (n > 0) off += (size_t)n;

    for (int v = 0; v < 24; v++) {
        SpuVoiceState s;
        spu_get_voice_state(v, &s);
        n = snprintf(out + off, cap - off,
            "%s{\"v\":%d,\"active\":%d,"
            "\"vol_l\":\"0x%04X\",\"vol_r\":\"0x%04X\","
            "\"pitch\":\"0x%04X\","
            "\"start\":\"0x%05X\",\"loop\":\"0x%05X\","
            "\"adsr_lo\":\"0x%04X\",\"adsr_hi\":\"0x%04X\","
            "\"cur_addr\":\"0x%05X\",\"repeat_addr\":\"0x%05X\","
            "\"flags\":\"0x%02X\",\"sample_idx\":%d,\"phase\":\"0x%04X\","
            "\"env\":\"0x%04X\",\"env_phase\":%d,"
            /* Live effective volumes. For a sweeping register (bit 15 set) the
             * vol_l/vol_r control words above say nothing about the current
             * level, so these are the only way to see a sweep actually glide. */
            "\"vol_cur_l\":%d,\"vol_cur_r\":%d}",
            v == 0 ? "" : ",",
            v, s.active,
            s.vol_ctrl_l, s.vol_ctrl_r,
            s.pitch,
            (uint32_t)s.start_lo << 3,
            (uint32_t)s.loop_lo  << 3,
            s.adsr_lo, s.adsr_hi,
            s.cur_addr, s.repeat_addr,
            s.last_flags, s.sample_idx, s.phase,
            s.env_level, s.adsr_phase,
            s.vol_cur_l, s.vol_cur_r);
        if (n > 0) off += (size_t)n;
    }
    n = snprintf(out + off, cap - off, "]}");
    if (n > 0) off += (size_t)n;
    send_fmt("%s", out);
    free(out);
}

/* ---- SPU RAM peek: {"addr":N,"len":M} -> hex bytes. Sample data is the
 * ground truth for voice-rail triage (what does a parked loop block hold?). */
void handle_spu_ram(int id, const char *json)
{
    uint32_t addr = (uint32_t)json_get_int(json, "addr", 0);
    int len = json_get_int(json, "len", 16);
    if (len < 1) len = 1;
    if (len > 4096) len = 4096;
    uint8_t bytes[4096];
    uint32_t got = spu_ram_peek(addr, bytes, (uint32_t)len);
    char *hex = (char *)malloc((size_t)got * 2u + 1u);
    if (!hex) { send_fmt("{\"id\":%d,\"ok\":false,\"err\":\"alloc\"}", id); return; }
    for (uint32_t i = 0; i < got; i++)
        snprintf(hex + i * 2u, 3u, "%02X", bytes[i]);
    send_fmt("{\"id\":%d,\"ok\":true,\"addr\":\"0x%05X\",\"len\":%u,\"hex\":\"%s\"}",
             id, addr, (unsigned)got, hex);
    free(hex);
}

/* ---- SPU event ring dump. Returns the most recent N events
 * (KEYON / KEYOFF / END_STOP / END_LOOP / IRQ) with frame timestamps. */
void handle_spu_events(int id, const char *json)
{
    int count = json_get_int(json, "count", 256);
    if (count < 1) count = 1;
    if (count > 4096) count = 4096;
    SpuEvent *evs = (SpuEvent *)malloc((size_t)count * sizeof(SpuEvent));
    if (!evs) { send_fmt("{\"id\":%d,\"ok\":false,\"err\":\"alloc\"}", id); return; }
    uint32_t got = spu_event_get(evs, (uint32_t)count);
    uint64_t total = spu_event_total();
    /* Index by SpuEventKind (spu.h). IRQ (=5) is not voice-attributable; the
     * ring stores voice=0xFF for it and `addr` is the byte address that matched
     * the programmed IRQ address. Keep this table in step with SpuEventKind or
     * a new kind renders as "?". */
    static const char *kind_names[6] = { "?", "KEYON", "KEYOFF", "END_STOP",
                                         "END_LOOP", "IRQ" };

    /* Worst case ~200 chars per event; 64 KB is plenty for 4096 events. */
    size_t cap = 256u + (size_t)got * 256u;
    char *out = (char *)malloc(cap);
    if (!out) { free(evs); send_fmt("{\"id\":%d,\"ok\":false,\"err\":\"alloc\"}", id); return; }
    size_t off = 0;
    int n = snprintf(out + off, cap - off,
        "{\"id\":%d,\"ok\":true,\"total\":%llu,\"count\":%u,\"events\":[",
        id, (unsigned long long)total, (unsigned)got);
    if (n > 0) off += (size_t)n;
    for (uint32_t i = 0; i < got; i++) {
        const SpuEvent *e = &evs[i];
        const char *kn = (e->kind < sizeof(kind_names) / sizeof(kind_names[0]))
                         ? kind_names[e->kind] : "?";
        n = snprintf(out + off, cap - off,
            "%s{\"seq\":%llu,\"frame\":%u,\"kind\":\"%s\",\"v\":%d,"
            "\"pitch\":\"0x%04X\",\"addr\":\"0x%05X\","
            "\"adsr_lo\":\"0x%04X\",\"adsr_hi\":\"0x%04X\","
            "\"vol_l\":\"0x%04X\",\"vol_r\":\"0x%04X\"}",
            i == 0 ? "" : ",",
            (unsigned long long)e->seq, e->frame, kn, (int)e->voice,
            e->pitch, e->addr,
            e->adsr_lo, e->adsr_hi,
            e->vol_l, e->vol_r);
        if (n > 0) off += (size_t)n;
    }
    n = snprintf(out + off, cap - off, "]}");
    if (n > 0) off += (size_t)n;
    send_fmt("%s", out);
    free(out);
    free(evs);
}

void handle_spu_events_reset(int id, const char *json)
{
    (void)json;
    spu_event_reset();
    send_fmt("{\"id\":%d,\"ok\":true}\n", id);
}
