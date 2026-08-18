# TCP Debug Server Commands

Protocol: **JSON over newline**, one object per line, responses on same connection.

- Request shape: `{"id": N, "cmd": "<command>", ...params}`
- Success: `{"id": N, "ok": true, ...data}`
- Failure: `{"id": N, "ok": false, "error": "<msg>"}`

There are **two live** servers, both implementing this protocol with overlapping command sets:

| Server | Port | Source |
|---|---|---|
| **Native** (our recompiled runtime) | `4370` | `runtime/src/debug_server.c` |
| **Beetle PSX** (oracle) | `4380` | `runtime/src/beetle_debug_server.c` |

> **DuckStation (port 4371) was retired as the oracle on 2026-05-05** and is no
> longer built from this repository — there is no `duckstation` entry in
> `.gitmodules`. The **D** column in the curated inventory below is historical
> and reflects DuckStation, not Beetle. For authoritative per-command
> native/Beetle coverage, use the generated
> [Complete command index](#complete-command-index-generated) at the bottom of
> this file, which is derived from the two servers' command tables.

The `debug_client.py` CLI can target either, or `compare` two at once to diff state live — that's how divergence hunts work.

```bash
python tools/debug_client.py <cmd> [args]           # native (port 4370)
python tools/debug_client.py --port 4380 <cmd>      # psx-beetle
python tools/debug_client.py --ds <cmd> [args]      # duckstation (port 4371)
python tools/debug_client.py compare <cmd>          # run on both, diff results
```

Commands without a bespoke CLI mapping pass through generically: extra
args of the form `key=value` become JSON fields (ints when numeric, else
strings), so every server command is reachable, e.g.
`debug_client.py --port 4370 gpu_frame_dump frame=14528 count=65536`.

---

## Command inventory

Columns: **N** = native, **D** = DuckStation oracle.

| Command | N | D | Params | Description |
|---|---|---|---|---|
| `ping` / `frame` | ✓ | ✓ | — | Heartbeat + current frame number |
| `get_registers` (`regs`) | ✓ | ✓ | — | All 32 GPRs + PC + HI + LO (native also: COP0 SR/Cause/EPC, I_STAT, I_MASK) |
| `read_ram` | ✓ | ✓ | `addr`, `len` | Read bytes from PS1 address space as hex string — up to the full 2 MB in ONE response line. `dump_ram` is an alias (the old chunked multi-line variant is gone: it broke the one-request/one-response protocol and wedged the server) |
| `write_ram` | ✓ | ✓ | `addr`, `val` | Write **one byte** to PS1 address space. Note the parameter is `val` (not `hex`), and the write is a single byte per call — this row previously documented both incorrectly |
| `read_scratch` |   | ✓ | `addr`, `len` | Read PS1 scratchpad (0x1F800000 region) |
| `read_vram` / `vram_peek` | ✓¹ | ✓ | `x`, `y`, `w`, `h` | Read 16-bit VRAM pixels (max 128×128) |
| `gpu_state` | ✓ | ✓ | — | Display area, display depth, draw offset, GPUSTAT, clip rect, xfer state |
| `input_lag` | ✓ |   | optional `reset` | In-process input-latency samples: for each press, the frame the HOST mask first showed it and the frame by which the guest had consumed it and presented, plus elapsed ms. Also reports live `vsync` / `low_latency_input`. **This is the only way to measure it** — see the `pad_probe` caveat below |
| `pad_probe` | ✓ |   | — | Current frame + host-visible pad mask + SIO-visible (guest) mask. ⚠ **Cannot measure input latency by polling.** Every request is serviced on the EMU thread, so both masks are read at the same instant, after the frame loop has already moved one to the other — it reports a 0-frame gap regardless of the real latency. Use `input_lag`. Fine for "is the host seeing my pad at all" |
| `osd_toast` | ✓ |   | `msg`, optional `ms` | Push a host OSD toast, so the OSD can be exercised without waiting for a controller hotplug or savestate |
| `pad_devices` | ✓ |   | — | SDL's joystick/gamepad enumeration (`index`, `is_gamepad`, `guid`, `instance`) **plus** how each player slot is routed (`kind` none/keyboard/controller, `guid`, `open`, `opened_name`, `mode`). Separates the three ways "my pad does nothing" can happen — SDL not enumerating it, enumerating it with no gamepad mapping (`is_gamepad:0`), or a slot routed to the keyboard so no handle is opened. Note `pad_status` shows only the PSX-visible pad, not the host device |
| `menu_state` | ✓ |   | — | F10 overlay menu's own state: `visible`, `expanded`, `menu`/`item`/`rows`, `hover_menu`/`hover_row`, `editing`, `dirty`, plus the `logical_w`/`logical_h`/`ui_scale` layout. **Presented pixels cannot answer "is the dropdown open?"** — an open dropdown and a mere hover highlight differ only by shade — so check this, never a screenshot |
| `menu_click` / `menu_move` | ✓ |   | `x`, `y` | Drive the overlay menu by pushing a real `SDL_MOUSEBUTTONDOWN`/`SDL_MOUSEMOTION`, in **window** pixels. Same code path as a physical mouse (same window→drawable scaling, same handlers). Needed because host-level `SendInput`/`mouse_event` delivers motion to this window but **not** button presses, which silently makes every "I clicked the menu" test a no-op |
| `menu_key` | ✓ |   | `key` | Push an `SDL_KEYDOWN` with the given SDL_Keycode (F10 = `1073741891` / `0x40000043`). Note SDL3 keycodes: function keys are `0x40000000 \| scancode`, not ASCII |
| `screenshot_hires` |   | ✓ | `path` | PNG of the **supersampled** surface (the present path the window uses), at `display × gr_scale()`. ⚠ `screenshot`/`screenshot_file` capture native 15-bit VRAM and are **blind to anything that only exists in the hi-res mirror** — geometry correction, SSAA edges, perspective UVs — so they show a clean frame while the player sees a broken one. Use this one to verify those. Falls back to the native resolve (and reports `scale: 1`) when no hi-res surface exists |
| `geom_correction` |   | ✓ | `geometry` `perspective` (optional) | `[video] geometry_correction` / `perspective_texturing` engagement, and the only way to flip either knob live. Passing `geometry` / `perspective` (0 or 1) applies it through the same setters startup uses, then reports; omit both to only read. This is what makes the same-scene off-vs-on comparison ENHANCEMENTS.md G1.6 requires cost two commands instead of two runs. Reports `lookups` / `geometry_vertex_hits` / `miss_unrecorded` / `miss_ambiguous` (a high ambiguous share means distinct vertices are landing on the same pixel — irreducible, see G1.4/G1.10) plus the perspective rejection split `persp_attempts` / `persp_no_source` / `persp_no_provenance` / `persp_zero_z`, which says WHY a title reports zero perspective triangles. Counters are free-running: sample twice and diff for a rate, and note coverage varies by >10x between scenes in one title, so always record which scene |
| `sio_state` | ✓ | ✓ | — | SIO registers + (native only) pad/memcard protocol + TX/RX history |
| `irq_state` | ✓ | ✓ | — | `I_STAT`, `I_MASK` (both), plus chain state on native |
| `dma_state` | ✓ | ✓ | — | DPCR, DICR, all 7 channel states (madr/bcr/chcr) |
| `event_state` |   | ✓ | — | EvCB table summary (stub on DS — events are BIOS-level) |
| `overlay_state` |   | ✓ | — | Current overlay info |
| `cdrom_sector_dump` | ✓ |   | `offset`, `len` | Dump bytes from the last CD-ROM sector observed by the controller, including LBA/mode metadata |
| `cdrom_sector_history` | ✓ |   | `count`, optional `lba` | Dump newest CD-ROM sector history entries, including raw XA subheader fields, CPU/audio delivery flags, and the first 128 bytes |
| `cdrom_sector_history_clear` | ✓ |   | — | Reset the CD-ROM sector history ring |
| `watch` | ✓ | ✓ | `addr` | Set byte-level memory watchpoint (fires per-frame on change) |
| `unwatch` | ✓ | ✓ | `addr` | Remove memory watchpoint |
| `set_input` | ✓ | ✓ | `buttons`, optional `frames`, optional `lx`, `ly`, `rx`, `ry` | Override pad1 buttons and optional analog axes (PS1 inverted bitmask, 0 = pressed; axes 0-255). Holds until `clear_input` on both backends; pass `frames=N` (beetle) to auto-release after N frames |
| `clear_input` | ✓ | ✓ | — | Remove input and analog axis overrides |
| `turbo` | ✓ |   | `enabled` | Enable/disable TCP-controlled frontend turbo for fast-forward validation |
| `turbo_state` | ✓ |   | — | Query TCP-controlled turbo state |
| `pause` | ✓ |   | — | **REMOVED** — still registered, but always returns an error. Query a ring buffer (`fn_entry_tail`, `wtrace_dump`, `gpu_frame_dump`) instead of synthesizing a snapshot |
| `continue` (`c`) | ✓ |   | — | **REMOVED** — nothing to resume, since `pause` is gone |
| `step` | ✓ |   | — | **REMOVED** — query a ring buffer over the window of interest instead of advancing N frames synchronously |
| `run_to_frame` | ✓ |   | — | **REMOVED** — use `frame_range` / `read_frame_ram` against the live frame ring instead |
| `history` | ✓ | ✓ | — | Ring buffer stats (frames available) |
| `get_frame` | ✓ | ✓ | `frame` | Full frame record from ring buffer |
| `frame_range` | ✓ | ✓ | `start`, `end` | Range query, max 200 frames |
| `frame_timeseries` | ✓ | ✓ | `start`, `end` | Compact timeseries, max 200 frames |
| `set_snapshot` | ✓ | ✓ | `slot`, `addr`, `size` | Configure per-frame RAM snapshot region (slots 0-3) |
| `get_snapshots` | ✓ | ✓ | — | Show snapshot config |
| `screenshot` | ✓ | ✓ | `path` (optional) | Write a **PNG** of the current display to `path` (default `psx_screenshot.png` in the runtime cwd); single metadata response `{path,width,height}`. `screenshot_file` is an alias; the old inline-hex-row `screenshot` is gone (it streamed h+1 response lines per request and poisoned the connection) |
| `first_failure` | ✓ |   | — | Find first divergence point between runs (native-side tracking) |
| `read_frame_ram` | ✓ |   | `addr`, `len`, `frame` | Read RAM **as of a specific frame** (from ring buffer) |
| `wtrace_range` | ✓ |   | `lo`, `hi` | Set RAM-write trace range (ring of 262 144 writes with RA — `WRITE_TRACE_CAP`, `1 << 18`) |
| `wtrace_dump` | ✓ | beetle | optional `addr_lo`, `addr_hi`, `count`, `newest` | Dump RAM-write trace entries as JSON. The address filter is applied server-side over the FULL ring before the emit cap — always pass it when hunting a specific buffer, otherwise you only see the oldest `count` entries of the whole ring |
| `wtrace_clear` | ✓ |   | — | Reset the trace ring |
| `mmio_dump` | ✓ |   | optional `addr`, `count`, `newest` | Dump the always-on MMIO write ring (256K entries, ALL 0x1F801xxx writes — SPU/DMA traffic rolls it in well under a minute of gameplay; for display history use `gp1_dump`) |
| `mmio_clear` | ✓ |   | — | Reset the MMIO write ring |
| `gp1_dump` | ✓ |   | optional `frame_lo`, `frame_hi`, `count`, `newest` | Dump the dedicated ALWAYS-ON GP1 (0x1F801814 display control) ring — 512K entries ≈ 15 min of gameplay (Tomba writes ~10 GP1/frame), survives the general MMIO ring's eviction. Frame filter is server-side over the full ring. Each entry: val + func/pc/cpu_pc/ra/sp/a0/a1/sr/epc/frame |
| `pc_break` |   | ✓² | `addr` | DS execute breakpoint, state captured on hit (via `pc_hit_last`) |
| `pc_unbreak` |   | ✓² | `addr` | Remove an execute breakpoint |
| `pc_break_list` |   | ✓² | — | List active execute breakpoints |
| `pc_hit_last` |   | ✓² | — | Captured state (PC, $ra, all GPRs, COP0) from most recent PC break hit |
| `pc_hit_clear` |   | ✓² | — | Clear the last-hit record |
| `quit` | ✓ |   | — | Shutdown native runtime |

¹ Native `vram_peek` is the legacy name; DS calls it `read_vram`. Same semantics.
² The `pc_*` family is specific to the DS oracle: DuckStation's CPU core honours `CPU::AddBreakpointWithCallback`, while our native runtime dispatches whole recompiled functions (no mid-function PC breaks).

### Boot-time write ranges

Set `PSX_WTRACE_BOOT=lo,hi[;lo,hi...]` before launching a debug-tools build to
retain the first writes to one or more half-open RAM ranges from guest
instruction zero. Addresses may be hexadecimal or decimal; KSEG addresses are
normalized to physical addresses. For example, the Crash Bash investigation
that motivated this option can be reproduced without title-specific code:

```powershell
$env:PSX_WTRACE_BOOT='0x000B3A80,0x000B3B00'
.\CrashBashRecomp.exe
```

Connect at any later point and query `wtrace_boot_stats`,
`wtrace_boot_summary`, or `wtrace_boot_dump`. Each retained entry includes the
write address/value/width, guest PC and return address, register context, frame,
and DMA channel. The option is ignored in builds made with debug tools disabled.

---

## Divergence-hunt workflow

When a recompiled-BIOS bug is suspected, the two servers let you find the **first** divergence instead of chasing symptoms. Standard procedure (inherited from v3's `DEBUG.md`):

1. **Sync state via PC + registers, not frame number.** Frames drift after even a single timing glitch. Pause both servers; compare `get_registers` until they match.
2. **Dump both sides fully.** Compare `get_frame`, `gpu_state`, `irq_state`, `dma_state` (DS), `dump_ram` over the same regions.
3. **Byte-level comparison.** Tiny mismatches usually point at one subsystem. Use `debug_client.py compare <cmd>` for automatic diff.
4. **Find the earliest mismatch**, not a later symptom. Ring-buffer queries (`frame_range`, `read_frame_ram`) help locate which frame went wrong.
5. **Trace the write.** Use `watch` to catch the divergent store, or DS's `pc_break` on the suspect function entry. Look at `$ra` in `pc_hit_last` to identify the caller chain.
6. **Classify.** codegen (recompiler generates wrong instruction), runtime (MMIO or kernel simulation wrong), timing (IRQ cadence), or BIOS (real-hardware quirk we didn't model).
7. **Minimal fix** in the correct subsystem. Never hand-deliver state to hide the symptom (see CLAUDE.md §0).

---

## Server send budget + serve-stall telemetry

The server is pumped on the **main thread**: every millisecond it spends
sending a response is a millisecond the emulator does not run. Inline
responses are bounded — 2 s per zero-progress chunk and **15 s total per
response**; a client that exceeds the budget is disconnected (the runtime
never stalls indefinitely). Responses bigger than the budget allows must
use the `*_dump_file` variants, which write to disk instead of the socket.

Cumulative serve-stall is exported as `tcp_send_stall_ms` /
`tcp_clients_dropped` in `psx_freeze_heartbeat.json` (plus per-tick
`tcp_ms` in its ring) and in every wedge/fatal dump header. **Check these
first when diagnosing slow or stalled frames** — on 2026-06-10 two
"attract-idle degradations" turned out to be a TCP client trickle-draining
mega-dumps, throttling the main loop to 6 fps (all 8 watchdog stack
samples inside `WS2_32!send`). A slow-frames wedge with a large
`tcp_send_stall_ms` delta over the same window is observer interference,
not a guest bug.

## Call-contract (bail) telemetry

The dispatch call contract (Bug D family fix, 2026-06-10) guards every
generated continuation: it may only run if the guest actually returned to
the call site ($ra == site return address, $sp == caller's sp at the
call). Violations begin a "bail" unwind that abandons stale C frames and
re-dispatches the guest's true target. Counters in
`psx_freeze_heartbeat.json` and dump headers:

- `bail_first` — contract violations detected (wild returns). Nonzero
  during gameplay means the game executed a wild control transfer (e.g.
  Tomba's dead jumptable case `jal 0x80120B3C`, the chest freeze). A
  small count with the game continuing normally is the fix working.
- `bail_resolved` — unwinds that resolved at an enclosing call site whose
  contract matched (multi-level return).
- `bail_flattened` — unwinds that reached the outermost dispatch loop and
  re-dispatched the wild target on a clean host stack.
- `bail_anomaly` — bail flag observed at exception entry (must stay 0;
  anything else is a runtime bug).

---

## `bios_info` — linked recompiled-BIOS identity (native only)

Reports which BIOS image this build's recompiled C was generated from
(`psx_bios_image`, emitted into the generated dispatch from the BIOS profile)
and whether the loaded ROM matches it: `image_id`, `sha256`, `crc32`, `size`,
`bundled` (redistributable image shipped with the game), the kernel-bless
window, the HLE anchors (`shell_entry_phys` / `deliver_event_ret`; 0 =
structurally unavailable on this BIOS), and `image_wordsum` vs
`loaded_wordsum` with `match`. With the launch identity gate a running
process always reports `match:1`.

- `{"cmd":"bios_info"}`

## `s3_smear_watch` — callee-saved-register smear tripwire (native only)

Latches the first interpreted instruction in a PC window whose execution
changes `$s3` (`runtime/src/dirty_ram_interp.c`). A `jalr`'s exec_one spans
the entire nested native callee, so the latch names the callee that returned
with a clobbered callee-saved register; the insn ring is frozen at the latch.

- `{"cmd":"s3_smear_watch","lo":"<hex>","hi":"<hex>"}` — arm (each arming
  fully re-specifies the watch). Optional `"excl":"<hex insn>"`: exact
  encoding to ignore, so a watched loop's own `$s3` advance (e.g. an
  `addi s3,s3,8` list walk) doesn't trip the latch.
- `{"cmd":"s3_smear_watch"}` — report the latch: `valid`, `pc`, `insn`,
  `s3_old`/`s3_new`, `call_target` (rs at the call site for jr/jalr),
  `frame`.
- `{"cmd":"s3_smear_watch","lo":"0"}` — disarm.

## `callret_watch` — interp JALR call-resolution ring (native only)

64-entry ring (`runtime/src/dirty_ram_interp.c`) recording, for every
interpreted JALR whose call PC lies in a window, which resolution tier ran
the callee and the full post-call outcome — the complement of
`s3_smear_watch`: the tripwire names the callee that came back smeared, this
ring names the return path that let it come back.

- `{"cmd":"callret_watch","lo":"<hex>","hi":"<hex>"}` — arm (resets the ring).
- `{"cmd":"callret_watch"}` — dump (newest last): per entry `cyc`, `f`
  (frame), `pc`, `tgt`, `path` (`CRES_*` tier code, see the enum in
  dirty_ram_interp.c; `|0x100` = finish() escaped), pre-call
  `sp_b`/`ra_b`/`s0_b`/`s3_b`, post-call `pc_a`/`ra_a`/`sp_a`/`s0_a`/
  `s3_a`/`v0_a`, `bail`/`rfe`/`esc`/`in_exc` flags, `dstatic`/`dblocks`/
  `dexc` engine-attribution deltas across the call, `last_func`.
- `{"cmd":"callret_watch","lo":"0"}` — disarm.

## `hle_dump` — BIOS-HLE tier call ring (native only)

Always-on ring (`runtime/src/bios_hle.c`, 16K entries) recording every
A0/B0/C0 kernel-vector dispatch the HLE tier's hook observes, plus the boot
shell-skip event.

The hook is installed when EITHER axis is on (`bios_hle_plan.h`), so a run with
the boot-skip on but kernel calls left to LLE — the default on the bundled
OpenBIOS, which exports no `deliver_event_ret` — reports
`backend: "LLE (recompiled BIOS)"` and still fills the ring with `route: 0`
vector observations plus the one `route: 2` boot entry. Only a run with BOTH axes
off installs no hook and leaves the ring empty; use `bioscall_dump` for LLE-side
vector observation there.

- `{"cmd":"hle_dump"}` — status: `backend` (`HLE (LLE fallback)` /
  `LLE (recompiled BIOS)`), `boot_skip`, `boot_turbo_active`, `total`.
- `{"cmd":"hle_dump","tail":N}` — last N entries: `seq`, `cycle` (guest
  cycle), `vec` (0xA0/0xB0/0xC0, or 0x30000 for the boot skip), `fn` ($t1
  function number), `a0..a3`, `ra`, `v0` (result when HLE-serviced), `route`
  (0 = fell through to LLE, 1 = serviced in HLE, 2 = boot shell-skip).
- Filters: `"fn":N`, `"route":0|1|2`.

---

## `gl_interp` / `interp_dump` — frame-interpolation state and history (native only)

`gl_interp` reports the presentation-only interpolation state, and takes live
knobs so a defect hunt costs commands instead of rebuilds (each rebuild costs
the player their place in the game):

- `enable=<0|1>`, `fps=<0|90..1000>` — **engage or drop interpolation live**,
  without a relaunch. `fps=0` means display refresh; omitting `fps` keeps the
  current target. Interpolation is otherwise chosen before the window exists
  (env / mod / `menu_settings.ini` / the F10 FRAME RATE row), so every A/B used
  to cost a full intro — and "did the env var actually reach the process"
  silently invalidated two measurements on 2026-08-16. Check `enabled` in the
  reply rather than assuming the launch took.

- `state_fix=<0|1>` — the guard that normalises the **presentation context's**
  GL state before the interpolated draw. That context is touched by exactly two
  things: the interpolated quad and the OSD compositor — and
  `gl_draw_osd_image_ex(blend=1)` (the F10 menu bar) leaves `GL_BLEND` armed
  with nobody restoring it. The main thread is accidentally immune because
  `hr_end()` disables blend every guest frame. `state_fix=0` reproduces the
  unguarded draw for an A/B.
- `alpha=<-1..1>` — pin the crossfade factor. `1` shows the current capture
  only, `0` the previous one, `-1` restores the frame clock. Pinning separates
  "the blend is wrong" from "the captured image is wrong" without reading back
  a single pixel.

Reply fields beyond the basics: `captures`, `tex_w`/`tex_h`/`scale`,
`src_*` (the VRAM rect the last capture copied), `source_path`, and
**`draw_blend` / `draw_blend_src` / `draw_blend_dst`** — the blend state the
last interpolated draw actually *inherited*, sampled before the guard runs, so
it still reports a leak while the guard is on.

`interp_dump` writes the history to PNG — all three images from one instant:

| file | what |
|---|---|
| `<prefix>_src.png` | the hr FBO re-read LIVE at the last capture's rect (what `interp_capture` copies FROM) |
| `<prefix>_prev.png` | the previous history texture |
| `<prefix>_curr.png` | the current history texture |
| `<prefix>_*_a.png` | the same images' **alpha channel** as greyscale (`alpha=0` to skip) |

The alpha planes are the point, not an extra: alpha on this path is the **PSX
mask bit**, not coverage. A layer that is present in RGB but sits at alpha 0 is
erased — and *only* it is erased — by any blend left armed in the presentation
context, which is precisely the "one background layer disappeared" symptom.
Colour-only dumps cannot see that. The reply reports `*_alpha_hi_pct` (percent
of pixels with alpha >= 128) and `*_mean` per image, so the split is often
readable without opening the files.

Reading it: **src present + history missing** → capture/sync (the textures are
shared across two GL contexts and gated by `glFenceSync`/`glWaitSync`).
**Both present** → the defect is in the draw/present.

Args: `prefix` (default `psx_interp`, relative to the exe's working directory),
`alpha=<0|1>`. Requires interpolation enabled with a full history pair.

---

## `frame_gate` — unlock a title's self-imposed frame cap (native only)

Most PS1 games cap themselves by waiting for N vblanks on a counter their own
VSync callback increments. `frame_gate` injects extra ticks into that counter
once per real vblank, so the wait is satisfied sooner and the game's main loop
runs faster.

    frame_gate addr=0x80092AC8 extra=1     engage (Yu-Gi-Oh! FM: 30 -> 60)
    frame_gate extra=0                     off

`addr` is always supplied by the caller — the counter is a game symbol, so this
stays a framework tool rather than a per-game hack. Reply reports `ticks`
(injections made) and the counter's live value.

**Why not just speed up host pacing:** host pacing accelerates the whole
machine, including whatever ticks the sound driver, so the music tempo rides
along — the same reason FAST LOADING drives the CD sector delay instead.
Injecting into the guest's own frame counter moves only the thing that waits on
it; anything clocked off the vblank INTERRUPT keeps real time. Whether a given
title's audio actually sits on that side of the line is a per-title fact, and
measuring it is the point of the command.

**Reading the result** (listen as well as look):

| observed | means |
|---|---|
| loop faster, music tempo unchanged | loop and audio clocks are separable — what a "speed up the game" feature needs |
| loop faster, music faster too | the sound driver rides the main loop; isolate it before shipping any speed option |
| game runs at 2x overall | logic is per-iteration, so *smoother motion* (rather than *faster play*) additionally needs every per-frame delta halved |
| nothing moves | the cap is not this counter, or there is a second gate |

**Finding the counter for a new title:** histogram the PC (`get_registers` in a
loop) — the samples pile up in the wait loop. Read that routine in
`generated/`, and the load it polls gives the counter (FM: PC clustered at
`0x80074310`/`0x80074358`, the routine loaded `base+10952` off `0x80090000`).
Confirm with `fntrace_arm` on the wait: the `a0` targets should advance by the
cap (FM: +2 per loop, 60 entries/s).

---

## Rule when the server can't answer your question

If an inspection need isn't covered by the existing commands, **do not fall back to printf or log files**. Instead:

1. Add a handler in `runtime/src/debug_server.c` (native)
2. Add the matching handler in `runtime/src/beetle_debug_server.c` (Beetle oracle)
   when the question needs a cross-check against hardware behaviour
3. Keep field names parallel between the two
4. Run `python tools/gen_tcp_commands.py` to refresh the generated index, and add
   a row to the curated inventory above if the command needs explaining

> Step 4 used to read "Update this file", and that did not survive contact with
> reality: this document described 47 commands while the servers registered 292.
> The index is now generated from the command tables, and
> `python tools/gen_tcp_commands.py --check` fails when it drifts. Prose in the
> curated inventory is still hand-written and still worth adding.

The TCP server is the canonical instrumentation surface. Rule 3 in `CLAUDE.md` is absolute: **no `fprintf(stderr, …)` in source code, ever, for any reason**.

<!-- BEGIN AUTOGENERATED COMMAND INDEX -- edit tools/gen_tcp_commands.py, not this block -->

## Complete command index (generated)

**292 commands registered** — 279 on the native server (`runtime/src/debug_server.c`), 61 on the Beetle server (`runtime/src/beetle_debug_server.c`).

47 of 292 have prose above; **245 are index-only**. An index-only command still works — it just has no description here yet. Send it `{"cmd":"<name>"}` and read the reply, or find its `handle_*` function in the server source.

Regenerate with `python tools/gen_tcp_commands.py`; `--check` fails if this block has drifted from the code.

| Command | Native | Beetle | Described above |
|---|:--:|:--:|:--:|
| `a0_history` | ✓ |  |  |
| `audio_events` | ✓ | ✓ |  |
| `audio_stats` | ✓ | ✓ |  |
| `audio_wav` | ✓ | ✓ |  |
| `autocompile_run` | ✓ |  |  |
| `autocompile_status` | ✓ |  |  |
| `bios_info` | ✓ |  | ✓ |
| `bioscall_dump` | ✓ |  |  |
| `c0_history` | ✓ |  |  |
| `call_focus_dump` | ✓ |  |  |
| `call_focus_reset` | ✓ |  |  |
| `call_focus_stats` | ✓ |  |  |
| `callret_watch` | ✓ |  | ✓ |
| `capture_freeze` | ✓ |  |  |
| `capture_quads` | ✓ |  |  |
| `card_buffer_dump` | ✓ |  |  |
| `card_data_writes` | ✓ |  |  |
| `card_data_writes_reset` | ✓ |  |  |
| `card_mgr_clear` | ✓ |  |  |
| `card_mgr_trace` | ✓ |  |  |
| `card_read_summary` | ✓ |  |  |
| `card_read_summary_reset` | ✓ |  |  |
| `card_trace_dump` | ✓ |  |  |
| `card_txn_dump` | ✓ |  |  |
| `cd_overwrite` | ✓ |  |  |
| `cd_read_log` | ✓ |  |  |
| `cdc_volume` |  | ✓ |  |
| `cdrom_bursts` | ✓ |  |  |
| `cdrom_cmd_dump` |  | ✓ |  |
| `cdrom_cmd_reset` |  | ✓ |  |
| `cdrom_command_history` | ✓ |  |  |
| `cdrom_command_history_clear` | ✓ |  |  |
| `cdrom_instant_rate` | ✓ |  |  |
| `cdrom_sector_dump` | ✓ |  | ✓ |
| `cdrom_sector_history` | ✓ |  | ✓ |
| `cdrom_sector_history_clear` | ✓ |  | ✓ |
| `cdrom_state` | ✓ |  |  |
| `cdrom_timing` | ✓ |  |  |
| `cdrom_trace_clear` | ✓ |  |  |
| `cdrom_trace_dump` | ✓ |  |  |
| `ce_profile` | ✓ |  |  |
| `chain_trace` | ✓ |  |  |
| `clear_input` | ✓ | ✓ | ✓ |
| `continue` | ✓ |  | ✓ |
| `cyc_watch` | ✓ | ✓ |  |
| `cyc_watch_clear` | ✓ | ✓ |  |
| `cyc_watch_dump` | ✓ | ✓ |  |
| `cycles_to_next_event` | ✓ |  |  |
| `d44_ring` | ✓ |  |  |
| `data_shards` | ✓ |  |  |
| `devtrace_ctl` | ✓ | ✓ |  |
| `devtrace_dump` | ✓ | ✓ |  |
| `dirty_block_dump_file` | ✓ |  |  |
| `dirty_block_log` | ✓ |  |  |
| `dirty_break_clear` | ✓ |  |  |
| `dirty_break_range` | ✓ |  |  |
| `dirty_break_state` | ✓ |  |  |
| `dirty_flow_log` | ✓ |  |  |
| `dirty_insn_dump_file` | ✓ |  |  |
| `dirty_insn_gate` | ✓ |  |  |
| `dirty_insn_log` | ✓ |  |  |
| `dirty_ram_stats` | ✓ |  |  |
| `dirty_ram_unsupported` | ✓ |  |  |
| `disp_ring` | ✓ |  |  |
| `dispatch_check` | ✓ |  |  |
| `dispatch_stats` | ✓ |  |  |
| `dispatch_tail` | ✓ |  |  |
| `display_ring_aux` | ✓ |  |  |
| `display_ring_get` | ✓ |  |  |
| `display_ring_stats` | ✓ |  |  |
| `dma_cdrom_history` | ✓ |  |  |
| `dma_state` | ✓ |  | ✓ |
| `dma_trace_clear` | ✓ |  |  |
| `dma_trace_dump` | ✓ |  |  |
| `dump_buffer` | ✓ |  |  |
| `dump_ram` | ✓ | ✓ | ✓ |
| `evcb_snapshot` | ✓ |  |  |
| `evcb_walk_dump` | ✓ |  |  |
| `evcb_walk_stats` | ✓ |  |  |
| `event_ring_clear` | ✓ |  |  |
| `event_ring_dump` | ✓ |  |  |
| `event_ring_tail` | ✓ |  |  |
| `exc_ring` |  | ✓ |  |
| `first_failure` | ✓ | ✓ | ✓ |
| `fmv_state` | ✓ |  |  |
| `fn_clear` | ✓ |  |  |
| `fn_disable` | ✓ |  |  |
| `fn_entry_dump` | ✓ |  |  |
| `fn_entry_tail` | ✓ |  | ✓ |
| `fn_exit_dump` | ✓ |  |  |
| `fn_filter` | ✓ |  |  |
| `fn_stats` | ✓ |  |  |
| `fntrace_arm` | ✓ | ✓ |  |
| `fntrace_arm_clear` | ✓ |  |  |
| `fntrace_armed` | ✓ |  |  |
| `fntrace_arms` |  | ✓ |  |
| `fntrace_clear` | ✓ |  |  |
| `fntrace_disarm` |  | ✓ |  |
| `fntrace_dump` | ✓ | ✓ |  |
| `fntrace_reset` |  | ✓ |  |
| `fntrace_unfiltered` |  | ✓ |  |
| `frame` | ✓ |  | ✓ |
| `frame_fingerprint` | ✓ |  |  |
| `frame_perf` | ✓ |  |  |
| `frame_range` | ✓ | ✓ | ✓ |
| `frame_timeseries` | ✓ | ✓ | ✓ |
| `freeze_check` | ✓ |  |  |
| `game_options` | ✓ |  |  |
| `get_frame` | ✓ | ✓ | ✓ |
| `get_quads` | ✓ |  |  |
| `get_registers` | ✓ | ✓ | ✓ |
| `get_snapshots` | ✓ | ✓ | ✓ |
| `gl_coh_ring` | ✓ |  |  |
| `gl_fbo_peek` | ✓ |  |  |
| `gl_interp` | ✓ |  |  |
| `gl_present_ring` | ✓ |  |  |
| `gl_vram_diff` | ✓ |  |  |
| `gl_wide_fast` | ✓ |  |  |
| `gl_ws_ablate` | ✓ |  |  |
| `gp1_dump` | ✓ |  | ✓ |
| `gpu_frame_dump` | ✓ |  | ✓ |
| `gpu_opcodes` | ✓ |  |  |
| `gpu_ring_stats` | ✓ |  |  |
| `gpu_state` | ✓ |  | ✓ |
| `gte_frame_stats` | ✓ |  |  |
| `gte_intpl_dump` | ✓ |  |  |
| `gte_latch_dump` | ✓ |  |  |
| `gte_ring_dump` | ✓ |  |  |
| `gte_state` | ✓ |  |  |
| `history` | ✓ | ✓ | ✓ |
| `hle_dump` | ✓ |  | ✓ |
| `idle_skip` | ✓ |  |  |
| `imask_trace` | ✓ |  |  |
| `insn_freeze` | ✓ |  |  |
| `insn_freeze_snapshot` | ✓ |  |  |
| `insn_freeze_status` | ✓ |  |  |
| `insn_freeze_target` | ✓ |  |  |
| `irq_state` | ✓ |  | ✓ |
| `irqctx_ring` | ✓ |  |  |
| `kernel_bless` | ✓ |  |  |
| `latency` | ✓ |  |  |
| `load_transitions` | ✓ |  |  |
| `lockstep` | ✓ |  |  |
| `lockstep_func` | ✓ |  |  |
| `mc_status` | ✓ |  |  |
| `mdec_state` | ✓ |  |  |
| `mdec_trace` | ✓ |  |  |
| `mdec_trace_clear` | ✓ |  |  |
| `mem_words` | ✓ |  |  |
| `menu_click` | ✓ |  |  |
| `menu_key` | ✓ |  |  |
| `menu_move` | ✓ |  |  |
| `menu_state` | ✓ |  |  |
| `mmio_clear` | ✓ |  | ✓ |
| `mmio_dump` | ✓ |  | ✓ |
| `mmx6_freshfix` | ✓ |  |  |
| `input_lag` | ✓ |  |  |
| `osd_toast` | ✓ |  |  |
| `pad_devices` | ✓ |  |  |
| `pad_probe` | ✓ |  |  |
| `overlay_candidates` | ✓ |  |  |
| `overlay_capture_dump` | ✓ |  |  |
| `overlay_cps_probe` | ✓ |  |  |
| `overlay_diff_off` | ✓ |  |  |
| `overlay_diff_on` | ✓ |  |  |
| `overlay_dump` | ✓ |  |  |
| `overlay_fp_dump` | ✓ |  |  |
| `overlay_irq_ratelimit` | ✓ |  |  |
| `overlay_irq_suppress_off` | ✓ |  |  |
| `overlay_irq_suppress_on` | ✓ |  |  |
| `overlay_loader_status` | ✓ |  |  |
| `overlay_native_block` | ✓ |  |  |
| `overlay_native_event_granularity` | ✓ |  |  |
| `overlay_native_off` | ✓ |  |  |
| `overlay_native_on` | ✓ |  |  |
| `overlay_native_ring` | ✓ |  |  |
| `overlay_rescan` | ✓ |  |  |
| `overlay_shadow_detail` | ✓ |  |  |
| `overlay_shadow_dump` | ✓ |  |  |
| `pace_state` | ✓ |  |  |
| `pad_status` | ✓ | ✓ |  |
| `parity_ctl` | ✓ | ✓ |  |
| `parity_dump` | ✓ | ✓ |  |
| `pause` | ✓ |  | ✓ |
| `phase_hot` | ✓ |  |  |
| `phase_profile` | ✓ |  |  |
| `ping` | ✓ | ✓ | ✓ |
| `present_ring` | ✓ |  |  |
| `press` | ✓ | ✓ |  |
| `probe_clear` | ✓ |  |  |
| `probe_trace` | ✓ |  |  |
| `quit` | ✓ |  | ✓ |
| `ra_load_watch` | ✓ |  |  |
| `read_frame_ram` | ✓ | ✓ | ✓ |
| `read_ram` | ✓ | ✓ | ✓ |
| `record_frame` | ✓ |  |  |
| `record_frame_dump` | ✓ |  |  |
| `record_reads_dump` | ✓ |  |  |
| `restore_trace` | ✓ |  |  |
| `restore_trace_clear` | ✓ |  |  |
| `restore_trace_window` | ✓ |  |  |
| `rtrace_arm` | ✓ | ✓ |  |
| `rtrace_clear` | ✓ |  |  |
| `rtrace_disarm` |  | ✓ |  |
| `rtrace_disarm_all` |  | ✓ |  |
| `rtrace_dump` | ✓ | ✓ |  |
| `rtrace_ranges` | ✓ | ✓ |  |
| `rtrace_reset` |  | ✓ |  |
| `rtrace_stats` | ✓ | ✓ |  |
| `run_to_frame` | ✓ |  | ✓ |
| `s3_smear_watch` | ✓ |  | ✓ |
| `savestate` | ✓ |  |  |
| `screenshot` | ✓ | ✓ | ✓ |
| `screenshot_file` | ✓ | ✓ | ✓ |
| `set_input` | ✓ | ✓ | ✓ |
| `set_snapshot` | ✓ | ✓ | ✓ |
| `sio_arm_audit` | ✓ |  |  |
| `sio_burst_stats` | ✓ |  |  |
| `sio_ctrl_reg_clear` | ✓ |  |  |
| `sio_ctrl_reg_trace` | ✓ |  |  |
| `sio_ctrl_reg_window` | ✓ |  |  |
| `sio_irq_dump` | ✓ |  |  |
| `sio_irq_window` | ✓ |  |  |
| `sio_pc_trace` | ✓ |  |  |
| `sio_pc_window` | ✓ |  |  |
| `sio_state` | ✓ |  | ✓ |
| `sio_trace` | ✓ | ✓ |  |
| `sio_trace_reset` |  | ✓ |  |
| `sio_trace_window` | ✓ |  |  |
| `sio_write_window` |  | ✓ |  |
| `sp_ring` | ✓ |  |  |
| `spu_events` | ✓ | ✓ |  |
| `spu_events_reset` | ✓ |  |  |
| `spu_ram` | ✓ |  |  |
| `spu_status` | ✓ |  |  |
| `spu_voices` | ✓ | ✓ |  |
| `sreg_trace_clear` | ✓ |  |  |
| `sreg_trace_dump` | ✓ |  |  |
| `sreg_trace_find` | ✓ |  |  |
| `sreg_trace_stats` | ✓ |  |  |
| `stack_profile` | ✓ |  |  |
| `starv_ring` | ✓ |  |  |
| `step` | ✓ |  | ✓ |
| `synth_recurse` | ✓ |  |  |
| `thread_ctx_ring` | ✓ |  |  |
| `thread_trace` | ✓ |  |  |
| `thread_trace_clear` | ✓ |  |  |
| `timers_state` | ✓ |  |  |
| `turbo` | ✓ |  | ✓ |
| `turbo_audio_sink` | ✓ |  |  |
| `turbo_loads` | ✓ |  |  |
| `turbo_state` | ✓ |  | ✓ |
| `unknown_dispatch_log` | ✓ |  |  |
| `unwatch` | ✓ |  | ✓ |
| `vblank_rate` | ✓ |  |  |
| `vk_perf` | ✓ |  |  |
| `vram_peek` | ✓ | ✓ | ✓ |
| `vsync_query_hle` | ✓ |  |  |
| `warm_cd_route` | ✓ |  |  |
| `watch` | ✓ |  | ✓ |
| `wide_full` | ✓ |  |  |
| `wide_shot` | ✓ |  |  |
| `write_ram` | ✓ |  | ✓ |
| `ws_aspect` | ✓ |  |  |
| `ws_aspect_cone_site` | ✓ |  |  |
| `ws_backdrop_margin` | ✓ |  |  |
| `ws_backdrop_ring` | ✓ |  |  |
| `ws_backdrop_stretch` | ✓ |  |  |
| `ws_census` | ✓ |  |  |
| `ws_dbg_stretch` | ✓ |  |  |
| `ws_dome` | ✓ |  |  |
| `ws_dome_probe` | ✓ |  |  |
| `ws_far_threshold` | ✓ |  |  |
| `ws_hud_mode` | ✓ |  |  |
| `ws_margin` | ✓ |  |  |
| `ws_nw` | ✓ |  |  |
| `wtrace_add` | ✓ |  |  |
| `wtrace_all_dump` | ✓ | ✓ |  |
| `wtrace_all_reset` | ✓ | ✓ |  |
| `wtrace_all_stats` | ✓ | ✓ |  |
| `wtrace_arm` | ✓ | ✓ |  |
| `wtrace_boot_dump` | ✓ |  |  |
| `wtrace_boot_reset` | ✓ |  |  |
| `wtrace_boot_stats` | ✓ |  |  |
| `wtrace_boot_summary` | ✓ |  |  |
| `wtrace_clear` | ✓ |  | ✓ |
| `wtrace_del` | ✓ |  |  |
| `wtrace_disarm` | ✓ | ✓ |  |
| `wtrace_disarm_all` | ✓ | ✓ |  |
| `wtrace_dump` | ✓ | ✓ | ✓ |
| `wtrace_range` | ✓ |  | ✓ |
| `wtrace_ranges` | ✓ | ✓ |  |
| `wtrace_reset` | ✓ | ✓ |  |
| `wtrace_stats` | ✓ | ✓ |  |
| `wtrace_trans_dump` | ✓ |  |  |
| `wtrace_trans_reset` | ✓ |  |  |
| `wtrace_trans_stats` | ✓ |  |  |
| `xlate` | ✓ |  |  |
| `xprobe` | ✓ |  |  |
| `xprobe_arm` | ✓ |  |  |

<!-- END AUTOGENERATED COMMAND INDEX -->
