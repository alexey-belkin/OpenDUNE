/** @file src/match.h Who is playing this match, and what controls each house. */

#ifndef MATCH_H
#define MATCH_H

enum {
	MATCH_SLOT_MAX = 2                                      /*!< v1: two houses, no more.  See mp.md. */
};

/**
 * What drives a house.  The simulation only ever asks whether a house is human
 * or not; the distinction between a local and a remote human belongs to the
 * network layer, which needs to know whose orders arrive over the wire.
 */
typedef enum MatchController {
	MATCH_CONTROLLER_NONE,                                  /*!< Empty slot. */
	MATCH_CONTROLLER_HUMAN_LOCAL,                           /*!< The person at this keyboard. */
	MATCH_CONTROLLER_HUMAN_REMOTE,                          /*!< A person at the other end of the wire. */
	MATCH_CONTROLLER_AI                                     /*!< The built-in AI. */
} MatchController;

extern void Match_Reset(void);
extern void Match_SetSlot(uint8 slot, uint8 houseID, MatchController controller);
extern void Match_Begin(void);

extern bool Match_IsActive(void);
extern bool Match_IsNetworked(void);
extern bool Match_IsHumanControlled(uint8 houseID);
extern bool Match_AreEnemies(uint8 houseID1, uint8 houseID2);
extern uint8 Match_GetOpponent(uint8 houseID);

#endif /* MATCH_H */
