/* psx_fusion_assist.c — the duel side of the fusion assistant.
 *
 * ---- where the hand lives -------------------------------------------------
 *
 * The duel keeps one 28-byte record per card in play, based at 0x801A7AE4.
 * That address is not a guess: func_8001A280, the routine that resolves a
 * two-card summon, forms it from immediates —
 *
 *     s2 = 0x8015C424, s0 = 0x00048000
 *     v0 = slot*28 + s2 + s0            = slot*28 + 0x801A4424
 *     lh a0, 14016(v0)                  = slot*28 + 0x801A7AE4
 *
 * — and hands the two halfwords it reads there straight to the fusion lookup.
 * So this array is, by construction, exactly what the game fuses. Confirmed by
 * writing ids into it and watching the summon produce what those ids fuse to.
 *
 * Fields used (all u16):
 *     +0  card id
 *     +2  attack
 *     +4  defence
 *     +10 flags; 0x8000 on a live card, cleared the moment it is consumed by a
 *         summon, and 0x8400 once it is a monster ON the field
 *     +12 the record's own slot index, 0-based
 *
 * ---- what is settled, and what is not -------------------------------------
 *
 * On the opening turn slots 0..4 are exactly the hand and the summoned monster
 * lands at 5, and reading "id in range, flags 0x8000, not on the field" gives
 * the right five cards. That much is measured.
 *
 * It does NOT survive a summon. After fusing slots 0 and 1, slot 0 was reused
 * to hold the RESULT with its live flag cleared, but slot 1 -- the other
 * material, just as consumed -- was left reading 0x8000. So the live flag is
 * about the record, not about hand membership, and this reader will report a
 * spent material as still holdable from the second turn onward.
 *
 * The authoritative list is almost certainly the one the hand's sprite pass
 * walks: func_800170C8 is called once per displayed hand card with
 * a0 = record_base - 12 + slot*28, in descending slot order. Finding what
 * drives that loop is the fix; until then this reader is honest about the
 * first turn and wrong after it, which is why `fusion_hand` dumps the whole
 * 28-byte record for a window of slots -- the evidence to settle it is in
 * that output, not in a re-derivation.
 */

#include "psx_fusion_assist.h"

#include <stdio.h>
#include <string.h>

#include "mod_plugins.h"
#include "psx_fusion_db.h"

#define PSX_FUSION_RECORDS    0x801A7AE4u
#define PSX_FUSION_STRIDE     28u
#define PSX_FUSION_OFF_FLAGS  10u
#define PSX_FUSION_OFF_SLOT   12u
#define PSX_FUSION_FLAG_LIVE  0x8000u
#define PSX_FUSION_FLAG_FIELD 0x0400u
/* How many records `fusion_hand` reports. Wide enough to show the field slots
 * and any hand the game may have spread past slot 4. */
#define PSX_FUSION_WINDOW    12

static uint32_t rec_addr(int slot)
{
    return PSX_FUSION_RECORDS + (uint32_t)slot * PSX_FUSION_STRIDE;
}

static void read_record(int slot, PsxFusionCard *c)
{
    const uint32_t a = rec_addr(slot);
    c->id    = psx_mod_read_half(a);
    c->atk   = psx_mod_read_half(a + 2u);
    c->def   = psx_mod_read_half(a + 4u);
    c->flags = psx_mod_read_half(a + PSX_FUSION_OFF_FLAGS);
    c->slot  = (uint8_t)slot;
}

int psx_fusion_assist_hand(PsxFusionCard *out, int cap)
{
    if (!out || cap <= 0) return 0;
    int n = 0;
    for (int slot = 0; slot < PSX_FUSION_HAND_MAX && n < cap; slot++) {
        PsxFusionCard c;
        read_record(slot, &c);
        if (c.id < 1 || c.id > PSX_FUSION_CARD_ID_MAX) continue;
        if (!(c.flags & PSX_FUSION_FLAG_LIVE)) continue;
        if (c.flags & PSX_FUSION_FLAG_FIELD) continue;   /* already summoned */
        out[n++] = c;
    }
    return n;
}

int psx_fusion_assist_hand_json(char *out, unsigned cap)
{
    if (!out || cap < 128u) return 0;
    PsxFusionCard hand[PSX_FUSION_HAND_MAX];
    const int n = psx_fusion_assist_hand(hand, PSX_FUSION_HAND_MAX);

    unsigned p = 0;
    p += (unsigned)snprintf(out + p, cap - p,
                            "\"base\":\"0x%08X\",\"stride\":%u,\"in_hand\":%d,"
                            "\"hand\":[", PSX_FUSION_RECORDS,
                            PSX_FUSION_STRIDE, n);
    for (int i = 0; i < n && p + 64u < cap; i++)
        p += (unsigned)snprintf(out + p, cap - p, "%s%u", i ? "," : "",
                                hand[i].id);
    p += (unsigned)snprintf(out + p, cap - p, "],\"records\":[");
    /* The whole 28-byte record, not just the fields this module names: the
     * "which slots are the hand" rule is still an observation, and the next
     * person to question it should be able to answer from this output rather
     * than by re-deriving the record layout. */
    for (int slot = 0; slot < PSX_FUSION_WINDOW && p + 260u < cap; slot++) {
        PsxFusionCard c;
        read_record(slot, &c);
        p += (unsigned)snprintf(out + p, cap - p,
                                "%s{\"slot\":%d,\"addr\":\"0x%08X\",\"id\":%u,"
                                "\"atk\":%u,\"def\":%u,\"flags\":\"0x%04X\","
                                "\"index\":%u,\"words\":[",
                                slot ? "," : "", slot, rec_addr(slot),
                                c.id, c.atk, c.def, c.flags,
                                psx_mod_read_half(rec_addr(slot) +
                                                  PSX_FUSION_OFF_SLOT));
        for (unsigned w = 0; w < PSX_FUSION_STRIDE / 2u; w++)
            p += (unsigned)snprintf(out + p, cap - p, "%s%u", w ? "," : "",
                                    psx_mod_read_half(rec_addr(slot) + w * 2u));
        p += (unsigned)snprintf(out + p, cap - p, "]}");
    }
    p += (unsigned)snprintf(out + p, cap - p, "]");
    return (int)p;
}

int psx_fusion_assist_list_json(char *out, unsigned cap)
{
    if (!out || cap < 256u) return 0;
    PsxFusionCard hand[PSX_FUSION_HAND_MAX];
    const int n = psx_fusion_assist_hand(hand, PSX_FUSION_HAND_MAX);

    /* Every unordered pair. The fusion table is order-free and an equip gives
     * the same monster either way, so one direction per pair is the whole
     * answer -- but the ORDER the player selects in still decides which card
     * the game keeps, so report the pair as (first, second) with `first` being
     * the lower hand position, which is what selecting left-to-right does. */
    struct { int i, j; uint16_t result; int kind; } hits[
        PSX_FUSION_HAND_MAX * (PSX_FUSION_HAND_MAX - 1) / 2];
    int nhits = 0;
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            int kind = PSX_FUSION_NONE;
            const uint16_t r =
                psx_fusion_db_result(hand[i].id, hand[j].id, &kind);
            if (!r) continue;
            hits[nhits].i = i;
            hits[nhits].j = j;
            hits[nhits].result = r;
            hits[nhits].kind = kind;
            nhits++;
        }
    }

    /* Reported in hand order. Ranking by the RESULT's attack -- what a "best
     * fusion available" line wants -- needs the per-card stats table, which is
     * not the record array (that holds the stats of the instance in hand, not
     * of the card a fusion would produce) and has not been located yet. Better
     * an honest list in hand order than a plausible-looking wrong order. */
    unsigned p = 0;
    p += (unsigned)snprintf(out + p, cap - p,
                            "\"ready\":%d,\"in_hand\":%d,\"count\":%d,"
                            "\"fusions\":[",
                            psx_fusion_db_ready(), n, nhits);
    for (int k = 0; k < nhits && p + 200u < cap; k++) {
        p += (unsigned)snprintf(out + p, cap - p,
                                "%s{\"a\":%u,\"b\":%u,\"slot_a\":%d,"
                                "\"slot_b\":%d,\"result\":%u,\"kind\":%d}",
                                k ? "," : "",
                                hand[hits[k].i].id, hand[hits[k].j].id,
                                hand[hits[k].i].slot, hand[hits[k].j].slot,
                                hits[k].result, hits[k].kind);
    }
    p += (unsigned)snprintf(out + p, cap - p, "]");
    return (int)p;
}

int psx_fusion_assist_try_json(char *out, unsigned cap, int a, int b)
{
    if (!out || cap < 128u) return 0;
    int kind = PSX_FUSION_NONE;
    const uint16_t r = psx_fusion_db_result((uint16_t)a, (uint16_t)b, &kind);
    return snprintf(out, cap,
                    "\"ready\":%d,\"a\":%d,\"b\":%d,\"result\":%u,\"kind\":%d",
                    psx_fusion_db_ready(), a, b, r, kind);
}
