# Next session — decompose `main.cpp` into a readable module set

Paste this into a fresh session. It is self-contained; everything below was
measured, not assumed.

---

## THE JOB

`runtime/src/main.cpp` is **15,363 lines**. Get it to **~1,000**, by moving
cohesive subsystems into real modules. This is the whole task. It is a
multi-session campaign, not one refactor — expect to land a few verified
extractions per session and stop cleanly between them.

**Read `CLAUDE.md` first.** This is enhancement-phase work on a proven core;
rule 3 (no printf/log-file debugging — runtime inspection is ALWAYS a TCP debug
command) and rule 4 (never hand-edit `generated/`) are absolute.

## STANDING INSTRUCTION ON STYLE — READ THIS TWICE

The user's words: *"lets make a serious effort to make this project look
readable, not immediately like it was made by Claude... a flaw with you is that
you build very large mains unless instructed not to."*

That critique is accurate and it is the reason this task exists. Concretely:

- **Never grow `main.cpp`.** New feature? It belongs in a module with a header.
  If you catch yourself appending to `main()`, stop and make the file first.
- **`main()` should read like a table of contents** — resolve config, run the
  launcher, init the runtime, run the frame loop. A reader should understand
  the program's shape from `main()` in under a minute.
- **Keep the comments.** They are load-bearing and repeatedly saved real time
  on this project. Do NOT strip them to hit a line target — 17% of the file is
  comments and that is fine. Trim only genuinely rambling ones. Cutting
  explanation to win a line count is the wrong trade, and the user agrees.
- **Write like a human engineer.** Comments explain *why* and record what was
  measured, not what the next line obviously does. No cheerful filler, no
  "Step 1 / Step 2" scaffolding, no restating code in prose.
- Match the house style already in `psx_card_drops.c`, `psx_rank_meter.c`,
  `psx_video_menu.c` — those are the reference for what good looks like here.

## GROUND TRUTH ABOUT THE FILE (all measured)

Do not re-derive these; do re-verify one before acting on it.

- **179 top-level functions**, ~9,270 lines of function bodies. The rest
  (~6,000 lines) is file scope: globals, tables, comment blocks.
- **`main()` is the only whale: 3,657 lines.** Everything else averages ~31.
  (An earlier claim that `sdl_vblank_present` was 3,380 lines was WRONG — it
  came from measuring gaps between detected definitions. Use brace matching.)
- `main()`'s internal phases: args/config ~1,085 | launcher session ~797 |
  runtime+GL+menu init ~1,069 | frame loop ~706.
- **Cluster map** (function lines only; their globals and tables move too):

  | cluster | lines | fns | notes |
  |---|---|---|---|
  | present | 1,550 | 7 | `sdl_vblank_present_body` is 1,179 |
  | input/pad | 1,116 | 40 | scattered line 368..6289, NOT contiguous |
  | netplay | 455 | 13 | |
  | savestate menu | 417 | 18 | |
  | rank glue | 269 | 10 | |
  | menu glue | 268 | 1 | `psx_apply_video_menu_state` |
  | audio | 247 | 5 | |
  | perf diag | 234 | 9 | |
  | load probe | 117 | 6 | |

- **Coupling, measured — this is what decides difficulty:**
  - present path touches **102 shared globals** -> hardest, do last.
  - input/pad touches **7 shared globals**, 20 functions called from outside ->
    cheap coupling, but physically scattered and interleaved with unrelated
    code (e.g. `resolve_overlay_capture_path` sits inside the input region).
  - `main()` phase A leaks **52 locals**; the launcher phase reads 39 of them
    and writes many back. That is why `PsxBootConfig` had to exist first.

## WHAT IS ALREADY DONE

- **`psx_card_drops.c` (993 lines)** — extracted from `main.cpp` and working.
  Owns its guest addresses, its own hook registration, the CARD DROPS results
  page and its debug surface. **This is the template. Copy its shape.**
- **`psx_cd_overlay.c` / `psx_cd_sprites.c`** — host-drawn "New!" sprite
  overlay for that page.
- **`PsxBootConfig`** — `main()`'s ~78 startup locals are now one named record,
  defined in `main.cpp` just above `main()` because several field types and
  defaults (`RuntimeConfig`, `PSX_DEFAULT_BIOS_PATH`, `PSX_WINDOW_TITLE`) are
  declared in that translation unit. It exists so startup phases can take
  `PsxBootConfig&` instead of 40+ parameters. **Moving it to a header is step
  one of the phase extraction**, and those prerequisites move with it.

## THE PLAN, IN ORDER OF VALUE PER RISK

1. **Startup phases out of `main()`** (~2,950 lines). `PsxBootConfig` already
   unblocks this. Three functions — config resolution, launcher session,
   runtime init — each taking `PsxBootConfig&`. Startup-only code, so the boot
   log is a complete oracle. `main()` drops to ~700.
2. **Easy clusters** (~1,200): audio, perf diag, load probe, savestate-menu
   host glue, rank glue. Proven pattern, low coupling.
3. **input/pad** (~1,100). Cheap globals but scattered; move function by
   function, never by line range.
4. **present path** (~1,550). 102 shared globals, hot path, interpolation
   thread and `s_interp_mutex`. Highest risk — last, with a soak test.
5. **The tail** (~5,000 across ~166 small functions) — netplay glue, pad
   sampling, probes. This is what actually gets you from ~6,600 to ~1,000.

`debug_server.c` is **15,026 lines** with the same disease. It is a dispatch
table of independent handlers, so it splits far more easily — good parallel
work when you want a lower-risk win.

## HOW TO VERIFY (do not skip; this caught real bugs)

1. **Boot-log oracle.** Capture stdout before and after; it must match line for
   line. 21 lines covering resolved config path, BIOS image, renderer,
   supersampling, disc region, text-guard range, frame pacing, BIOS backend and
   fast loading. A diff here means the refactor changed behaviour.
2. **Runtime regression suite** over the TCP debug server (port 4370):
   load savestate slot 2 -> `card_drops_set drops=40` -> poke `0x8009B23B` = 0
   to rebuild the results screen -> `card_drops_list` must report distinct 28 /
   total 39 -> D-pad Right -> `card_drops_p3` must show `active:1, overlay:1`.
   Also `frame_pacing` (base 16.6667 / mult / period) and `gpu_state` 320x240.
3. **Verify behaviour, not compilation.** The `psx_card_drops` extraction
   compiled clean and the behavioural pass still found a real latent bug.

## LESSONS FROM THE FAILED ATTEMPT (read before scripting anything)

The `PsxBootConfig` transform was scripted and it **over-captured**: it swept in
a `static`, late locals whose initialisers depend on runtime values, a
multi-declarator (`int game_w = ..., game_h = 0;`), and a `tex_scale` that is
declared independently in several functions. The automated repair then deleted
those declarations *everywhere in the file*, breaking two unrelated functions,
and a later heuristic inserted a declaration into an unrelated function at line
370. Recovery was manual.

- The safety checks that were run (lambda captures, shadowing, string literals)
  all passed and gave false confidence, because none tested for **over-capture**
  — the actual failure mode. Write that check: *does every captured declaration
  belong to the phase being extracted?*
- Never delete lines by matching text globally. Operate on confirmed line
  ranges, one function at a time.
- Prefer many small verified steps over one clever transform.
- **`git` now exists.** Commit before each extraction so a bad one is a
  `git checkout`, not an hour of fixing forward.

## PROJECT / BUILD

Root: `C:\dev\memories\YuGiOhForbiddenMemoriesRecomp`
`psxrecomp/` is a **JUNCTION** to `C:\dev\memories\psxrecomp-master` (not a
submodule). All runtime code lives there and is shared.

```bash
cmake --build build-dbg -j     # debug + TCP debug server on 127.0.0.1:4370
cmake --build build -j         # release
```

- `C:\msys64\mingw64\bin` MUST be first on PATH or **every compile fails
  silently**. From the Bash tool also `export USERPROFILE` or ccache aborts.
- **Kill the running exe before building** or the link fails with "Permission
  denied" — that error means the code compiled fine.
- Launch with `SDL_JOYSTICK_DIRECTINPUT=0` or it hangs at boot. Launch detached,
  or it dies when the shell that started it is torn down.
- Sources in `runtime/src`, headers in `runtime/include`; add new modules to the
  source list in `runtime/runtime.cmake`.
- Modules are plain **C** where possible (`cpu_state.h`, `mod_plugins.h`,
  `host_osd.h` are C-safe with `extern "C"` guards). Going C++ -> C the fallout
  is mechanical: drop `extern "C"`, strip `std::` from memcpy/memset/snprintf,
  `struct X {}` needs a `typedef`, and C++ references (`T &e = arr[i]`) become
  pointers.
- Heredocs through the Bash tool mangle backslashes and some quoting; write
  scripts with a file-writing tool, or use `chr(92)` for backslash in Python.

## GIT

`psxrecomp-master` is a git repo (1,590 files; build outputs, `generated/`,
saves and local `.ini` state ignored).

- `v0-pre-decomposition` — tag on the baseline commit. **It already contains
  `PsxBootConfig`**: the repo was created after that change, so there is no
  pre-boot-struct revert point. The tagged state is verified working.
- Commit each extraction separately, with the measured before/after in the
  message.

## OPEN BUGS (not this task, but do not regress them)

- **#1 duel soft-lock** — reproduces from savestate **slot 11**. Guest parked in
  the BIOS VSync spin; duel/UI/work RAM byte-stable while sound and kernel pages
  advance; input completely dead (a lost internal event, not an input wait).
  Frozen at game state 9, handler `func_8001F55C -> func_8001F560`
  (`generated/SLUS_014.11_full_05.c`). Next step is that function's sub-state
  dispatch. See `ISSUES.md`.
- **#4 results screen magnified/clipped** — could not reproduce; GPU state and
  present rects are identical between duel and results. Marked unreproducible
  with reopen instructions.

## USER PREFERENCES WORTH KNOWING

- Wants pixel offsets applied **literally**, not converted between coordinate
  spaces. Layout is live-tunable via `card_drops_layout` — never rebuild to
  nudge a pixel.
- Available to test in-game; ask rather than guess when something needs a real
  duel or a real mouse. Host `SendInput` cannot click the SDL window.
- Savestate slots: **2** = results screen (main harness), **5/6** = chest,
  **7** = 2182-card collection, **10** = the user's current spot (do not
  clobber), **11** = the frozen duel repro. **Check a slot before writing it** —
  one was overwritten this session by not looking first.
