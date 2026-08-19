# Next session — make this shareable (bring-your-own disc)

Paste this into a fresh session. Facts marked "verified" were measured, not
assumed. **Read `CLAUDE.md` first** — rule 3 (no printf, every observable is a
TCP debug command) and rule 4 (never edit `generated/`) are absolute.

---

## THE JOB

Package the recomp so other people can run it **without receiving the disc**.
On first launch it must ask the player to point at their own dump, verify it is
the right one, remember it, and never ask again. The user's framing:

> "i cant supply the rom, so it must be pointed to when the launcher opens if
> it hasnt already been. i imagine the hash will have to be compared to as well
> to make sure they have the same version of the rom."

Two halves: **where the path comes from** (a first-run prompt) and **whether
the dump is the right one** (a hash check with an honest failure message).

## WHAT ALREADY EXISTS — do not rebuild this

Most of the identity half is already here. Read it before writing anything.

- **`game.toml`** carries the whole contract today:
  - `disc = "C:\\dev\\memories\\Yu-Gi-Oh! Forbidden Memories (USA).cue"` — an
    ABSOLUTE path to one machine's dump. **This is the thing that has to go.**
  - `known_sha1 = [...]` and `required_disc_fp = "a828d94c...161c4"` (sha256).
  - `exe = "disc/SLUS_014.11"`, serial `SLUS-01411`.
- **`runtime/src/disc_identity.cpp`** already computes disc identity, handling
  both 2352-byte raw and 2048-byte cooked sector layouts (real .bin/.cue vs
  .iso). It uses `psx_sha256` and `crc32`. Find out what it already reports and
  what the runtime does today when the fingerprint does NOT match — that
  behaviour is the foundation of the failure path, and it may already be right.
- **`catalog_identity.json`** — cue/bin names, track counts, boot exe, region.
- **`disc_probe.json`** — a previous probe's output (md5/sha1 of the data
  track, sizes), produced by `tools/new_project_layout/probe_disc.py`.
- **CLI overrides that already work:** `--disc <path>`, `--bios <path>`,
  `--memcard-dir <path>`, `--no-launcher`, `--launcher`.

So the likely shape is: keep `disc_identity`'s verification, replace the baked
absolute path with a stored user setting, and add the first-run prompt.

## THE BLOCKER TO RESOLVE FIRST

**The launcher (`recomp-ui`) is NOT buildable in this checkout** — this has
been true all session and is recorded in memory
(`psxrecomp-launcher-not-buildable`). Every build here runs `--no-launcher`.

So "ask when the launcher opens" cannot be taken at face value until someone
establishes whether the launcher can be built at all. Decide EARLY between:

1. Make `recomp-ui` buildable (find out why it is not — missing submodule, like
   rewind's `retcomm-rbengine` was, or something worse), then add the prompt
   there; or
2. Put first-run disc selection in the runtime itself, so it works with or
   without the launcher. An SDL file-picker or a plain "drop the path in this
   file" flow both beat a prompt nobody can build.

Do not spend the session half-building the launcher. Establish which of the two
it is in the first half hour and say so.

## DESIGN NOTES

- **Persist the answer** next to the other player settings. `menu_settings.ini`
  already exists beside the exe, is read and written by `psx_video_menu.c`, and
  survives restarts — the same machinery `fusion_hint`/`card_drops` use.
- **The hash message has to be useful.** "Wrong disc" is not enough: say what
  was expected, what was found, and that the required dump is the USA release
  serial SLUS-01411. A player with a PAL or Greatest Hits dump should learn
  that from the message, not from a forum.
- **Do not ship the disc, the BIOS, or the saves.** `saves/card1.mcd` is the
  user's own progress; `bios/openbios.bin` is bundled already (MIT, with its
  notice) — confirm that stays true for anything else that gets packaged.
- **`scripts/package_setup_release.sh` exists** — read it before inventing a
  packaging step; it may already do most of this.

## VERIFICATION

The two standing oracles must stay green, and both are cheap:

1. **Boot log** — `<exe> --no-launcher`, 22 lines, byte-identical run to run.
   Ignore any `opened controller` line (that is a pad being plugged in).
2. **Card-drops snapshot** — savestate slot 2 → `card_drops_set drops=40` →
   `write_ram 0x8009B23B = 0` → poll `card_drops_list` until it is STABLE at
   distinct 28 / total 39 → D-pad Right until `card_drops_p3` reports
   `active:1`. Sync on conditions, never on sleep.

For this feature specifically: the honest test is a **fresh directory with no
settings file and no disc path**, launched cold. If it does not ask, or asks
twice, or accepts a wrong dump, it is not done. Test the failure path with a
deliberately wrong file, not only the happy path.

## STATE OF THE FUSION ASSISTANT (finished; don't regress it)

Complete and verified in-game. `psx_fusion_db.c` (the game's own fusion and
equip tables, read live), `psx_fusion_assist.c` (hand, chain, best line),
`psx_fusion_overlay.c` (the line + pick-order badges), `psx_fusion_font.c`
(baked from VRAM by `tools/font_extract.py`). VIEW carries FUSION HINT
(OFF / NUMBERS / NUMBERS + INFO) and SUGGEST FUSION BY (ATTACK / DEFENSE),
both persisted. main.cpp gained one tick call.

Read-backs: `fusion_db`, `fusion_hand`, `fusion_list`, `fusion_chain`,
`fusion_best`, `fusion_try`, `fusion_overlay`.

**New instrument worth knowing about: `screenshot_present`.** `screenshot` and
`screenshot_hires` both resolve BEFORE the overlay pass, so neither can see any
host overlay — proven by capturing an OSD toast that was plainly on screen and
finding it absent. Use `screenshot_present` (queue, then poll
`screenshot_present_status`) for anything visual.

Key guest addresses are in the auto-memory note `ygofm-fusion-tables`.

## PROJECT / BUILD (unchanged, hard-won)

Root `C:\dev\memories\YuGiOhForbiddenMemoriesRecomp`; `psxrecomp/` is a
junction to `C:\dev\memories\psxrecomp-master` (the git repo — commit per
verified step). `cmake --build build-dbg -j` (debug + TCP on 4370),
`cmake --build build -j` (release).

- `C:\msys64\mingw64\bin` FIRST on PATH or compiles fail silently; from Bash
  also `export USERPROFILE` or ccache aborts.
- **Kill the exe before building** ("Permission denied" at link = code was
  fine). Launch with `SDL_JOYSTICK_DIRECTINPUT=0`, detached.
- The tree is CRLF (`psx_video_menu.c` is bare-LF): read with `newline=''`,
  detect the terminator, translate needles into it. Heredocs through Bash
  mangle backslashes — write scripts with the file-writing tool.
- A multi-edit script that writes only after ALL its substitutions succeed will
  silently apply NOTHING when one needle misses. Two bugs this session came
  from exactly that. Check the file afterwards, not the script's exit code.
