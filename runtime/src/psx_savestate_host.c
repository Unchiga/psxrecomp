/* Host-side save-state glue -- see psx_savestate_host.h.
 *
 * Three files share this feature. savestate.c serialises and restores the
 * machine; psx_savestate_menu.c draws the slot overlay and hit-tests it; this
 * file is the frontend logic between them -- which slot is selected, what a key
 * means, and the guard that stops the confirm press leaking into the guest once
 * the state is restored.
 *
 * The guard is the subtle half and the reason the trace ring exists. A restore
 * hands the guest a running game mid-keypress, so the press meant for the menu
 * would otherwise register in the duel. Two mechanisms cover it because neither
 * is sufficient alone: a short wall-clock settle window, and a release latch
 * that outlives it for as long as the button is genuinely held. The ring
 * records every button word and every arming so "did it leak, and by which
 * path" is answered from evidence rather than inference (CLAUDE.md rule 3).
 *
 * Raw SDL polling deliberately stayed in main.cpp: whether the player is still
 * holding a resume input is a question about keyboard and controller state, so
 * the host answers it through psx_savestate_host_resume_inputs_held().
 */

#include "psx_sdl.h"
#include "psx_savestate_host.h"

#include "psx_savestate_menu.h"
#include "psx_rewind.h"
#include "host_osd.h"
#include "host_keymap.h"
#include "savestate.h"
#include "psx_netplay.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t      g_savestate_input_guard_min_until = 0;
static uint32_t      g_savestate_input_guard_max_until = 0;
/* Buttons that were already held when a save-state action dismissed the menu,
 * forced to RELEASED until the player physically lets go. The window guard
 * above cannot do this job on its own: it is a wall-clock mute with a 700 ms
 * ceiling, so any confirm press held longer than that expires mid-hold and the
 * game receives the press that was only ever meant for the menu (a deliberate
 * "press X, watch the screen" is easily a second). This is release-latched
 * instead of timed, so it cannot expire early — and because it only ever masks
 * the bits that were down at arm time, each clearing on its own release, it
 * cannot swallow a NEW press either. One mask per pad slot: the snapshot is
 * taken on that slot's first sample, so slots sampled later still get one. */
uint16_t g_savestate_hold_mask[PSX_MAX_PLAYERS];
static uint32_t      g_savestate_hold_pending = 0;  /* bit per pad slot */
/* Arm the release latch alone. This is the SAVE case: saving does not disturb
 * the running game, so muting it for a settle window would be a visible input
 * hiccup during normal play — the only thing that must not reach the guest is
 * the confirm press itself, which is exactly what the latch holds. */
void savestate_hold_guard_arm(void) {
    g_savestate_hold_pending = ~0u;   /* every slot re-snapshots */
    savestate_diag_arm("hold");
}

/* Arm the post-load settle window AND the release latch. Load needs both: the
 * window also neutralises the sticks and covers the restore itself, while the
 * latch outlives it for as long as the button is genuinely held. */
void savestate_input_guard_arm(void) {
    uint32_t now = (uint32_t)SDL_GetTicks();
    g_savestate_input_guard_min_until = now + 90u;
    g_savestate_input_guard_max_until = now + 700u;
    savestate_hold_guard_arm();
}

/* Apply the release latch to one slot's finished button word.
 *
 * The PSX pad word is ACTIVE-LOW: a 0 bit is a pressed button. The snapshot is
 * deferred to the first sample rather than taken at arm time because the
 * settle window returns early above — taking it here means the latch picks up
 * precisely whatever is still held at the moment the window gives up. */
uint16_t savestate_hold_guard_apply(int slot, uint16_t buttons) {
    if (slot < 0 || slot >= PSX_MAX_PLAYERS) return buttons;
    const uint32_t bit = 1u << slot;
    if (g_savestate_hold_pending & bit) {
        g_savestate_hold_pending &= ~bit;
        g_savestate_hold_mask[slot] = (uint16_t)(~buttons);
    }
    uint16_t held = g_savestate_hold_mask[slot];
    if (!held) return buttons;
    held &= (uint16_t)(~buttons);        /* drop whatever has been released */
    g_savestate_hold_mask[slot] = held;
    return (uint16_t)(buttons | held);   /* report the rest as released */
}

/* Save-state input trace ring, queryable as `savestate_input_trace`.
 *
 * Every button word that reaches the guest, plus each guard arming, so the
 * question "did the confirm press leak into the game, and by which path" is
 * answered from recorded evidence instead of inference. Rule 3: inspection is
 * a debug-server command, never a printf.
 *
 * Idle frames are dropped and consecutive identical frames collapse into one
 * entry with a repeat count — a held button is otherwise 60 indistinguishable
 * entries a second, which would wrap the ring before anyone could read it. */
#define SS_TRACE_CAP 256u
typedef struct {
    uint32_t first_ms, last_ms;
    uint16_t raw, out, mask, rep;
    uint8_t  tag, slot, win;
} SsTraceEntry;
static SsTraceEntry s_ss_trace[SS_TRACE_CAP];
static uint64_t     s_ss_trace_seq;

/* 0 cap (host sample), 1 sio (what SIO was handed), 2 ovr (debug injection),
 * 3 arm-hold, 4 arm-notify-load, 5 arm-notify-save. */
static const char *const SS_TRACE_TAGS[] = {
    "cap", "sio", "ovr", "arm-hold", "arm-load", "arm-save"
};

static void savestate_trace_push(int tag, int slot, unsigned raw,
                                 unsigned out_btn, int windowed) {
    const uint32_t now = (uint32_t)SDL_GetTicks();
    const uint16_t mask = (uint16_t)(slot >= 0 && slot < PSX_MAX_PLAYERS
                                         ? g_savestate_hold_mask[slot] : 0);
    if (s_ss_trace_seq) {
        SsTraceEntry *prev =
            &s_ss_trace[(s_ss_trace_seq - 1) % SS_TRACE_CAP];
        if (prev->tag == (uint8_t)tag && prev->slot == (uint8_t)slot &&
            prev->raw == (uint16_t)raw && prev->out == (uint16_t)out_btn &&
            prev->win == (uint8_t)(windowed != 0) && prev->mask == mask &&
            prev->rep < 0xFFFFu) {
            prev->rep++;
            prev->last_ms = now;
            return;
        }
    }
    SsTraceEntry *e = &s_ss_trace[s_ss_trace_seq % SS_TRACE_CAP];
    e->first_ms = e->last_ms = now;
    e->raw  = (uint16_t)raw;
    e->out  = (uint16_t)out_btn;
    e->mask = mask;
    e->rep  = 1;
    e->tag  = (uint8_t)tag;
    e->slot = (uint8_t)(slot < 0 ? 0 : slot);
    e->win  = (uint8_t)(windowed != 0);
    s_ss_trace_seq++;
}

void savestate_diag_arm(const char *what) {
    int tag = 3;
    if (what && what[0] == 'n')
        tag = strstr(what, "load") ? 4 : 5;
    savestate_trace_push(tag, 0, 0xFFFFu, 0xFFFFu, 0);
}

void savestate_diag_note(const char *tag, int slot, unsigned raw,
                                unsigned out_btn, int windowed) {
    int t = 0;
    if (tag) {
        if (tag[0] == 's') t = 1;
        else if (tag[0] == 'o') t = 2;
    }
    /* Nothing pressed and nothing suppressed is not evidence of anything. */
    if (raw == 0xFFFFu && out_btn == 0xFFFFu && !windowed) return;
    savestate_trace_push(t, slot, raw, out_btn, windowed);
}

/* Render the most recent `count` entries as the body of a JSON object. */
int psx_savestate_trace_json(char *buf, unsigned size, int count) {
    if (!buf || size == 0) return 0;
    int total = (int)(s_ss_trace_seq < SS_TRACE_CAP ? s_ss_trace_seq
                                                    : SS_TRACE_CAP);
    if (count <= 0 || count > total) count = total;
    int n = snprintf(buf, size,
                          "\"total\":%llu,\"available\":%d,\"now_ms\":%u,"
                          "\"entries\":[",
                          (unsigned long long)s_ss_trace_seq, total,
                          (unsigned)SDL_GetTicks());
    const uint64_t start = s_ss_trace_seq - (uint64_t)count;
    for (int i = 0; i < count && n > 0 && (unsigned)n < size; i++) {
        const SsTraceEntry *e = &s_ss_trace[(start + (uint64_t)i) % SS_TRACE_CAP];
        n += snprintf(buf + n, size - (unsigned)n,
                           "%s{\"tag\":\"%s\",\"slot\":%u,\"raw\":\"%04X\","
                           "\"out\":\"%04X\",\"win\":%u,\"mask\":\"%04X\","
                           "\"rep\":%u,\"first_ms\":%u,\"last_ms\":%u}",
                           i == 0 ? "" : ",",
                           e->tag < (sizeof SS_TRACE_TAGS / sizeof *SS_TRACE_TAGS)
                               ? SS_TRACE_TAGS[e->tag] : "?",
                           (unsigned)e->slot, (unsigned)e->raw, (unsigned)e->out,
                           (unsigned)e->win, (unsigned)e->mask, (unsigned)e->rep,
                           (unsigned)e->first_ms, (unsigned)e->last_ms);
    }
    if (n > 0 && (unsigned)n < size)
        n += snprintf(buf + n, size - (unsigned)n, "]");
    return (n > 0 && (unsigned)n < size) ? n : 0;
}

int savestate_input_guard_active(void) {
    uint32_t now;
    if (g_savestate_input_guard_max_until == 0)
        return 0;
    now = (uint32_t)SDL_GetTicks();
    if ((int32_t)(now - g_savestate_input_guard_max_until) >= 0) {
        g_savestate_input_guard_min_until = 0;
        g_savestate_input_guard_max_until = 0;
        return 0;
    }
    if ((int32_t)(now - g_savestate_input_guard_min_until) >= 0 &&
        !psx_savestate_host_resume_inputs_held()) {
        g_savestate_input_guard_min_until = 0;
        g_savestate_input_guard_max_until = 0;
        return 0;
    }
    return 1;
}
int savestate_menu_open = 0;
static int savestate_menu_slot = 0;
int savestate_menu_ignore_toggle_release = 0;
int savestate_menu_open_key = 0;

static void savestate_menu_sync_overlay(void) {
    psx_savestate_menu_set_state(savestate_menu_open, savestate_menu_slot);
}

void savestate_menu_close(void) {
    savestate_menu_open = 0;
    savestate_menu_sync_overlay();
    host_osd_push("Save states closed", 800);
}

void savestate_menu_toggle(int opened_by_key) {
    if (psx_rewind_is_open())
        return;
    if (savestate_menu_open) {
        savestate_menu_close();
        return;
    }
    savestate_menu_open = 1;
    savestate_menu_ignore_toggle_release = 1;
    savestate_menu_open_key = opened_by_key;
    savestate_menu_sync_overlay();
}

void savestate_menu_set_slot(int slot) {
    if (slot < 0 || slot >= 12) return;
    savestate_menu_slot = slot;
    savestate_menu_sync_overlay();
}

void savestate_menu_move(int delta) {
    savestate_menu_slot += delta;
    while (savestate_menu_slot < 0)
        savestate_menu_slot += 12;
    while (savestate_menu_slot >= 12)
        savestate_menu_slot -= 12;
    savestate_menu_sync_overlay();
}

static int savestate_submit_slot(int slot, int save) {
    if (!save && !savestate_slot_exists(slot)) {
        char msg[32];
        snprintf(msg, sizeof(msg), "Slot %d is empty", slot + 1);
        host_osd_push(msg, 1200);
        return 0;
    }
    if (!save)
        savestate_input_guard_arm();
    else
        savestate_hold_guard_arm();
    if (psx_netplay_active()) {
        if (!psx_netplay_is_host()) {
            host_osd_push("Save states are host-only in netplay", 1500);
            return 0;
        }
        if (save)
            (void)psx_netplay_request_save(slot);
        else
            (void)psx_netplay_request_load(slot);
    } else if (save) {
        (void)savestate_request_save(slot);
    } else {
        (void)savestate_request_load(slot);
    }
    return 1;
}

void savestate_menu_submit(int save) {
    if (savestate_submit_slot(savestate_menu_slot, save) && savestate_menu_open) {
        savestate_menu_open = 0;
        savestate_menu_sync_overlay();
    }
}

static int savestate_menu_slot_from_key(int key) {
    if (key >= SDLK_1 && key <= SDLK_9)
        return (int)(key - SDLK_1);
    if (key == SDLK_0)
        return 9;
    if (key == SDLK_MINUS)
        return 10;
    if (key == SDLK_EQUALS)
        return 11;
    return -1;
}

void savestate_menu_handle_key(int key, int mod, int repeat) {
    int slot;
    if (repeat)
        return;
    if (savestate_menu_open_key && key == savestate_menu_open_key)
        return;
    slot = savestate_menu_slot_from_key(key);
    if (slot >= 0) {
        savestate_menu_slot = slot;
        savestate_menu_sync_overlay();
        return;
    }
    if (host_keymap_match(HOST_KEYMAP_SAVE_STATE_MENU, (int)key, mod) ||
        key == SDLK_ESCAPE || key == SDLK_BACKSPACE) {
        savestate_menu_close();
    } else if (key == SDLK_LEFT || key == SDLK_UP) {
        savestate_menu_move(-1);
    } else if (key == SDLK_RIGHT || key == SDLK_DOWN) {
        savestate_menu_move(+1);
    } else if (key == SDLK_s) {
        savestate_menu_submit(1);
    } else if (key == SDLK_l) {
        savestate_menu_submit(0);
    } else if (key == SDLK_RETURN || key == SDLK_SPACE) {
        savestate_menu_submit((mod & KMOD_SHIFT) != 0);
    }
}


/* The slot menu had no observable at all, which made it the one part of
 * this file a refactor could break silently. */
void psx_savestate_menu_debug(int *open, int *slot) {
    if (open) *open = savestate_menu_open;
    if (slot) *slot = savestate_menu_slot;
}
