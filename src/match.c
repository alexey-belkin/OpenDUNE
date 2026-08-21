/** @file src/match.c Who is playing this match, and what controls each house. */

#include <string.h>
#include "types.h"

#include "match.h"

#include "house.h"
#include "opendune.h"

typedef struct MatchSlot {
	uint8 houseID;
	MatchController controller;
} MatchSlot;

static struct {
	bool active;
	MatchSlot slot[MATCH_SLOT_MAX];
} s_match;

/**
 * Forget the match.  The campaign runs with no match at all, which is what
 * makes every query below fall back to the original single-player rule.
 */
void Match_Reset(void)
{
	memset(&s_match, 0, sizeof(s_match));
}

void Match_SetSlot(uint8 slot, uint8 houseID, MatchController controller)
{
	if (slot >= MATCH_SLOT_MAX) return;

	s_match.slot[slot].houseID    = houseID;
	s_match.slot[slot].controller = controller;
}

void Match_Begin(void)
{
	s_match.active = true;
}

bool Match_IsActive(void)
{
	return s_match.active;
}

/**
 * A match goes over the wire only when both slots are people.  Human against
 * AI and AI against AI are local matches in v1: they need no lockstep, no relay
 * and no turn loop.  See mp.md.
 */
bool Match_IsNetworked(void)
{
	uint8 i;

	if (!s_match.active) return false;

	for (i = 0; i < MATCH_SLOT_MAX; i++) {
		if (s_match.slot[i].controller != MATCH_CONTROLLER_HUMAN_LOCAL &&
		    s_match.slot[i].controller != MATCH_CONTROLLER_HUMAN_REMOTE) return false;
	}

	return true;
}

/**
 * Whether a house is driven by a person.
 *
 * This is the question the simulation actually wants wherever it used to ask
 * "is this g_playerHouseID".  The two agree in the campaign, where there is one
 * human and it owns the screen; they stop agreeing the moment a second person
 * is playing, and a global scalar cannot answer for both.
 */
bool Match_IsHumanControlled(uint8 houseID)
{
	uint8 i;

	if (!s_match.active) return (houseID == g_playerHouseID);

	for (i = 0; i < MATCH_SLOT_MAX; i++) {
		if (s_match.slot[i].controller == MATCH_CONTROLLER_NONE) continue;
		if (s_match.slot[i].houseID != houseID) continue;

		return (s_match.slot[i].controller == MATCH_CONTROLLER_HUMAN_LOCAL ||
		        s_match.slot[i].controller == MATCH_CONTROLLER_HUMAN_REMOTE);
	}

	return false;
}

/**
 * Two slots make the alliance matrix a single line: in a match, anyone who is
 * not you is against you.  The matrix as data waits for v2, when there can be
 * more than two of anything.
 */
bool Match_AreEnemies(uint8 houseID1, uint8 houseID2)
{
	if (!s_match.active) return false;
	if (houseID1 == HOUSE_INVALID || houseID2 == HOUSE_INVALID) return false;

	return (houseID1 != houseID2);
}

/**
 * The house in a slot, or HOUSE_INVALID if nobody is in it.  Callers that have
 * to do something for every house in the match walk the slots with this.
 */
uint8 Match_GetSlotHouse(uint8 slot)
{
	if (!s_match.active || slot >= MATCH_SLOT_MAX) return HOUSE_INVALID;
	if (s_match.slot[slot].controller == MATCH_CONTROLLER_NONE) return HOUSE_INVALID;

	return s_match.slot[slot].houseID;
}

/**
 * The other side, or HOUSE_INVALID in a solo match.
 */
uint8 Match_GetOpponent(uint8 houseID)
{
	uint8 i;

	if (!s_match.active) return HOUSE_INVALID;

	for (i = 0; i < MATCH_SLOT_MAX; i++) {
		if (s_match.slot[i].controller == MATCH_CONTROLLER_NONE) continue;
		if (s_match.slot[i].houseID == houseID) continue;

		return s_match.slot[i].houseID;
	}

	return HOUSE_INVALID;
}
