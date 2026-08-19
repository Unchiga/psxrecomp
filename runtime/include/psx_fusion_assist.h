#ifndef PSX_FUSION_ASSIST_H
#define PSX_FUSION_ASSIST_H

/* In-duel fusion assistant — Yu-Gi-Oh! Forbidden Memories.
 *
 * Reads the live duel's hand out of guest RAM and works out what it can make,
 * using psx_fusion_db for every "what does X + Y give?" question. The guest
 * addresses for the duel side of that live here; the fusion data's addresses
 * live in psx_fusion_db.c.
 *
 * No UI yet: this is the logic half, plus the debug-server surface that makes
 * it checkable before a single pixel is drawn.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSX_FUSION_HAND_MAX 5

typedef struct PsxFusionCard {
    uint16_t id;
    uint16_t atk;
    uint16_t def;
    uint16_t flags;   /* record+8; 0x8000 while the card is live in hand */
    uint8_t  slot;    /* index into the guest's card-record array */
} PsxFusionCard;

/* Snapshot the hand. Returns the number of cards written (0 when no duel is in
 * progress, which is also what a non-duel screen reads as). */
int psx_fusion_assist_hand(PsxFusionCard *out, int cap);

/* ---- debug-server surface ---------------------------------------------- */

/* `fusion_hand`: the raw card-record window the hand is read from, so the
 * "which slots are the hand" rule can be checked against live data rather
 * than assumed. Writes a JSON array; returns bytes written. */
int psx_fusion_assist_hand_json(char *out, unsigned cap);

/* `fusion_list`: every pair in the current hand that combines, with what it
 * makes, best first by the result's attack. Writes a JSON object; returns
 * bytes written. */
int psx_fusion_assist_list_json(char *out, unsigned cap);

/* `fusion_try`: what the game would make of these two card ids, independent of
 * the hand. The direct read-back for psx_fusion_db. */
int psx_fusion_assist_try_json(char *out, unsigned cap, int a, int b);

/* The cards the player has picked so far, in PICK order (which is the order
 * the game folds them in), and what would stand if they summoned now. Writes
 * up to `cap` steps and returns how many; `out_result` is the standing card,
 * 0 when nothing is picked. This is the chain preview's whole logic half. */
int psx_fusion_assist_chain(PsxFusionCard *steps, int cap, uint16_t *out_result);

/* `fusion_chain`: the above as JSON, with the running result after each pick. */
int psx_fusion_assist_chain_json(char *out, unsigned cap);

#ifdef __cplusplus
}
#endif

#endif /* PSX_FUSION_ASSIST_H */
