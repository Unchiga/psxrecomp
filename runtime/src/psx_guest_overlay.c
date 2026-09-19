/* psx_guest_overlay.c — see psx_guest_overlay.h. */

#include "psx_guest_overlay.h"

/* Fixed capacity, no allocation: registration runs during static
 * initialisation. Titles can legitimately compose several independent HUD,
 * privacy, diagnostic and modal layers, and they reached twelve: Forbidden
 * Memories' thirteenth was its mode-select confirm prompt, which registers
 * LAST so it paints over everything, so the table quietly refused the one
 * overlay whose whole job was to be on top. Its logic still ran -- the prompt
 * opened, took the pad and answered correctly -- with nothing on screen.
 * Sixteen now, and Vulkan's VK_OVL_MAX carries all of them plus its menu and
 * toast layers. */
#define PSX_GUEST_OVERLAY_MAX 16

static PsxGuestOverlay s_ov[PSX_GUEST_OVERLAY_MAX];
static int             s_count;
static int             s_dropped;

int psx_guest_overlay_register(const PsxGuestOverlay *ov)
{
    if (!ov || !ov->image || !ov->origin) return 0;
    /* Count refusals. Most callers register with (void) because there is
     * nothing useful to do about a full table, and a dropped overlay looks
     * from the outside exactly like one that decided not to draw -- which is
     * how a missing modal prompt went unexplained. `guest_overlays` on the
     * debug server reports this. */
    if (s_count >= PSX_GUEST_OVERLAY_MAX) { s_dropped++; return 0; }
    s_ov[s_count++] = *ov;
    return 1;
}

int psx_guest_overlay_count(void) { return s_count; }
int psx_guest_overlay_max(void) { return PSX_GUEST_OVERLAY_MAX; }
int psx_guest_overlay_dropped(void) { return s_dropped; }

const PsxGuestOverlay *psx_guest_overlay_at(int i)
{
    if (i < 0 || i >= s_count) return 0;
    return &s_ov[i];
}

int psx_guest_overlay_any_needs_present(void)
{
    for (int i = 0; i < s_count; i++)
        if (s_ov[i].needs_present && s_ov[i].needs_present()) return 1;
    return 0;
}
