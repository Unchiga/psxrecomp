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

/* ---- the selection the player is building ---------------------------------
 *
 * Picking cards with Up does NOT touch the card records — nothing in the whole
 * 28-byte array moves — so the selection lives elsewhere: a parallel table of
 * five 12-byte entries at 0x800EA030, one per hand slot.
 *
 *     +0  the slot's card object, always present
 *     +4  the selection marker object; ZERO while the card is unpicked
 *     +9  the 1-based order in which this card was picked
 *
 * with the number picked mirrored in a byte at 0x800E9F25. Measured by picking
 * slots 4, 1 and 3 in that order and watching entries appear with ordinals
 * 1, 2, 3. Pressing Up on an already-picked card is a no-op, not a toggle.
 *
 * ---- and the order the chain folds in --------------------------------------
 *
 * SELECTION order, not left-to-right hand order. Picking slot 4, then 1, then
 * 3 (Maiden of the Moonlight, Ganigumo, Beaked Snake — no fusion among them)
 * summoned Beaked Snake, the last card PICKED. Folding by hand position would
 * have ended on Maiden of the Moonlight, so the two hypotheses are cleanly
 * separated and this is the one that happened. That also pins the fold's base
 * case: when a step does not fuse, the incoming card is what stands.
 */
#define PSX_FUSION_SELECT     0x800EA030u
#define PSX_FUSION_SEL_STRIDE 12u
#define PSX_FUSION_SEL_MARK   4u    /* u32; nonzero => picked */
#define PSX_FUSION_SEL_ORDER  9u    /* byte; 1-based pick order */
#define PSX_FUSION_SEL_COUNT  0x800E9F25u

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
    typedef struct { int i, j; uint16_t result; int kind; } Hit;
    Hit hits[PSX_FUSION_HAND_MAX * (PSX_FUSION_HAND_MAX - 1) / 2];
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

    /* Strongest first, by the RESULT's printed stats -- not the materials',
     * which is why this needs the card table rather than the hand records.
     * Attack decides, defence breaks ties: a hand offering both Kaminari
     * Attack (1900/1500) and Nekogal #2 (1900/2000) should put Nekogal on top,
     * which is the call a player makes too. Insertion sort over at most ten. */
    for (int a = 1; a < nhits; a++) {
        Hit v = hits[a];
        int va = 0, vd = 0;
        psx_fusion_db_stats(v.result, &va, &vd, NULL);
        int b = a - 1;
        for (; b >= 0; b--) {
            int ba = 0, bd = 0;
            psx_fusion_db_stats(hits[b].result, &ba, &bd, NULL);
            if (ba > va || (ba == va && bd >= vd)) break;
            hits[b + 1] = hits[b];
        }
        hits[b + 1] = v;
    }

    unsigned p = 0;
    p += (unsigned)snprintf(out + p, cap - p,
                            "\"ready\":%d,\"in_hand\":%d,\"count\":%d,"
                            "\"fusions\":[",
                            psx_fusion_db_ready(), n, nhits);
    for (int k = 0; k < nhits && p + 240u < cap; k++) {
        int atk = 0, def = 0, type = -1;
        psx_fusion_db_stats(hits[k].result, &atk, &def, &type);
        p += (unsigned)snprintf(out + p, cap - p,
                                "%s{\"a\":%u,\"b\":%u,\"slot_a\":%d,"
                                "\"slot_b\":%d,\"result\":%u,\"kind\":%d,"
                                "\"atk\":%d,\"def\":%d,\"type\":%d}",
                                k ? "," : "",
                                hand[hits[k].i].id, hand[hits[k].j].id,
                                hand[hits[k].i].slot, hand[hits[k].j].slot,
                                hits[k].result, hits[k].kind, atk, def, type);
    }
    p += (unsigned)snprintf(out + p, cap - p, "]");
    return (int)p;
}

/* One step of the fold: what stands after putting `next` onto `carry`. A step
 * that does not fuse leaves the incoming card standing, which is what the game
 * does — so this never returns 0 for a real card. */
static uint16_t chain_step(uint16_t carry, uint16_t next, int *out_kind)
{
    const uint16_t r = psx_fusion_db_result(carry, next, out_kind);
    return r ? r : next;
}

int psx_fusion_assist_chain(PsxFusionCard *steps, int cap, uint16_t *out_result)
{
    if (out_result) *out_result = 0;
    if (!steps || cap <= 0) return 0;

    /* Gather the picked slots, ordered by the pick ordinal rather than by slot,
     * because that is the order the fold runs in. */
    PsxFusionCard picked[PSX_FUSION_HAND_MAX];
    uint8_t order[PSX_FUSION_HAND_MAX];
    int n = 0;
    for (int slot = 0; slot < PSX_FUSION_HAND_MAX; slot++) {
        const uint32_t e = PSX_FUSION_SELECT + (uint32_t)slot * PSX_FUSION_SEL_STRIDE;
        if (!psx_mod_read_word(e + PSX_FUSION_SEL_MARK)) continue;
        PsxFusionCard c;
        read_record(slot, &c);
        if (c.id < 1 || c.id > PSX_FUSION_CARD_ID_MAX) continue;
        picked[n] = c;
        order[n] = psx_mod_read_byte(e + PSX_FUSION_SEL_ORDER);
        n++;
    }
    for (int a = 1; a < n; a++) {
        PsxFusionCard c = picked[a];
        const uint8_t o = order[a];
        int b = a - 1;
        for (; b >= 0 && order[b] > o; b--) {
            picked[b + 1] = picked[b];
            order[b + 1] = order[b];
        }
        picked[b + 1] = c;
        order[b + 1] = o;
    }

    uint16_t carry = n ? picked[0].id : 0;
    for (int i = 0; i < n && i < cap; i++) steps[i] = picked[i];
    for (int i = 1; i < n; i++) carry = chain_step(carry, picked[i].id, NULL);
    if (out_result) *out_result = carry;
    return n < cap ? n : cap;
}

int psx_fusion_assist_chain_json(char *out, unsigned cap)
{
    if (!out || cap < 192u) return 0;
    PsxFusionCard steps[PSX_FUSION_HAND_MAX];
    uint16_t result = 0;
    const int n = psx_fusion_assist_chain(steps, PSX_FUSION_HAND_MAX, &result);

    unsigned p = 0;
    p += (unsigned)snprintf(out + p, cap - p,
                            "\"ready\":%d,\"picked\":%d,\"guest_count\":%u,"
                            "\"result\":%u,\"steps\":[",
                            psx_fusion_db_ready(), n,
                            psx_mod_read_byte(PSX_FUSION_SEL_COUNT), result);
    /* Replay the fold so each step reports the card standing after it — that
     * running value IS what the preview shows as the player picks. */
    uint16_t carry = n ? steps[0].id : 0;
    for (int i = 0; i < n && p + 160u < cap; i++) {
        int kind = PSX_FUSION_NONE;
        if (i) carry = chain_step(carry, steps[i].id, &kind);
        int atk = 0, def = 0;
        psx_fusion_db_stats(carry, &atk, &def, NULL);
        p += (unsigned)snprintf(out + p, cap - p,
                                "%s{\"slot\":%u,\"id\":%u,\"carry\":%u,"
                                "\"kind\":%d,\"atk\":%d,\"def\":%d}",
                                i ? "," : "", steps[i].slot, steps[i].id,
                                carry, kind, atk, def);
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
