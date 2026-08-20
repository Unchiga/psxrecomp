# Next session — in-duel Fusion Assistant

Paste this into a fresh session. It is self-contained; facts below marked
"verified" were measured this session, not assumed.

---

## THE JOB

Build an **in-duel fusion assistant**: the player opens their hand, and the
feature tells them what they can fuse. Forbidden Memories has hundreds of
fusions driven by hidden type rules, and the game gives the player nothing —
you either memorize a FAQ or you guess. Fix that, natively.

The user's words on quality: *"we want it to feel as natural as possible, we
want it to feel like its part of the game like everything we've done so far.
polished, right at home with yugioh forbidden memories."* That is the bar the
rank meter and CARD DROPS page set: guest-styled, layout-tunable, and
indistinguishable from something Konami shipped.

**Read `CLAUDE.md` first.** Rule 3 (no printf — every observable is a TCP
debug command) and rule 4 (never edit `generated/`) are absolute.

## ARCHITECTURE — DECIDED, NOT OPTIONAL

The decomposition campaign (see `DECOMPOSE_MAIN_PROMPT.md`) exists because
main.cpp grew to 15k lines. **This feature contributes ~3 lines to main.cpp**:
a hook registration, a tick in the frame loop, a menu-setting handoff. The
user asked for this explicitly: *"make sure that we try to not have to keep
the bulk of it in main.cpp."*

Model it on the proven trio:

- **`psx_card_drops.c`** — the template. Owns its guest addresses, its own
  hook registration (`psx_card_drops_register_hooks`), its debug surface, and
  its menu setting. main.cpp registers, ticks, and hands it the menu value.
- **`psx_rank_logic.c` / `psx_rank_meter.c`** — logic vs compositor split:
  one file reads guest RAM and decides, the other rasterizes a canvas.
- **`psx_cd_overlay.c` / `psx_cd_sprites.c`** — host-drawn sprites composited
  over the game. The compositor in `gpu_gl_renderer.c` calls
  `*_needs_present()` / `*_image()` / `*_origin()`; a new overlay adds one
  such block there, not a renderer rewrite.

Suggested shape: `psx_fusion_db.c` (the fusion table + queries, no UI),
`psx_fusion_assist.c` (duel-state reading, hand tracking, what-can-fuse
logic, debug surface), `psx_fusion_overlay.c` (canvas). Headers in
`runtime/include`, sources registered in `runtime/runtime.cmake`.

Menu integration: a MODS-menu row via the same one-shot pattern as
GAME > REWIND (`psx_video_menu_take_rewind` — added 2026-08-18, copy its
shape) or a cycling option row like CARD DROPS. Persist via the existing
menu_settings.ini machinery. Layout live-tunable over TCP like
`card_drops_layout` — **never rebuild to nudge a pixel** (user preference,
long-standing).

## GROUND TRUTH (verified this session)

- **Savestate: TCP slot 0 = the harness.** The user saved "state 1" in the F7
  menu, which displays slots 1-based; the file is `slot00`, and loading TCP
  `{"cmd":"savestate","op":"load","slot":0}` shows the FREE DUEL screen with
  **Simon Muran** highlighted (WIN 10 LOSS 0) — verified by screenshot. The
  user stocked the deck with almost every kind of monster on purpose. Press
  through Simon to get a real duel with a fat, varied hand.
- **Fusion guides are in the game repo**: `docs/_txt/Fusion FAQ.txt` (10,019
  lines) and `docs/_txt/Fusion Guide.txt` (4,536 lines), plus the HTML
  originals one level up. Read the Guide's early sections first — it explains
  the MODEL: fusions are mostly **general type rules** ([Female] + [Fish] =
  Amazon of the Seas), layered with specific named pairs, a conflict/
  precedence system when several rules match, and the property that the
  result almost always has strictly more attack than both materials (the
  ~14 exceptions are listed). Equips "fuse" onto monsters by type lists
  (Dragon Treasure + any dragon + 36 named non-dragons) — that is the
  "spells fusing" weirdness the user remembers.
- **The guides are the model, NOT the implementation source.** The game has
  its own fusion data (it evaluates fusions at runtime), and the assistant
  must read THAT so it is right by construction. The FAQ is for orientation
  and for sanity-checking the extracted table.

## RESEARCH PATH (the actual first work)

1. **Find the hand in guest RAM.** Load slot 0, enter the duel. The debug
   server has `read_ram` / `mem_words` / `watch` / wtrace rings. Card IDs are
   u16s 1..722; a hand is 5 of them somewhere stable. Cheap trick: save two
   states that differ by one known card and diff the RAM (`read_ram` the full
   2MB is one request). The card-name stream (memory note: name = 0x801D0000
   + u16[0x801D5800 + id*2]) converts IDs to names for eyeballing.
2. **Find the fusion evaluation.** Perform a known fusion in-duel (guide page
   ~100 lists cheap pairs, e.g. Zanki + Warrior Elimination = Armored Zombie)
   with `fntrace` armed; the routine that runs at the moment of fusion leads
   to the table it reads. Alternatively search RAM/disc for the pair→result
   pattern using known IDs. The card database loads from disc — the table may
   be per-card lists in that blob.
3. **Extract or mirror the table into `psx_fusion_db.c`** — either read it
   live from guest RAM each boot (preferred: right for any rom revision) or
   bake it (only if the live read proves impractical; document why).
4. **Debug surface BEFORE UI** (rule 3, and it de-risks everything): a
   `fusion_hand` command reporting the hand IDs+names, and `fusion_list`
   reporting every fusable pair/chain and its result. That alone is a
   demoable milestone, verifiable against the guides — and the user can
   confirm in-game by performing a listed fusion.
5. Only then build the overlay.

Guest-call discipline if any hook ends up calling game code: the **$ra rule**
(see the card-drop memory / `psx_card_drops.c` comments), and **never poke
code into a hooked function**.

## UX — DECIDE WITH THE USER, EARLY

The user floated several shapes and explicitly said *"i really dont know"*.
They are available to test in-game — prototype cheap, show, iterate. Their
ideas, with what this session can add:

- **Free buttons exist**: while navigating the hand with no card selected,
  Circle, L1 and R1 do nothing. Any of them can open/toggle the assistant
  without colliding with the game. (Verify in-duel before relying on it.)
- **(a) Overlay browser**: L1/R1/Circle opens a panel listing everything the
  current hand can make. Most complete; model the panel on the rewind
  filmstrip / savestate menu (canvas + pause-free overlay, mouse optional).
- **(b) Hover highlight**: hovering a card marks the other hand cards it can
  fuse with (host-drawn markers over the hand, like the "New!" sprites), and
  maybe names the result. Natural, but the user spotted the limit
  themselves: it only shows one step, not what to do with the RESULT.
- **(c) Chain preview**: the game already fuses multi-selected cards left to
  right, two at a time. Mirror that: as the player selects cards (their
  native Up-press selection), show the running result of the chain so far.
  This answers the "what about the card it makes" problem exactly, because
  the chain preview IS iterated fusion. Likely the most "part of the game"
  feeling of the three, and it composes with (a).
- **(d) Best-by-attack**: a one-line "best fusion available: X (atk N)"
  readout, like the rank meter's status line. Cheap once fusion_list exists.

A sane arc: fusion_list debug command → (d) as the first visible slice →
then (a) or (c) per the user's taste after they play with it. Note fusions
can also target monsters already ON the field — guides cover it; keep scope
hand-only first and say so.

## VERIFICATION

- The two standing oracles must stay identical: boot log (19 lines,
  `--no-launcher`, byte-stable) and the card-drops snapshot (slot 2 → 40
  drops → distinct 28 / total 39 → page 3 active). Recipes in
  `DECOMPOSE_MAIN_PROMPT.md` §HOW TO VERIFY.
- The feature's own oracle: `fusion_list` output vs the guide's pair lists
  (sample, don't transcribe), and the decisive check — perform a predicted
  fusion in-game and the result card matches. The user will play; ask.
- Every new state gets a TCP readback the same commit it lands (the
  `savestate_menu_state` / `rewind_state` precedent — both exist because a
  refactor could otherwise break silently).

## PROJECT / BUILD (unchanged facts, hard-won)

Root `C:\dev\memories\YuGiOhForbiddenMemoriesRecomp`; `psxrecomp/` is a
junction to `C:\dev\memories\psxrecomp-master` (git repo; commit per
verified step). `cmake --build build-dbg -j` (debug + TCP on 4370),
`cmake --build build -j` (release). Both builds have `PSX_REWIND=ON` now.

- `C:\msys64\mingw64\bin` FIRST on PATH or compiles fail silently; from Bash
  also `export USERPROFILE` or ccache aborts.
- **Kill the exe before building** ("Permission denied" at link = code was
  fine). Launch with `SDL_JOYSTICK_DIRECTINPUT=0`, detached.
- The tree is CRLF (`psx_video_menu.c` is bare-LF): read with `newline=''`,
  detect the terminator, translate needles into it; `open(newline='\r\n')`
  on CRLF content makes `\r\r\n` which still compiles. Heredocs through Bash
  mangle backslashes — write scripts with the file-writing tool.
- Grep a candidate file for `std::`/`nullptr`/`class` before choosing C vs
  C++ for a new module; C is house style where it fits cleanly.
- The launcher (`recomp-ui`) is not buildable in this checkout; netplay is
  not needed (user said so) and `lib/recomp-net` is an empty submodule.
- `menu_key` pushes SDL events — it cannot fake `SDL_GetKeyboardState`
  polling. Guest-side button reads are immune to that limitation.

## SAVESTATE SLOTS (TCP numbering; F7 UI shows +1)

| TCP | contents |
|---|---|
| 0 | **FREE DUEL over Simon Muran — the fusion harness** (verified) |
| 2 | results screen (card-drops harness) |
| 5/6 | chest |
| 7 | 2182-card collection |
| 10 | **user's current spot — DO NOT CLOBBER** |
| 11 | frozen duel repro (bug #1; guest parked in BIOS VSync spin) |

**Check a slot before writing it.** One was overwritten once by not looking.

## STILL OPEN (not this task; don't regress)

- Rank meter scoring path unverified (needs a live duel — this task will
  finally provide one; glance at `rank_meter_state` while you're there).
- Bug #1 duel soft-lock (slot 11), bug #4 results-screen zoom
  (unreproduced). See `ISSUES.md`.

---

## STATUS after 2026-08-18 session 2 — the data half is DONE and verified

Committed as `psx_fusion_db.c` + `psx_fusion_assist.c` (+ headers, +
`runtime.cmake`, + four debug commands). **main.cpp untouched.**

**The game's rule, traced, not guessed.** `func_8001A280` makes three attempts
and takes the first that answers: fusion table `0x8017C2D8`, then equip table
`0x8017A1D8` in each argument order, else no fusion and the second card is
summoned as itself. Packing and record layout are in the `psx_fusion_db.c`
header comment.

**Both tables are duel data streamed from disc** — zeros outside a duel. So the
assistant can only answer inside a duel, which is where it is wanted anyway.

**Verified against the game itself**, by writing card ids into the live hand
records and summoning: 5/5 pairs matched, including two the GameFAQs fusion
list gets wrong and one fusion it invents. Do not treat that FAQ as an oracle.

Read-backs: `fusion_db`, `fusion_hand`, `fusion_list`, `fusion_try a= b=`.
Both standing oracles still pass (boot log byte-identical, card drops 28/39
with page 3 active).

### The one open defect — read this before building UI

`psx_fusion_assist_hand()` is **right on the opening turn and wrong after it**.
Fusing slots 0 and 1 reused slot 0 for the result and cleared its live flag, but
left slot 1 — equally consumed — reading `0x8000`. The flag is about the record,
not hand membership. The authoritative list is almost certainly whatever drives
the hand's sprite pass: `func_800170C8` is called once per displayed hand card
with `a0 = 0x801A7AE4 - 12 + slot*28`, descending. Find that loop's driver.
`fusion_hand` already dumps whole 28-byte records for 12 slots — the evidence is
in that output.

### Also still missing

- **Per-card stats table not located.** The hand records carry the stats of the
  card *in hand*, not of the card a fusion would *produce*, so "best fusion
  available (atk N)" — UX slice (d) — cannot be ranked yet. `fusion_list` is
  deliberately in hand order rather than a plausible-looking wrong order.
- UX shape still unchosen; that was always the user's call.

### Hazard

Navigating into a duel over TCP: wait ~5 s after Cross on Simon before pressing
Circle. Rushing it leaves the presses inside the DECK BUILDER, editing the deck.
Back up `saves/card*.mcd` first.

---

## STATUS after 2026-08-18 session 3 — the feature is ON SCREEN

The chain preview is built and verified end to end. A line above the hand names
the best fusion the hand can reach; as cards are picked it shows the card that
would stand if they were summoned now. The line read Twin-headed Thunder Dragon
2800/2100 and summoning those three cards produced exactly that.

**Everything since the last status:**

- **Card table 0x801D4244**, u32 per card by `id-1`: `atk=(w&0x1FF)*10`,
  `def=((w>>9)&0x1FF)*10`, `type=(w>>26)&0x1F`. Found by write-tracing a hand
  record during the deal. Matches the password guide on 614/620 cards; all six
  misses are guide typos.
- **Hand membership is the selection table, not the records.** Entry+0 at
  0x800EA030 + slot*12 is the slot's card object and is nonzero exactly while
  the hand is pickable. The records keep spent materials, live flag and all,
  through the whole summon animation.
- **Pick state**: entry+4 nonzero = picked, byte at +9 = 1-based pick order.
  The fold runs in PICK order, and a step that does not fuse leaves the
  incoming card standing.
- **`fusion_best`**: highest attack, fewest cards, defence breaking ties.
  Brute force over every ordered subset (320 at five cards).
- **Font**: `tools/font_extract.py` (untracked, beside sprite_extract.py) bakes
  the duel card-name font from VRAM 4bpp (2560,0), 8x12 cells, ASCII order with
  a seam at 0x5B..0x5F. The 4-bit value is a brightness ramp, not a palette
  index. At 8bpp the texels pair up and you get legible shapes full of colour
  noise — that is what sent the first search to the wrong sheet.

**New instrument: `screenshot_present`.** `screenshot` and `screenshot_hires`
both resolve BEFORE the overlay pass, so neither has ever been able to see the
rank meter, the CARD DROPS tags, or an OSD toast — proven by capturing a toast
that was plainly on screen and finding it absent. Use `screenshot_present`
(queue, then poll `screenshot_present_status`) for any overlay work.

### Open

- Layout is fitted by eye at y=124; tune live with
  `fusion_overlay x= y= text_x= enabled=`. No backing bar behind the text yet —
  over the bright field it is readable but not as crisp as the game's own bar.
- Guardian stars are bits 18..25 of the card word, unread and unmeasured.
- Nothing persists the overlay's on/off state; there is no MODS menu row yet.

---

## DONE — 2026-08-18 session 4. The feature is finished.

Everything above is built, on screen and verified in-game. What landed after
the overlay first drew:

- **Card table 0x801D4244** (u32 per card by `id-1`: atk, def, type) — so the
  suggestion can be ranked by what it actually makes.
- **`fusion_best`**: highest attack, fewest cards, defence breaking ties, and
  only over lines containing a REAL fusion. A hand that makes nothing says
  "No fusions in hand" rather than dressing up its biggest card.
- **Turn gate at 0x8009B1D5** (0 player, 1 opponent). The selection table alone
  was never enough: through the opponent's turn the game keeps the hand's
  objects allocated and its records populated and simply draws the cards face
  DOWN, so every "is the hand pickable" test stayed true. Found by recording a
  full turn cycle — 49 whole-RAM snapshots each paired with a
  `screenshot_present`, so samples could be labelled by what was on screen —
  then keeping bytes constant across all 44 player-turn samples and different
  across all five opponent-turn ones. Two earlier guesses were counters that
  differed on a single sample. **Validate a candidate flag across a whole
  cycle, never one before/after pair.**
- **Green/red on the name** by whether the picks would summon the suggested
  card — a RESULT test, not a pick-order test. The order test was wrong and the
  user found the case: the suggestion is the SHORTEST line, not the only one,
  and reaching the same card the long way is still reaching it.
- **VIEW rows** FUSION HINT and SUGGEST FUSION BY, both persisted. They belong
  in VIEW, not MODS: they only show information, where CARD DROPS changes what
  the game does.
- **Rank meter flicker**: hiding is immediate, showing waits twelve steady
  frames, so a fusion hides it for the duration instead of strobing.
- **`screenshot_present`**, because `screenshot` and `screenshot_hires` both
  resolve before the overlay pass and can see none of this.

Next work is packaging for other people — see `SHARE_BUILD_PROMPT.md`.

---

## STATUS after 2026-08-20 — equips are now worth something

### Corrections to the section above

- **"Per-card stats table not located" is STALE.** It was found: packed one
  u32 per card at `PSX_FUSION_STATS_BASE 0x801D4244`, indexed by `id - 1`,
  with `atk/10` in bits 0-8, `def/10` in bits 9-17 and type in bits 26-30
  (`psx_fusion_db_stats`). Ranking by the result's real stats has worked for a
  while. Bits 18-25 are still undecoded (level/attribute, most likely).
- Searching for that table as a plain array of `u16` atk/def pairs finds
  nothing at any stride, because of the packing above. Do not repeat that hunt.

### The equip defect the user reported, and what it actually was

Reported: with two Blue-Eyes, a Trihorned Dragon, Dragon Treasure and
Megamorph in hand, the helper suggested only a two-card play.

It was **not** missing chain support — `best_search` already walks every
ordered subset. It was that an equip scored **zero**: the line was ranked with
`psx_fusion_db_stats(carry)`, and in an equip chain `carry` stays the base
monster, so a 2-card and a 3-card line both scored 3000/2500. `line_better`
then breaks ties with *shorter wins*, so the longer, stronger line was actively
discarded. Equips looked free and worthless at the same time.

### What an equip is worth — traced, not taken from the FAQ

`0x8001A7F8` in the summon path:

```asm
8001A7F8  li   v0, 500          default bonus
8001A800  sh   v0, 42(s1)
8001A814  lh   v1, 766(gp)      the equip's card id
8001A818  li   v0, 657          Megamorph
8001A81C  bne  v1, v0 -> skip   anything else keeps 500
8001A828  sh   v1, 42(s1)       Megamorph stores 1000
```

There is no per-equip bonus to read from the card table instead: **every**
equip's stats word is byte-identical (`0x5C000000` — atk 0, def 0, type 23), so
this two-case rule is the whole of it.

**The bonus applies to ATTACK AND DEFENCE ALIKE, and successive equips ADD.**
Verified in a live duel by the user: Blue-Eyes White Dragon (3000/2500) fused
with Dragon Treasure and Megamorph came out **4500/4000**, and the resulting
field record read base 3000/2500 with the accumulator at record `+6` holding
**1500** = 500 + 1000. The record keeps base stats; the bonus is a separate
field added on display.

Note the earlier reading of `+6` as a terrain/FIELD bonus was wrong — two field
monsters each showing 500 were each wearing one equip.

### Fixed

`psx_fusion_db_equip_bonus()` plus a `fold_step`/`line_stats` pair that carry
the running bonus through the chain. `fusion_best`, `fusion_chain` and
`fusion_list` all report stats **including** equips, and `fusion_chain` also
reports the running `bonus`.

Verified by rebuilding that exact hand in a scratch duel (write the ids into
the records at `0x801A7AE4` and the pickable-marker words at `0x800EA030`):

```
fusion_best -> cards=3  pick=[0,3,4]  result=1  atk=4500  def=4000
fusion_list ->   1 + 657 -> 1   atk=4000 def=3500
                 1 + 315 -> 1   atk=3500 def=3000
```

which is exactly what the game produced.

**One modelling assumption, NOT verified:** a step that produces a *different*
card resets the bonus to zero, on the reasoning that the equips were spent on
the monster just consumed. The verified case is equips accumulating onto one
monster. Someone should check fuse-then-equip-then-fuse before relying on it.

### Still missing — the six-card case

The user's point: *"it actually allows you to combine up to six cards if you
place them on a monster card you already have on the field."*

Unimplemented. `PSX_FUSION_FLAG_FIELD (0x0400)` is defined and **never used**,
`psx_fusion_assist_hand()` only walks the five hand slots, and
`PSX_FUSION_HAND_MAX` caps the search at 5. So a field monster is never
considered as a fusion base, and equipping onto an existing monster — often the
strongest play, since that monster may already carry a bonus — is invisible to
the helper.

What it needs, in order:

1. **Which records are the player's field.** Records 5, 6, 7 read `flags
   0x8400` (LIVE|FIELD) with the player's three monsters out, and the 12-record
   window has room for more. Establish the player/opponent split from the game
   rather than assuming a slot range — putting a monster on each side and
   diffing is enough.
2. **Seed the search from each eligible field monster**, with `carry` = its id
   and `bonus` = its existing record `+6`, then fold hand cards onto it. Raise
   the depth cap to 6.
3. **Report it distinctly.** "Play these onto your Blue-Eyes on the field" is a
   different instruction from "play these from hand", and the UI has to say
   which.
