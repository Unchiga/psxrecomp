# Next session — decompose `main.cpp` into a readable module set

Paste this into a fresh session. It is self-contained; everything below was
measured, not assumed.

---

## THE JOB

`runtime/src/main.cpp` is **15,450 lines** and `main()` itself is **1,258**
(it was 3,607 when this campaign started). Get the FILE to **~1,000**, by moving
cohesive subsystems into real modules. This is the whole task. It is a
multi-session campaign, not one refactor — expect to land a few verified
extractions per session and stop cleanly between them.

> **Session of 2026-08-18 landed five commits** (`cef7d98`..`0d2e35e`), taking
> `main()` from 3,607 to 1,258 lines. The startup phases named in the old plan
> are DONE. The file total barely moved, because the extracted phases are still
> `static` functions in `main.cpp` — see "WHERE THIS STANDS NOW" below for why,
> and what has to happen before they can leave the translation unit.

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
- **`main()` was the only whale at 3,607 lines** (brace-matched; the handoff's
  earlier 3,657 was close but off). It is now **1,258** — see WHAT IS ALREADY
  DONE. Everything else averages ~31. (An earlier claim that
  `sdl_vblank_present` was 3,380 lines was WRONG — it came from measuring gaps
  between detected definitions. Use brace matching.)
- `main()`'s old internal phases measured, for reference: args/config 827 |
  launcher session 944 | device init 296 | window init 269. The handoff's
  original estimates (1,085 / 797 / 1,069 / 706) were drawn on different
  boundaries; trust the table above.
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
- **`PsxBootConfig`** — `main()`'s ~78 startup locals as one named record.
- **The four startup phases (2026-08-18).** `main()` 3,607 -> 1,258 lines:

  | function | lines | what it owns |
  |---|---|---|
  | `resolve_boot_config` | 827 | CLI, game.toml, settings.toml, mod scan |
  | `run_launcher_session` | 944 | overlay worker, launcher GUI, mod activation |
  | `init_runtime_devices` | 296 | GPU/SSAA, devices, disc, text guard |
  | `open_game_window` | 269 | SDL, audio, pads, GL/VK, F10 menu, savestates |

  Plus the five `*_offered` policy constants hoisted to file scope, which were
  what actually blocked the first phase.
- **`psx_rank_logic.c` (498 lines, 2026-08-18)** — the game-logic half of the
  duel rank meter, moved out beside `psx_rank_meter.c` (its compositor).
  **First change to reduce the file total: 15,450 -> 14,996.** Its scoring path
  is NOT covered by any oracle — it needs a live duel, and no reachable
  savestate provides one (slot 11's guest is parked in the BIOS VSync spin).

## WHERE THIS STANDS NOW — READ BEFORE PLANNING

`main()` reads like a table of contents and the startup work is done.
`main.cpp` is **14,996** lines (was 15,363 at the campaign start; the four
phase extractions moved code *within* the file, `psx_rank_logic.c` was the
first that moved code *out*).

### The phase functions should stay in main.cpp — this was measured

The previous handoff said "extract the settings globals, then the phase
functions leave almost mechanically". **That was wrong, and the number is
why.** Moving the four phase functions to their own TU needs **96 shared
symbols exported**, not the ~35 settings globals:

| shared symbol group | examples |
|---|---|
| SDL/renderer handles | `sdl_renderer` (42 uses), `sdl_texture`, `sdl_pixel_buf`, `sdl_audio_device`, `g_gl_active`, `g_vk_active` |
| video settings | `g_video_aspect_num/den`, `g_video_vsync`, `g_video_scale`, `g_video_aa`, `g_fullscreen` |
| host helpers | `exe_dir_from_argv`, `resolve_bios_path`, `refresh_player_devices`, `controller_deadzone` |
| pad/netplay state | `g_players`, `s_netplay_gl_present`, `s_drc`, `s_picker_game_name` |

Only 22 of the 118 symbols the phases touch are exclusive to them. Exporting
the SDL handles and renderer flags through a public header to buy a 2,340-line
move would spread `main.cpp`'s internals across the tree and make the design
*worse*. **Do not do it.** The phases are already named, already readable, and
already the "table of contents" the style instruction asked for. Leave them.

### Extract cohesive FEATURES instead — that is what moves the file

`psx_rank_logic.c` is the proof: 442 lines out, 26 of its 28 dependencies were
exclusive to it, and the one genuinely shared symbol (`g_rank_meter`) became a
better API (`psx_rank_logic_set_mode`) than the open-coded clamp it replaced.
**Run the exclusive-vs-shared split before committing to any candidate** — a
cluster with a handful of shared symbols is a module; one with 96 is not.

### What is left inside `main()` (1,258 lines)

| region | lines | notes |
|---|---|---|
| prologue -> `session_reboot:` | 85 | already just named calls |
| `session_reboot:` -> lobby | 726 | CPU wiring, BIOS backend select, execute, shutdown |
| `soft_return_lobby:` -> end | 446 | netplay lobby rematch |

**The `goto` pair blocks the rest.** `session_reboot:` is jumped to from the
end of the lobby block, and `goto soft_return_lobby` runs the other way, so the
whole region is one control-flow unit. Turning the label pair into a real loop
is a behaviour-affecting restructure on the netplay rematch path, **and netplay
is covered by neither oracle.** Do not attempt it without a way to test a real
rematch.

## THE PLAN, IN ORDER OF VALUE PER RISK

1. **More feature modules, largest first.** Measured clusters still in
   `main.cpp`, with the caveat that each needs its own exclusive-vs-shared
   check before you commit:
   - savestate menu host glue (~417) — `psx_savestate_menu.c` already exists
   - menu glue (~268) — `psx_apply_video_menu_state`; `psx_video_menu.c` exists
   - audio (~247) — `psx_sdl_audio.cpp` already exists
   - perf diag (~248) — `runtime_perf_*`, contiguous at ~2826-3087, self-named.
     Its one wart: call sites pass `&g_runtime_perf.<field>` to
     `runtime_perf_section_end`, so a clean module wants named section IDs.
   - load probe (~117) — `post_load_probe_*`
   The three with an existing sibling module are the best value: the split is
   already half-made, and the pattern is proven.
2. **input/pad** (~1,100). Cheap globals but scattered; move function by
   function, never by line range.
3. **present path** (~1,550). 102 shared globals, hot path, interpolation
   thread and `s_interp_mutex`. Highest risk — last, with a soak test.
4. **`debug_server.c`** is 15,038 lines with the same disease. It is a dispatch
   table of independent handlers, so it splits far more easily — good parallel
   work when you want a lower-risk win.

## HOW TO VERIFY (do not skip; this caught real bugs)

Working harness scripts from the 2026-08-18 session are described here; they
were throwaway, so rebuild them, but rebuild them to THIS shape.

1. **Boot-log oracle.** `<exe> --no-launcher`, capture stdout, kill after ~14 s.
   19 lines covering resolved config path, BIOS image, renderer, supersampling,
   disc region, text-guard range, BIOS backend and fast loading. Byte-identical
   across runs; a diff means the refactor changed behaviour.
2. **Runtime regression snapshot** over the TCP debug server (port 4370):
   savestate load slot 2 -> `card_drops_set drops=40` -> `write_ram`
   `0x8009B23B` = 0 -> `card_drops_list` settles at **distinct 28 / total 39**
   -> D-pad Right (`set_input buttons=0xFFDF`) -> `card_drops_p3` reports
   `active:1`. Dump every field as JSON and diff two runs.

   **Sync on conditions, never on `sleep`.** Fixed sleeps made the snapshot
   depend on which frame the guest was in when a command landed, and it
   reported a refactor difference that did not exist. Poll until the drop tally
   has been *stable* for ~8 polls (it accumulates over many frames, so
   "non-zero" is not the end state), and retry the D-pad press in a loop until
   `active:1`, because the screen ignores input while it is still building.

   **Prune the free-running fields** or they produce false diffs: `frame`,
   the `gp0_*` counters, `speed_mult`/`period_ms`, `display_x`/`draw_area`/
   `draw_offset` (double-buffer phase), `gpustat` (bit 31 is the interlace
   line, bits 0-4 the live texture page), and `sub`/`prev_page`/`applies`/
   `overrides` on `card_drops_p3` (per-render counters).
3. **Byte-identity check for moved code.** `git show <rev>:runtime/src/main.cpp`,
   pull the original line range, diff it against the new function body. It must
   differ ONLY by the edits you intended. This is what caught nothing and
   proved everything — and it is the only real check available for the
   launcher region (below).
4. **Verify behaviour, not compilation.** The `psx_card_drops` extraction
   compiled clean and the behavioural pass still found a real latent bug.

## ⚠ THE LAUNCHER IS NOT BUILDABLE IN THIS CHECKOUT

`PSX_RECOMP_UI:BOOL=OFF` in **both** `build-dbg` and `build`, and
`RECOMP_UI_ROOT` is empty — there is no `recomp-ui/` directory in the game
repo. So `RECOMP_LAUNCHER` is never defined, and **roughly 700 lines of
`run_launcher_session` are preprocessed away and never compiled here.** A plain
launch (no `--no-launcher`) boots straight into the game; there is no launcher
window to test.

That region was verified by byte-identity + balanced preprocessor conditionals
+ zero brace delta with `RECOMP_LAUNCHER` both on and off. That is sound but it
is not execution. **Before a release build, someone with `recomp-ui` must
compile and run the launcher path once.** Treat any future edit inside
`#if defined(RECOMP_LAUNCHER)` the same way: identity-check it, and say so.

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
- **That check was written on 2026-08-18 and it is what made the four phase
  extractions safe.** Shape: strip comments/strings, walk the function tracking
  brace depth, collect every declaration at depth 1 inside the candidate range,
  and grep the text AFTER the range for each name. Anything still referenced
  ESCAPES and must not move. All four phases were cut only after this reported
  zero escapes, and all four then compiled on the first attempt. Run it before
  every extraction; if it reports escapes, move the boundary rather than
  "fixing" them.
- Check the range for `return` / `goto` / label before cutting. Fatal `return 1`
  sites become a status return (`bool`, or an enum when the phase has more than
  two outcomes — `run_launcher_session` needed Boot/Quit/Failed). A `goto`
  crossing the boundary means the region is NOT extractable.
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
- **The tree is CRLF.** Two traps, both hit on 2026-08-18: a Python
  `str.replace` whose needle uses `\n` silently matches NOTHING (no error, the
  patch just does not apply — check your replacements landed), and writing with
  `open(..., newline='\r\n')` on content that already has CRLF produces
  `\r\r\n`. That still compiles, so only a diff catches it. Read with
  `newline=''`, detect the terminator, and translate needles into it.

## GIT

`psxrecomp-master` is a git repo (1,590 files; build outputs, `generated/`,
saves and local `.ini` state ignored).

- `v0-pre-decomposition` — tag on the baseline commit. **It already contains
  `PsxBootConfig`**: the repo was created after that change, so there is no
  pre-boot-struct revert point. The tagged state is verified working.
- `cef7d98`..`0d2e35e` — the 2026-08-18 phase extractions, one commit each with
  the measured before/after in the message. Any of them reverts cleanly.
- Commit each extraction separately, with the measured before/after in the
  message. Committing first is also what makes the oracle honest: to compare
  before/after you check out the previous commit, build, capture, then restore.

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
