/* psx_guest_overlay.c — see psx_guest_overlay.h. */

#include "psx_guest_overlay.h"

/* Fixed capacity, no allocation: registration runs during static
 * initialisation. Three is what this title needs; eight leaves room without
 * pretending the composite loop is free.  */
#define PSX_GUEST_OVERLAY_MAX 8

static PsxGuestOverlay s_ov[PSX_GUEST_OVERLAY_MAX];
static int             s_count;

int psx_guest_overlay_register(const PsxGuestOverlay *ov)
{
    if (!ov || !ov->image || !ov->origin) return 0;
    if (s_count >= PSX_GUEST_OVERLAY_MAX) return 0;
    s_ov[s_count++] = *ov;
    return 1;
}

int psx_guest_overlay_count(void) { return s_count; }

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
