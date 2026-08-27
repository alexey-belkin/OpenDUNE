/** @file src/structure.c %Structure handling routines. */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "types.h"
#include "os/common.h"
#include "os/error.h"
#include "os/math.h"
#include "os/strings.h"

#include "structure.h"
#include "mpcommand.h"
#include "mpturn.h"

#include "animation.h"
#include "audio/sound.h"
#include "explosion.h"
#include "gfx.h"
#include "gui/gui.h"
#include "gui/font.h"
#include "gui/widget.h"
#include "house.h"
#include "inifile.h"
#include "map.h"
#include "match.h"
#include "opendune.h"
#include "pool/pool.h"
#include "pool/house.h"
#include "pool/structure.h"
#include "pool/team.h"
#include "pool/unit.h"
#include "scenario.h"
#include "doctrine.h"
#include "skirmish.h"
#include "sprites.h"
#include "string.h"
#include "table/strings.h"
#include "team.h"
#include "tile.h"
#include "timer.h"
#include "tools.h"
#include "unit.h"


Structure *g_structureActive = NULL;
uint16 g_structureActivePosition = 0;
uint16 g_structureActiveType = 0;

static bool s_debugInstantBuild = false; /*!< When non-zero, constructions are almost instant. */
static uint32 s_tickStructureDegrade   = 0; /*!< Indicates next time Degrade function is executed. */
static uint32 s_tickStructureStructure = 0; /*!< Indicates next time Structures function is executed. */
static uint32 s_tickStructureScript    = 0; /*!< Indicates next time Script function is executed. */
static uint32 s_tickStructurePalace    = 0; /*!< Indicates next time Palace function is executed. */

uint16 g_structureIndex;

static void Structure_AutoRepair_Consider(Structure *s, House *h);

typedef struct UnitBuildQueue {
	uint16 type;
	uint8 pending;
	uint8 ready;                                            /*!< Construction Yard: finished and waiting, beyond the one held in linkedID. */
	uint16 readyType;                                       /*!< What those are.  The whole stack is one type; see Structure_Queue_Stash(). */
} UnitBuildQueue;

/* Queues contain repeat orders for one type only.  The Starport retains its
 * separate order cart. */
static UnitBuildQueue s_unitBuildQueue[STRUCTURE_INDEX_MAX_HARD];

/* How many finished buildings the local player has already clicked a spot for
 * whose placement command has not run yet.  Local presentation state and
 * nothing else: it decides only whether the cursor stays in placement mode, so
 * that a player with four slabs ready can put all four down with four clicks
 * without the count going stale between the click and the turn that executes
 * it.  Never read by the simulation -- on the other client it simply stays at
 * zero. */
static uint8 s_yardPlaceCommitted[STRUCTURE_INDEX_MAX_HARD];

/* Where a factory sends what it produces, 0 for "just leave the bay".  Kept
 * out of the savegame deliberately: the original format has no room for a tile
 * per structure, and a rally point is a convenience of the current session, not
 * state the battle depends on. */
static uint16 s_structureRally[STRUCTURE_INDEX_MAX_HARD];

static void Structure_CancelBuild(Structure *s);

/** Point a factory at a tile, or clear it with an invalid one. */
void Structure_SetRallyPoint(Structure *s, uint16 packed)
{
	if (s == NULL || s->o.index >= STRUCTURE_INDEX_MAX_HARD) return;

	s_structureRally[s->o.index] = Map_IsValidPosition(packed) ? packed : 0;
}

uint16 Structure_GetRallyPoint(const Structure *s)
{
	if (s == NULL || s->o.index >= STRUCTURE_INDEX_MAX_HARD) return 0;

	return s_structureRally[s->o.index];
}

static UnitBuildQueue *Structure_Queue_Get(const Structure *s)
{
	if (s == NULL || s->o.index >= STRUCTURE_INDEX_MAX_SOFT) return NULL;
	return &s_unitBuildQueue[s->o.index];
}

static bool Structure_Queue_IsYard(const Structure *s)
{
	return s != NULL && s->o.type == STRUCTURE_CONSTRUCTION_YARD;
}

/** Drop the repeat orders.  What the structure has already finished is not
 * touched: cancelling the sixth of six is not a reason to lose the first. */
static void Structure_Queue_Clear(Structure *s)
{
	UnitBuildQueue *queue = Structure_Queue_Get(s);

	if (queue == NULL) return;
	queue->type = UNIT_INVALID;
	queue->pending = 0;
}

/** Everything, for a pool slot about to be handed to a different building. */
static void Structure_Queue_Reset(Structure *s)
{
	UnitBuildQueue *queue = Structure_Queue_Get(s);

	if (queue == NULL) return;
	queue->type = UNIT_INVALID;
	queue->pending = 0;
	queue->ready = 0;
	queue->readyType = 0xFFFF;
	s_yardPlaceCommitted[s->o.index] = 0;
}

bool Structure_Queue_CanOrder(const Structure *s)
{
	const StructureInfo *si;

	if (s == NULL || s->o.index >= STRUCTURE_INDEX_MAX_SOFT || !Match_IsHumanControlled(s->o.houseID)) return false;
	if (s->o.type == STRUCTURE_REPAIR || s->o.type == STRUCTURE_STARPORT) return false;

	si = &g_table_structureInfo[s->o.type];
	if (!si->o.flags.factory) return false;

	/* The Construction Yard picks from the structure table, every other factory
	 * from the unit table, and objectType indexes whichever of the two.  Reading
	 * it against the wrong bound is how a yard set to build a Palace looked like
	 * a factory building unit 12. */
	return s->objectType < (Structure_Queue_IsYard(s) ? STRUCTURE_MAX : UNIT_MAX);
}

/**
 * How many finished buildings a Construction Yard is holding, ready to be put
 * down.  The head of the stack is the one in linkedID whenever the yard has not
 * moved on to the next order; the rest are a count and a type, and are created
 * at the moment they are placed.  See Structure_Queue_Stash().
 */
uint16 Structure_Queue_GetReadyCount(const Structure *s)
{
	const UnitBuildQueue *queue;

	if (!Structure_Queue_CanOrder(s) || !Structure_Queue_IsYard(s)) return 0;

	queue = Structure_Queue_Get(s);
	return queue->ready + ((s->o.linkedID != 0xFF && s->countDown == 0) ? 1 : 0);
}

/**
 * Everything the structure still owes the player: what is finished and waiting,
 * what is on the bench right now, and what is queued behind it.  This is the
 * number the +1/-1 clicks move.
 */
uint16 Structure_Queue_GetOrderCount(const Structure *s)
{
	const UnitBuildQueue *queue;
	uint16 count;

	if (!Structure_Queue_CanOrder(s)) return 0;

	queue = Structure_Queue_Get(s);
	count = queue->pending;
	if (s->o.linkedID != 0xFF) count++;
	if (Structure_Queue_IsYard(s)) count += queue->ready;

	return count;
}

bool Structure_Queue_AddOrder(Structure *s)
{
	UnitBuildQueue *queue;

	if (!Structure_Queue_CanOrder(s)) return false;

	queue = Structure_Queue_Get(s);
	if (s->o.linkedID == 0xFF) {
		queue->type = s->objectType;
		return Structure_BuildObject(s, queue->type);
	}

	if (queue->type != s->objectType) {
		queue->type = s->objectType;
		queue->pending = 0;
	}

	if (queue->pending == 99) return false;

	queue->pending++;
	return true;
}

/**
 * Give a finished building back to the treasury.  A completed structure is paid
 * for in full, so the refund is the full price -- the same arithmetic
 * Structure_CancelBuild() does with a countDown of zero.
 */
static void Structure_Queue_RefundOneReady(Structure *s)
{
	UnitBuildQueue *queue = Structure_Queue_Get(s);
	House *h;

	if (queue == NULL || queue->ready == 0 || queue->readyType >= STRUCTURE_MAX) return;

	h = House_Get_ByIndex(s->o.houseID);
	if (h != NULL) h->credits += g_table_structureInfo[queue->readyType].o.buildCredits;

	queue->ready--;
	if (queue->ready == 0) queue->readyType = 0xFFFF;
}

/** Empty the stack of finished buildings, refunding each.  Used when the yard
 * is told to build something else, which is what already happened to the single
 * finished building the original game could be holding. */
void Structure_Queue_RefundReady(Structure *s)
{
	UnitBuildQueue *queue = Structure_Queue_Get(s);

	if (queue == NULL) return;
	while (queue->ready != 0) Structure_Queue_RefundOneReady(s);
	s_yardPlaceCommitted[s->o.index] = 0;
}

bool Structure_Queue_RemoveOrder(Structure *s)
{
	UnitBuildQueue *queue;

	if (!Structure_Queue_CanOrder(s)) return false;

	queue = Structure_Queue_Get(s);

	/* Take the most recent order off first: what is queued, then what is on the
	 * bench, and only then something already finished.  A player counting down
	 * from six expects the sixth to go, not the first. */
	if (queue->pending != 0) {
		queue->pending--;
		return true;
	}

	if (s->o.linkedID != 0xFF) {
		Structure_CancelBuild(s);
		return true;
	}

	/* Nothing on the bench: the yard has moved everything it finished onto the
	 * stack, so the last order left is one of those. */
	if (Structure_Queue_IsYard(s) && queue->ready != 0) {
		Structure_Queue_RefundOneReady(s);
		return true;
	}

	return false;
}

/**
 * A Construction Yard finishes one building and then stops, because linkedID is
 * a single slot and the finished building sits in it until the player finds a
 * spot.  That is the whole reason a queue of buildings needs more than a
 * counter: to keep working, the yard has to hand the finished one somewhere
 * else first.
 *
 * It goes nowhere.  The completed Structure is freed and remembered as a count
 * and a type, and one is created again at the moment it is placed
 * (Structure_Queue_PlaceReady).  Keeping N of them allocated instead would
 * spend N slots of the structure pool on buildings that are not on the map, and
 * would have to be written into the savegame, which has no room for it.
 */
static void Structure_Queue_Stash(Structure *s)
{
	UnitBuildQueue *queue;
	Structure *done;

	if (!Structure_Queue_CanOrder(s) || !Structure_Queue_IsYard(s)) return;

	queue = Structure_Queue_Get(s);
	if (queue->pending == 0 || queue->ready == 255) return;
	if (s->o.linkedID == 0xFF || s->countDown != 0 || s->o.flags.s.onHold) return;

	done = Structure_Get_ByIndex(s->o.linkedID);
	if (done == NULL) return;

	/* One stack, one type.  Choosing a different building cancels what the yard
	 * is holding (Structure_BuildObject -> Structure_CancelBuild), so the two
	 * can never disagree. */
	queue->readyType = done->o.type;
	Structure_Free(done);

	queue->ready++;
	s->o.linkedID = 0xFF;
	Structure_SetState(s, STRUCTURE_STATE_IDLE);
}

static void Structure_Queue_StartNext(Structure *s)
{
	UnitBuildQueue *queue;

	Structure_Queue_Stash(s);

	if (!Structure_Queue_CanOrder(s) || s->o.linkedID != 0xFF || s->state != STRUCTURE_STATE_IDLE) return;

	queue = Structure_Queue_Get(s);
	if (queue->pending == 0) return;

	if (Structure_Queue_IsYard(s)) {
		if (queue->type >= STRUCTURE_MAX) return;
		/* A tree change or a rival's capture can put the queued building out of
		 * reach between two orders.  Nothing has been charged for it yet, so
		 * dropping the queue is the whole of the cleanup. */
		if ((Structure_GetBuildable(s) & (1 << queue->type)) == 0) {
			queue->pending = 0;
			return;
		}
	} else {
		if (queue->type >= UNIT_MAX) return;
	}

	if (Structure_BuildObject(s, queue->type)) queue->pending--;
}

/**
 * Put down the next building a Construction Yard has finished.
 *
 * Two shapes of "finished" arrive here.  Normally the building is the one in
 * linkedID and this is what the original game did.  When the yard has already
 * moved on to the next order the head of the stack is only a count and a type
 * (Structure_Queue_Stash), and the Structure is created here -- and freed again
 * if the spot is refused, so a refusal costs the player nothing and the pool
 * ends where it began.
 *
 * @param yard The Construction Yard.
 * @param packed Where the player clicked.
 * @param type Out: what was placed.  Valid even for the types Structure_Place()
 *        frees on the spot, which is every wall and slab.
 * @return The placed structure, or NULL if nothing was ready or the spot was
 *         refused.  Already freed for walls and slabs -- read only *type then.
 */
Structure *Structure_Queue_PlaceReady(Structure *yard, uint16 packed, uint16 *type)
{
	UnitBuildQueue *queue;
	Structure *s;

	if (type != NULL) *type = 0xFFFF;
	if (yard == NULL || !Structure_Queue_IsYard(yard)) return NULL;

	queue = Structure_Queue_Get(yard);

	if (yard->o.linkedID != STRUCTURE_INVALID) {
		/* Only what the yard has finished.  Placing a building still under
		 * construction leaves the yard counting down towards an object it no
		 * longer has, and it never builds anything again -- which is what the
		 * first human-versus-AI run did for 40000 ticks. */
		if (yard->countDown != 0) return NULL;

		s = Structure_Get_ByIndex(yard->o.linkedID);
		if (s == NULL) return NULL;

		if (type != NULL) *type = s->o.type;

		/* A refused spot leaves the building with the yard, so the player can
		 * try somewhere else -- which is also what keeps a failed command
		 * harmless. */
		if (!Structure_Place(s, packed)) return NULL;

		yard->o.linkedID = STRUCTURE_INVALID;
		return s;
	}

	if (queue == NULL || queue->ready == 0 || queue->readyType >= STRUCTURE_MAX) return NULL;

	if (type != NULL) *type = queue->readyType;

	s = Structure_Create(STRUCTURE_INDEX_INVALID, (uint8)queue->readyType, yard->o.houseID, 0xFFFF);
	if (s == NULL) return NULL;

	if (!Structure_Place(s, packed)) {
		Structure_Free(s);
		return NULL;
	}

	queue->ready--;
	if (queue->ready == 0) queue->readyType = 0xFFFF;

	return s;
}

/**
 * How many more spots the player may click before the cursor has to leave
 * placement mode.  Live count less what has already been clicked for and not
 * yet executed, so a player putting four slabs down in four quick clicks cannot
 * order a fifth that does not exist.
 */
uint16 Structure_Queue_GetPlaceableCount(const Structure *s)
{
	uint16 ready;
	uint16 committed;

	if (s == NULL || s->o.index >= STRUCTURE_INDEX_MAX_SOFT) return 0;

	ready = Structure_Queue_GetReadyCount(s);
	committed = s_yardPlaceCommitted[s->o.index];

	return (ready > committed) ? ready - committed : 0;
}

/** Note that a spot has been clicked for one of them.  @see Structure_Queue_GetPlaceableCount */
void Structure_Queue_PlaceCommit(Structure *s)
{
	if (s == NULL || s->o.index >= STRUCTURE_INDEX_MAX_SOFT) return;
	if (s_yardPlaceCommitted[s->o.index] < 255) s_yardPlaceCommitted[s->o.index]++;
}

/** The command has run, whether it placed anything or not. */
void Structure_Queue_PlaceRelease(Structure *s)
{
	if (s == NULL || s->o.index >= STRUCTURE_INDEX_MAX_SOFT) return;
	if (s_yardPlaceCommitted[s->o.index] != 0) s_yardPlaceCommitted[s->o.index]--;
}

/**
 * Loop over all structures, preforming various of tasks.
 */
/**
 * Put the module's schedulers back to the start of time.
 *
 * They hold absolute deadlines -- "next run at g_timerGame + delta" -- so a
 * match that starts with the clock at zero inherits deadlines from the last one
 * and simply does not run until the clock catches up.  Every match has to begin
 * from a known state, which matters for a rematch, for a reconnect, and for any
 * harness that plays twice in one process.  See mp.md.
 */
void Structure_ResetTicks(void)
{
	s_tickStructureDegrade   = 0;
	s_tickStructureStructure = 0;
	s_tickStructureScript    = 0;
	s_tickStructurePalace    = 0;

	/* Both are indexed by structure index, and the pool hands the same index to
	 * a different building next match. */
	memset(s_unitBuildQueue,  0, sizeof(s_unitBuildQueue));
	memset(s_structureRally,  0, sizeof(s_structureRally));
}

void GameLoop_Structure(void)
{
	PoolFindStruct find;
	bool tickDegrade   = false;
	bool tickStructure = false;
	bool tickScript    = false;
	bool tickPalace    = false;

	if (s_tickStructureDegrade <= g_timerGame && g_campaignID > 1) {
		tickDegrade = true;
		s_tickStructureDegrade = g_timerGame + Tools_AdjustToGameSpeed(10800, 5400, 21600, true);
	}

	if (s_tickStructureStructure <= g_timerGame || s_debugInstantBuild) {
		tickStructure = true;
		s_tickStructureStructure = g_timerGame + Tools_AdjustToGameSpeed(30, 15, 60, true);
	}

	if (s_tickStructureScript <= g_timerGame) {
		tickScript = true;
		s_tickStructureScript = g_timerGame + 5;
	}

	if (s_tickStructurePalace <= g_timerGame) {
		tickPalace = true;
		s_tickStructurePalace = g_timerGame + 60;
	}

	find.houseID = HOUSE_INVALID;
	find.index   = 0xFFFF;
	find.type    = 0xFFFF;

	if (g_debugScenario) return;

	while (true) {
		const StructureInfo *si;
		const HouseInfo *hi;
		Structure *s;
		House *h;

		s = Structure_Find(&find);
		if (s == NULL) break;
		if (s->o.type == STRUCTURE_SLAB_1x1 || s->o.type == STRUCTURE_SLAB_2x2 || s->o.type == STRUCTURE_WALL) continue;

		si = &g_table_structureInfo[s->o.type];
		h  = House_Get_ByIndex(s->o.houseID);
		hi = &g_table_houseInfo[h->index];

		g_scriptCurrentObject    = &s->o;
		g_scriptCurrentStructure = s;
		g_scriptCurrentUnit      = NULL;
		g_scriptCurrentTeam      = NULL;

		if (tickPalace && s->o.type == STRUCTURE_PALACE) {
			if (s->countDown != 0) {
				s->countDown--;

				if (s->o.houseID == g_playerHouseID) {
					GUI_Widget_ActionPanel_Draw(true);
				}
			}

			/* Check if we have to fire the weapon for the AI immediately */
			if (s->countDown == 0 && !h->flags.human && h->flags.isAIActive) {
				Structure_ActivateSpecial(s);
			}
		}

		if (tickDegrade && s->o.flags.s.degrades && s->o.hitpoints > si->o.hitpoints / 2) {
			Structure_Damage(s, hi->degradingAmount, 0);
		}

		if (tickStructure) {
			Structure_Queue_StartNext(s);
			/* Repeat production must not remain stuck after a temporary funds
			 * shortage.  Construction yards retain their original manual flow. */
			if (Structure_Queue_CanOrder(s) && s->o.flags.s.onHold && s->countDown != 0 && s->o.linkedID != 0xFF && h->credits != 0) {
				s->o.flags.s.onHold = false;
			}

			/* Before the chain below, which is what actually pays for and
			 * applies the repair: this only ever sets the flag the player's own
			 * Repair button sets. */
			Structure_AutoRepair_Consider(s, h);

			if (s->o.flags.s.upgrading) {
				uint16 upgradeCost = si->o.buildCredits / 40;

				if (upgradeCost <= h->credits) {
					h->credits -= upgradeCost;

					if (s->upgradeTimeLeft > 5) {
						s->upgradeTimeLeft -= 5;
					} else {
						s->upgradeLevel++;
						s->o.flags.s.upgrading = false;

						/* Ordos Heavy Vehicle gets the last upgrade for free */
						if (s->o.houseID == HOUSE_ORDOS && s->o.type == STRUCTURE_HEAVY_VEHICLE && s->upgradeLevel == 2) s->upgradeLevel = 3;

						s->upgradeTimeLeft = Structure_IsUpgradable(s) ? 100 : 0;
					}
				} else {
					s->o.flags.s.upgrading = false;
				}
			} else if (s->o.flags.s.repairing) {
				uint16 repairCost;

				/* ENHANCEMENT -- The calculation of the repaircost is a bit unfair in Dune2, because of rounding errors (they use a 256 float-resolution, which is not sufficient) */
				if (g_dune2_enhanced) {
					repairCost = si->o.buildCredits * 2 / si->o.hitpoints;
				} else {
					repairCost = ((2 * 256 / si->o.hitpoints) * si->o.buildCredits + 128) / 256;
				}

				if (repairCost <= h->credits) {
					h->credits -= repairCost;

					/* AIs repair in early games slower than in later games */
					if (Match_IsHumanControlled(s->o.houseID) || g_campaignID >= 3) {
						s->o.hitpoints += 5;
					} else {
						s->o.hitpoints += 3;
					}

					if (s->o.hitpoints > si->o.hitpoints) {
						s->o.hitpoints = si->o.hitpoints;
						s->o.flags.s.repairing = false;
						s->o.flags.s.onHold = false;
					}
				} else {
					s->o.flags.s.repairing = false;
				}
			} else {
				if (!s->o.flags.s.onHold && s->countDown != 0 && s->o.linkedID != 0xFF && s->state == STRUCTURE_STATE_BUSY && si->o.flags.factory) {
					ObjectInfo *oi;
					uint16 buildSpeed;
					uint16 buildCost;

					if (s->o.type == STRUCTURE_CONSTRUCTION_YARD) {
						oi = &g_table_structureInfo[s->objectType].o;
					} else if (s->o.type == STRUCTURE_REPAIR) {
						oi = &g_table_unitInfo[Unit_Get_ByIndex(s->o.linkedID)->o.type].o;
					} else {
						oi = &g_table_unitInfo[s->objectType].o;
					}

					buildSpeed = 256;
					if (s->o.hitpoints < si->o.hitpoints) {
						buildSpeed = s->o.hitpoints * 256 / si->o.hitpoints;
					}

					/* For AIs, we slow down building speed in all but the last campaign */
					if (!Match_IsHumanControlled(s->o.houseID)) {
						if (buildSpeed > g_campaignID * 20 + 95) buildSpeed = g_campaignID * 20 + 95;
					}

					buildCost = oi->buildCredits * 256 / oi->buildTime;

					if (buildSpeed < 256) {
						buildCost = buildSpeed * buildCost / 256;
					}

					if (s->o.type == STRUCTURE_REPAIR && buildCost > 4) {
						buildCost /= 4;
					}

					buildCost += s->buildCostRemainder;

					if (buildCost / 256 <= h->credits) {
						s->buildCostRemainder = buildCost & 0xFF;
						h->credits -= buildCost / 256;

						/* The only place production is paid for, so the only place
						 * a skirmish can tell army spending from economy spending. */
						if (Skirmish_IsActive()) Skirmish_War_Charge(s, buildCost / 256);

						if (buildSpeed < s->countDown) {
							s->countDown -= buildSpeed;
						} else {
							s->countDown = 0;
							s->buildCostRemainder = 0;

							Structure_SetState(s, STRUCTURE_STATE_READY);

							/* Who gets told and who gets served are two different
							 * questions, and this used to be one branch on
							 * g_playerHouseID answering both.  The message is for
							 * whoever is watching; putting the building down for
							 * a house that has nobody to do it is the AI, and a
							 * second person's house is neither -- it was silently
							 * getting the AI's free placement, which is how a
							 * house nobody was playing built itself a base. */
							if (Match_IsHumanControlled(s->o.houseID)) {
								if (s->o.houseID == g_playerHouseID &&
								    s->o.type != STRUCTURE_BARRACKS && s->o.type != STRUCTURE_WOR_TROOPER) {
									uint16 stringID = STR_IS_COMPLETED_AND_AWAITING_ORDERS;
									if (s->o.type == STRUCTURE_HIGH_TECH) stringID = STR_IS_COMPLETE;
									if (s->o.type == STRUCTURE_CONSTRUCTION_YARD) stringID = STR_IS_COMPLETED_AND_READY_TO_PLACE;

									GUI_DisplayText("%s %s", 0, String_Get_ByIndex(oi->stringID_full), String_Get_ByIndex(stringID));

									Sound_Output_Feedback(0);
								}
							} else if (s->o.type == STRUCTURE_CONSTRUCTION_YARD) {
								/* An AI immediately places the structure when it is done building */
								Structure *ns;
								uint8 i;
								bool placed = false;

								ns = Structure_Get_ByIndex(s->o.linkedID);
								s->o.linkedID = 0xFF;

								/* The AI places structures which are operational immediately */
								Structure_SetState(s, STRUCTURE_STATE_IDLE);

								/* Find the position to place the structure */
								for (i = 0; i < 5; i++) {
									if (ns->o.type != h->ai_structureRebuild[i][0]) continue;

									if (!Structure_Place(ns, h->ai_structureRebuild[i][1])) continue;

									h->ai_structureRebuild[i][0] = 0;
									h->ai_structureRebuild[i][1] = 0;
									placed = true;
									break;
								}

								/* Nothing left to rebuild means this came out of the skirmish base
								 * plan, which knows where the structure was meant to go. */
								if (!placed && Skirmish_IsActive()) {
									uint16 planned = Skirmish_Plan_TakePosition(h, ns->o.type);

									if (planned != 0xFFFF) placed = Structure_Place(ns, planned);
								}

								/* If the AI no longer had in memory where to store the structure, free it and forget about it */
								if (!placed) {
									const StructureInfo *nsi = &g_table_structureInfo[ns->o.type];

									h->credits += nsi->o.buildCredits;

									Structure_Free(ns);
								}
							}
						}
					} else {
						/* Out of money means the building gets put on hold */
						if (Match_IsHumanControlled(s->o.houseID)) {
							s->o.flags.s.onHold = true;
							GUI_DisplayText(String_Get_ByIndex(STR_INSUFFICIENT_FUNDS_CONSTRUCTION_IS_HALTED), 0);
						}
					}
				}

				if (s->o.type == STRUCTURE_REPAIR) {
					if (!s->o.flags.s.onHold && s->countDown != 0 && s->o.linkedID != 0xFF) {
						const UnitInfo *ui;
						uint16 repairSpeed;
						uint16 repairCost;

						ui = &g_table_unitInfo[Unit_Get_ByIndex(s->o.linkedID)->o.type];

						repairSpeed = 256;
						if (s->o.hitpoints < si->o.hitpoints) {
							repairSpeed = s->o.hitpoints * 256 / si->o.hitpoints;
						}

						/* XXX -- This is highly unfair. Repairing becomes more expensive if your structure is more damaged */
						repairCost = 2 * ui->o.buildCredits / 256;

						if (repairCost < h->credits) {
							h->credits -= repairCost;

							if (repairSpeed < s->countDown) {
								s->countDown -= repairSpeed;
							} else {
								s->countDown = 0;

								Structure_SetState(s, STRUCTURE_STATE_READY);

								if (s->o.houseID == g_playerHouseID) Sound_Output_Feedback(g_playerHouseID + 55);
							}
						}
					} else if (h->credits != 0) {
						/* Automaticly resume repairing when there is money again */
						s->o.flags.s.onHold = false;
					}
				}

				/* AI maintenance on structures */
				if (h->flags.isAIActive && s->o.flags.s.allocated && !Match_IsHumanControlled(s->o.houseID) && h->credits != 0) {
					/* When structure is below 50% hitpoints, start repairing */
					if (s->o.hitpoints < si->o.hitpoints / 2) {
						Structure_SetRepairingState(s, 1, NULL);
					}

					/* If the structure is not doing something, but can build stuff, see if there is stuff to build */
					if (si->o.flags.factory && s->countDown == 0 && s->o.linkedID == 0xFF) {
						uint16 type = Structure_AI_PickNextToBuild(s);

						if (type != 0xFFFF) Structure_BuildObject(s, type);
					}
				}
			}
		}

		if (tickScript) {
			if (s->o.script.delay != 0) {
				s->o.script.delay--;
			} else {
				if (Script_IsLoaded(&s->o.script)) {
					uint8 i;

					/* Run the script 3 times in a row */
					for (i = 0; i < 3; i++) {
						if (!Script_Run(&s->o.script)) break;
					}

					/* ENHANCEMENT -- Dune2 aborts all other structures if one gives a script error. This doesn't seem correct */
					if (!g_dune2_enhanced && i != 3) return;
				} else {
					Script_Reset(&s->o.script, s->o.script.scriptInfo);
					Script_Load(&s->o.script, s->o.type);
				}
			}
		}
	}
}

/**
 * Convert the name of a structure to the type value of that structure, or
 *  STRUCTURE_INVALID if not found.
 */
uint8 Structure_StringToType(const char *name)
{
	uint8 type;
	if (name == NULL) return STRUCTURE_INVALID;

	for (type = 0; type < STRUCTURE_MAX; type++) {
		if (strcasecmp(g_table_structureInfo[type].o.name, name) == 0) return type;
	}

	return STRUCTURE_INVALID;
}

/**
 * Create a new Structure.
 *
 * @param index The new index of the Structure, or STRUCTURE_INDEX_INVALID to assign one.
 * @param typeID The type of the new Structure.
 * @param houseID The House of the new Structure.
 * @param position The packed position where to place the Structure. If 0xFFFF, the Structure is not placed.
 * @return The new created Structure, or NULL if something failed.
 */
Structure *Structure_Create(uint16 index, uint8 typeID, uint8 houseID, uint16 position)
{
	const StructureInfo *si;
	Structure *s;

	if (houseID >= HOUSE_MAX) return NULL;
	if (typeID >= STRUCTURE_MAX) return NULL;

	si = &g_table_structureInfo[typeID];
	s = Structure_Allocate(index, typeID);
	if (s == NULL) return NULL;
	Structure_Queue_Reset(s);

	s->o.houseID            = houseID;
	s->creatorHouseID       = houseID;
	s->o.flags.s.isNotOnMap = true;
	s->o.position.x         = 0;
	s->o.position.y         = 0;
	s->o.linkedID           = 0xFF;
	s->state                = (g_debugScenario) ? STRUCTURE_STATE_IDLE : STRUCTURE_STATE_JUSTBUILT;

	if (typeID == STRUCTURE_TURRET) {
		s->rotationSpriteDiff = g_iconMap[g_iconMap[ICM_ICONGROUP_BASE_DEFENSE_TURRET] + 1];
	}
	if (typeID == STRUCTURE_ROCKET_TURRET) {
		s->rotationSpriteDiff = g_iconMap[g_iconMap[ICM_ICONGROUP_BASE_ROCKET_TURRET] + 1];
	}

	s->o.hitpoints  = si->o.hitpoints;
	s->hitpointsMax = si->o.hitpoints;

	if (houseID == HOUSE_HARKONNEN && typeID == STRUCTURE_LIGHT_VEHICLE) {
		s->upgradeLevel = 1;
	}

	/* Check if there is an upgrade available */
	if (si->o.flags.factory) {
		s->upgradeTimeLeft = Structure_IsUpgradable(s) ? 100 : 0;
	}

	s->objectType = 0xFFFF;

	Structure_BuildObject(s, 0xFFFE);

	s->countDown = 0;

	/* AIs get the full upgrade immediately */
	if (!Match_IsHumanControlled(houseID)) {
		while (true) {
			if (!Structure_IsUpgradable(s)) break;
			s->upgradeLevel++;
		}
		s->upgradeTimeLeft = 0;
	}

	if (position != 0xFFFF && !Structure_Place(s, position)) {
		Structure_Free(s);
		return NULL;
	}

	return s;
}

/**
 * Place a structure on the map.
 *
 * @param structure The structure to place on the map.
 * @param position The (packed) tile to place the struction on.
 * @return True if and only if the structure is placed on the map.
 */
bool Structure_Place(Structure *s, uint16 position)
{
	const StructureInfo *si;
	int16 validBuildLocation;

	if (s == NULL) return false;
	if (position == 0xFFFF) return false;

	si = &g_table_structureInfo[s->o.type];

	switch (s->o.type) {
		case STRUCTURE_WALL: {
			Tile *t;

			/* The same exemption the general path at the end of this function
			 * gets, and for the same reason.  Structure_IsValidBuildLocation()
			 * ends in a "must touch something of your own" test written against
			 * g_playerHouseID, so no AI house can ever satisfy it -- and in a
			 * skirmish the player owns nothing at all.  Walls were the only
			 * structure whose placement enforced it regardless of house, so every
			 * AI wall was silently refunded and freed while its plan entry was
			 * spent: an AI base never had a single one. */
			if (Structure_IsValidBuildLocation(position, STRUCTURE_WALL, s->o.houseID) == 0 &&
			    Match_IsHumanControlled(s->o.houseID) && !g_debugScenario && g_validateStrictIfZero == 0) return false;

			t = &g_map[position];
			t->groundTileID = g_wallTileID + 1;
			/* ENHANCEMENT -- Dune2 wrongfully only removes the lower 2 bits, where the lower 3 bits are the owner. This is no longer visible. */
			t->houseID  = s->o.houseID;

			g_mapTileID[position] |= 0x8000;

			if ((Match_IsActive() || s->o.houseID == g_playerHouseID)) Tile_RemoveFogInRadius(Tile_UnpackTile(position), 1);

			if (Map_IsPositionUnveiled(position)) t->overlayTileID = 0;

			Structure_ConnectWall(position, true);
			Structure_Free(s);

		} return true;

		case STRUCTURE_SLAB_1x1:
		case STRUCTURE_SLAB_2x2: {
			uint16 i, result;

			result = 0;

			for (i = 0; i < g_table_structure_layoutTileCount[si->layout]; i++) {
				uint16 curPos = position + g_table_structure_layoutTiles[si->layout][i];
				Tile *t = &g_map[curPos];

				if (Structure_IsValidBuildLocation(curPos, STRUCTURE_SLAB_1x1, s->o.houseID) == 0) continue;

				t->groundTileID = g_builtSlabTileID;
				t->houseID = s->o.houseID;

				g_mapTileID[curPos] |= 0x8000;

				if ((Match_IsActive() || s->o.houseID == g_playerHouseID)) Tile_RemoveFogInRadius(Tile_UnpackTile(curPos), 1);

				if (Map_IsPositionUnveiled(curPos)) t->overlayTileID = 0;

				Map_Update(curPos, 0, false);

				result = 1;
			}

			/* XXX -- Dirt hack -- Parts of the 2x2 slab can be outside the building area, so by doing the same loop twice it will build for sure */
			if (s->o.type == STRUCTURE_SLAB_2x2) {
				for (i = 0; i < g_table_structure_layoutTileCount[si->layout]; i++) {
					uint16 curPos = position + g_table_structure_layoutTiles[si->layout][i];
					Tile *t = &g_map[curPos];

					if (Structure_IsValidBuildLocation(curPos, STRUCTURE_SLAB_1x1, s->o.houseID) == 0) continue;

					t->groundTileID = g_builtSlabTileID;
					t->houseID = s->o.houseID;

					g_mapTileID[curPos] |= 0x8000;

					if (Match_IsActive() || s->o.houseID == g_playerHouseID) {
						Tile_RemoveFogInRadius(Tile_UnpackTile(curPos), 1);
						t->overlayTileID = 0;
					}

					Map_Update(curPos, 0, false);

					result = 1;
				}
			}

			if (result == 0) return false;

			Structure_Free(s);
		} return true;
	}

	validBuildLocation = Structure_IsValidBuildLocation(position, s->o.type, s->o.houseID);
	if (validBuildLocation == 0 && Match_IsHumanControlled(s->o.houseID) && !g_debugScenario && g_validateStrictIfZero == 0) return false;

	/* ENHANCEMENT -- In Dune2, it only removes the fog around the top-left tile of a structure, leaving for big structures the right in the fog. */
	if (!g_dune2_enhanced && (Match_IsActive() || s->o.houseID == g_playerHouseID)) Tile_RemoveFogInRadius(Tile_UnpackTile(position), 2);

	s->o.seenByHouses |= 1 << s->o.houseID;
	/* The player's base is visible to every House from the moment it is
	 * built, which is what gives the campaign AI something to attack.  A
	 * skirmish has no player base, so both AI bases take that role -- without
	 * it Unit_GetTargetStructurePriority() returns 0 and no team ever moves. */
	if (s->o.houseID == g_playerHouseID || Skirmish_IsActive()) s->o.seenByHouses |= 0xFF;

	s->o.flags.s.isNotOnMap = false;

	s->o.position = Tile_UnpackTile(position);
	s->o.position.x &= 0xFF00;
	s->o.position.y &= 0xFF00;

	s->rotationSpriteDiff = 0;
	s->o.hitpoints  = si->o.hitpoints;
	s->hitpointsMax = si->o.hitpoints;

	/* An AI house never carried this penalty.  The adjacency test it is bundled
	 * with used to fail for every AI, which returned 0 and skipped the whole
	 * branch; making that test answer honestly would hand every AI rebuild a
	 * hitpoint cut and the degrades flag, which is a balance change nobody asked
	 * for.  The foundation rule stays the player's. */
	if (validBuildLocation < 0 && !Match_IsHumanControlled(s->o.houseID)) validBuildLocation = 0;

	/* If the return value is negative, there are tiles without slab. This gives a penalty to the hitpoints. */
	if (validBuildLocation < 0) {
		uint16 tilesWithoutSlab = -(int16)validBuildLocation;
		uint16 structureTileCount = g_table_structure_layoutTileCount[si->layout];

		s->o.hitpoints -= (si->o.hitpoints / 2) * tilesWithoutSlab / structureTileCount;

		s->o.flags.s.degrades = true;
	} else {
		/* ENHANCEMENT -- When you build a structure completely on slabs, it should not degrade */
		if (!g_dune2_enhanced) {
			s->o.flags.s.degrades = true;
		}
	}

	Script_Reset(&s->o.script, g_scriptStructure);

	s->o.script.variables[0] = 0;
	s->o.script.variables[4] = 0;

	/* XXX -- Weird .. if 'position' enters with 0xFFFF it is returned immediately .. how can this ever NOT happen? */
	if (position != 0xFFFF) {
		s->o.script.delay = 0;
		Script_Reset(&s->o.script, s->o.script.scriptInfo);
		Script_Load(&s->o.script, s->o.type);
	}

	{
		uint16 i;

		for (i = 0; i < g_table_structure_layoutTileCount[si->layout]; i++) {
			uint16 curPos = position + g_table_structure_layoutTiles[si->layout][i];
			Unit *u;

			u = Unit_Get_ByPackedTile(curPos);

			Unit_Remove(u);

			/* ENHANCEMENT -- In Dune2, it only removes the fog around the top-left tile of a structure, leaving for big structures the right in the fog. */
			if (g_dune2_enhanced && (Match_IsActive() || s->o.houseID == g_playerHouseID)) Tile_RemoveFogInRadius(Tile_UnpackTile(curPos), 2);

		}
	}

	if (s->o.type == STRUCTURE_WINDTRAP) {
		House *h;

		h = House_Get_ByIndex(s->o.houseID);
		h->windtrapCount += 1;
	}

	if (g_validateStrictIfZero == 0) {
		House *h;

		h = House_Get_ByIndex(s->o.houseID);
		House_CalculatePowerAndCredit(h);
	}

	Structure_UpdateMap(s);

	{
		House *h;
		h = House_Get_ByIndex(s->o.houseID);
		h->structuresBuilt = Structure_GetStructuresBuilt(h);
	}

	return true;
}

/**
 * Calculate the power usage and production, and the credits storage.
 *
 * @param h The house to calculate the numbers for.
 */
void Structure_CalculateHitpointsMax(House *h)
{
	PoolFindStruct find;
	uint16 power = 0;

	if (h == NULL) return;

	House_UpdateRadarState(h);

	if (h->powerUsage == 0) {
		power = 256;
	} else {
		power = min(h->powerProduction * 256 / h->powerUsage, 256);
	}

	find.houseID = h->index;
	find.index   = 0xFFFF;
	find.type    = 0xFFFF;

	while (true) {
		const StructureInfo *si;
		Structure *s;

		s = Structure_Find(&find);
		if (s == NULL) return;
		if (s->o.type == STRUCTURE_SLAB_1x1 || s->o.type == STRUCTURE_SLAB_2x2 || s->o.type == STRUCTURE_WALL) continue;

		si = &g_table_structureInfo[s->o.type];

		s->hitpointsMax = si->o.hitpoints * power / 256;
		s->hitpointsMax = max(s->hitpointsMax, si->o.hitpoints / 2);

		if (s->hitpointsMax >= s->o.hitpoints) continue;
		Structure_Damage(s, 1, 0);
	}
}

/**
 * Set the state for the given structure.
 *
 * @param s The structure to set the state of.
 * @param state The new sate value.
 */
void Structure_SetState(Structure *s, int16 state)
{
	if (s == NULL) return;
	s->state = state;

	Structure_UpdateMap(s);
}

/**
 * Get the structure on the given packed tile.
 *
 * @param packed The packed tile to get the structure from.
 * @return The structure.
 */
Structure *Structure_Get_ByPackedTile(uint16 packed)
{
	Tile *tile;

	if (Tile_IsOutOfMap(packed)) return NULL;

	tile = &g_map[packed];
	if (!tile->hasStructure) return NULL;
	return Structure_Get_ByIndex(tile->index - 1);
}

/**
 * Get a bitmask of all built structure types for the given House.
 *
 * @param h The house to get built structures for.
 * @return The bitmask.
 */
uint32 Structure_GetStructuresBuilt(House *h)
{
	PoolFindStruct find;
	uint32 result;

	if (h == NULL) return 0;

	result = 0;
	find.houseID = h->index;
	find.index   = 0xFFFF;
	find.type    = 0xFFFF;

	/* Recount windtraps after capture or loading old saved games. */
	h->windtrapCount = 0;

	while (true) {
		Structure *s;

		s = Structure_Find(&find);
		if (s == NULL) break;
		if (s->o.flags.s.isNotOnMap) continue;
		if (s->o.type == STRUCTURE_SLAB_1x1 || s->o.type == STRUCTURE_SLAB_2x2 || s->o.type == STRUCTURE_WALL) continue;
		result |= 1 << s->o.type;

		if (s->o.type == STRUCTURE_WINDTRAP) h->windtrapCount++;
	}

	return result;
}

/**
 * Whether concrete may be poured onto sand.
 *
 * Off is the original game: `notOnConcrete` sends the slab through
 * `isValidForStructure2`, which is true for rock and nothing else, so a base can
 * only ever grow over the rock the map happens to have put there.
 *
 * On, a slab is also a road.  The landscape table already gives
 * LST_CONCRETE_SLAB a movement speed of 255 for every movement type against
 * sand's 112 -- the fastest surface in the game, with nowhere to use it -- so
 * paving costs credits and buys both a foundation and a road, and the
 * pathfinder prefers it without being told to.
 */
static bool s_slabOnSand = true;

void Structure_BuildRules_Init(void)
{
	s_slabOnSand = (IniFile_GetInteger("build_slab_on_sand", 1) != 0);
}

/** Both settings of the rule, for the self-test that has to see it refuse. */
void Structure_BuildRules_SetSlabOnSand(bool allowed)
{
	s_slabOnSand = allowed;
}

/** Whether concrete may be poured on sand.  Folded into the lobby digest. */
bool Structure_BuildRules_SlabOnSand(void)
{
	return s_slabOnSand;
}

/**
 * The tech tree -- what has to stand before a building may be built.
 *
 * Westwood's answer is the `structuresRequired` mask in g_table_structureInfo,
 * one line per building, and in a campaign it is only half the gate: the
 * mission number decides the rest through `availableCampaign`.  A match has no
 * mission number -- Skirmish_Prepare() sets g_campaignID to 8, past the last
 * building on the list -- so in multiplayer the mask *is* the tech tree, and
 * changing it is the only way to change the shape of an opening.
 *
 * This module makes it configuration rather than a table.  It patches
 * g_table_structureInfo the way Unit_CombatBalance_Init() patches the unit
 * table, and for the same reason: the lobby folds both tables into the room
 * name (Lobby_FoldStructure() hashes structuresRequired, upgradeLevelRequired
 * and upgradeCampaign), so two players whose trees differ ask the relay for
 * different rooms and never meet.  A disagreement about the tech tree is
 * therefore "the other player never joined" rather than a desync the moment
 * somebody builds.  That is why this runs at start-up and not at match start:
 * the digest is taken in the menu, before either of them has clicked BEGIN.
 *
 * `tech_tree=stock` is the compiled-in tree and the default.  `tech_tree=mp`
 * is the tree this fork plays: every branch hangs off the Refinery instead of
 * the Outpost, the Outpost carries the three high-tech buildings, and the
 * Rocket Turret is bought with the House of IX rather than with a Construction
 * Yard upgrade.  Either can be edited key by key with `tech_req_<building>`.
 */

typedef struct TechTreeEdge {
	uint8  type;                                            /*!< Which building this line is about. */
	uint32 required;                                        /*!< What has to stand before it. */
	uint16 upgradeLevelRequired;                            /*!< Construction Yard upgrade level it also needs. */
} TechTreeEdge;

/**
 * The multiplayer tree.
 *
 * Read it as four branches off one trunk.  The Refinery is the trunk -- nothing
 * but the Windtrap comes before it, so the first decision of a match is always
 * the same one and the openings diverge after it.  Off the Refinery hang the
 * cheap branches a player needs early: infantry, light vehicles, the defence
 * line and the Outpost.  Off the Light Factory hang the two buildings that are
 * about vehicles, and off the Outpost the three that are about technology.  The
 * Palace is the only building that asks for a whole tier: Angar, IX and
 * Starport together, which is most of a finished base.
 *
 * The Rocket Turret moves the furthest.  In the original it is an Outpost
 * building behind two Construction Yard upgrades, which makes the best defence
 * in the game an early purchase; here it is behind the House of IX, so a player
 * who wants it pays for the Outpost, the Starport-free IX branch and 500
 * credits of building first, and an army that arrives before that meets Turrets.
 *
 * WOR keeps the Barracks *and* the Refinery in its mask although the Barracks
 * alone would imply the Refinery.  Structure_GetBuildable() waives the Barracks
 * bit for Harkonnen -- their identity, and the one thing the tree may not
 * flatten -- and without the Refinery beside it that waiver would leave WOR
 * with no prerequisite at all, i.e. rocket infantry on the first tick.
 */
static const TechTreeEdge s_techTreeMultiplayer[] = {
	{ STRUCTURE_WINDTRAP,      FLAG_STRUCTURE_NONE,                                                        0 },
	{ STRUCTURE_REFINERY,      FLAG_STRUCTURE_WINDTRAP,                                                    0 },
	{ STRUCTURE_BARRACKS,      FLAG_STRUCTURE_REFINERY,                                                    0 },
	{ STRUCTURE_LIGHT_VEHICLE, FLAG_STRUCTURE_REFINERY,                                                    0 },
	{ STRUCTURE_TURRET,        FLAG_STRUCTURE_REFINERY,                                                    0 },
	{ STRUCTURE_WALL,          FLAG_STRUCTURE_REFINERY,                                                    0 },
	{ STRUCTURE_SILO,          FLAG_STRUCTURE_REFINERY,                                                    0 },
	{ STRUCTURE_OUTPOST,       FLAG_STRUCTURE_REFINERY,                                                    0 },
	{ STRUCTURE_WOR_TROOPER,   FLAG_STRUCTURE_BARRACKS | FLAG_STRUCTURE_REFINERY,                          0 },
	{ STRUCTURE_HEAVY_VEHICLE, FLAG_STRUCTURE_LIGHT_VEHICLE,                                               0 },
	{ STRUCTURE_REPAIR,        FLAG_STRUCTURE_LIGHT_VEHICLE,                                               0 },
	{ STRUCTURE_HIGH_TECH,     FLAG_STRUCTURE_OUTPOST,                                                     0 },
	{ STRUCTURE_HOUSE_OF_IX,   FLAG_STRUCTURE_OUTPOST,                                                     0 },
	{ STRUCTURE_STARPORT,      FLAG_STRUCTURE_OUTPOST,                                                     0 },
	{ STRUCTURE_ROCKET_TURRET, FLAG_STRUCTURE_HOUSE_OF_IX,                                                 0 },
	{ STRUCTURE_PALACE,        FLAG_STRUCTURE_HIGH_TECH | FLAG_STRUCTURE_HOUSE_OF_IX | FLAG_STRUCTURE_STARPORT, 0 }
};

/** The compiled-in tree, taken once before anything patches it. */
static struct {
	uint32 required;
	uint16 upgradeLevelRequired;
	uint16 upgradeCampaign[3];
} s_techTreeStock[STRUCTURE_MAX];
static bool s_techTreeStockTaken = false;

/** Which tree is standing, for the self-test and for the log line. */
static char s_techTreeName[16] = "stock";

/** --tech-tree=NAME, which beats the ini key.  See Structure_TechTree_SetTree(). */
static char s_techTreeOverride[16] = "";

static void Structure_TechTree_TakeStock(void)
{
	uint16 i;
	uint16 j;

	if (s_techTreeStockTaken) return;

	for (i = 0; i < STRUCTURE_MAX; i++) {
		s_techTreeStock[i].required             = g_table_structureInfo[i].o.structuresRequired;
		s_techTreeStock[i].upgradeLevelRequired = g_table_structureInfo[i].o.upgradeLevelRequired;
		for (j = 0; j < 3; j++) s_techTreeStock[i].upgradeCampaign[j] = g_table_structureInfo[i].upgradeCampaign[j];
	}

	s_techTreeStockTaken = true;
}

static void Structure_TechTree_Restore(void)
{
	uint16 i;
	uint16 j;

	if (!s_techTreeStockTaken) return;

	for (i = 0; i < STRUCTURE_MAX; i++) {
		g_table_structureInfo[i].o.structuresRequired   = s_techTreeStock[i].required;
		g_table_structureInfo[i].o.upgradeLevelRequired = s_techTreeStock[i].upgradeLevelRequired;
		for (j = 0; j < 3; j++) g_table_structureInfo[i].upgradeCampaign[j] = s_techTreeStock[i].upgradeCampaign[j];
	}
}

/** The ini key for a building: its table name, lowercased, non-letters collapsed. */
static void Structure_TechTree_KeySlug(const char *name, char *dest, uint16 destLen)
{
	uint16 out = 0;
	uint16 i;

	for (i = 0; name[i] != '\0' && out + 1 < destLen; i++) {
		char c = name[i];

		if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
		if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
			dest[out++] = c;
		} else if (out != 0 && dest[out - 1] != '_') {
			dest[out++] = '_';
		}
	}
	while (out != 0 && dest[out - 1] == '_') out--;
	dest[out] = '\0';
}

/**
 * A building named in a config line.
 *
 * The table names are what the keys are built from, but half of them are the
 * abbreviations that fitted the original's build panel -- "Light Fctry",
 * "Hi-Tech", "R-Turret".  The aliases let a config say what a person would say
 * without introducing a second naming scheme: both spellings answer.
 */
static uint8 Structure_TechTree_TypeByName(const char *token)
{
	static const struct {
		const char *name;
		uint8 type;
	} aliases[] = {
		{ "construction_yard", STRUCTURE_CONSTRUCTION_YARD },
		{ "light_factory",     STRUCTURE_LIGHT_VEHICLE },
		{ "heavy_factory",     STRUCTURE_HEAVY_VEHICLE },
		{ "high_tech",         STRUCTURE_HIGH_TECH },
		{ "hitech",            STRUCTURE_HIGH_TECH },
		{ "house_of_ix",       STRUCTURE_HOUSE_OF_IX },
		{ "rocket_turret",     STRUCTURE_ROCKET_TURRET },
		{ "silo",              STRUCTURE_SILO },
		{ "radar",             STRUCTURE_OUTPOST },
		{ "slab",              STRUCTURE_SLAB_1x1 },
		{ "slab4",             STRUCTURE_SLAB_2x2 }
	};
	char slug[32];
	uint16 i;

	for (i = 0; i < STRUCTURE_MAX; i++) {
		Structure_TechTree_KeySlug(g_table_structureInfo[i].o.name, slug, sizeof(slug));
		if (slug[0] != '\0' && strcasecmp(slug, token) == 0) return (uint8)i;
	}

	for (i = 0; i < lengthof(aliases); i++) {
		if (strcasecmp(aliases[i].name, token) == 0) return aliases[i].type;
	}

	return STRUCTURE_INVALID;
}

/** "refinery, outpost" -> the mask.  "none" is no prerequisite, "never" is unbuildable. */
static bool Structure_TechTree_ParseList(const char *list, uint32 *mask)
{
	char token[32];
	uint16 len = 0;
	uint16 i;

	*mask = FLAG_STRUCTURE_NONE;

	for (i = 0; ; i++) {
		char c = list[i];

		if (c != '\0' && c != ',' && c != '+' && c != ' ' && c != '\t') {
			if (len + 1 < sizeof(token)) token[len++] = c;
			continue;
		}

		if (len != 0) {
			token[len] = '\0';
			len = 0;

			if (strcasecmp(token, "none") == 0) {
				/* Nothing: an empty mask is the answer. */
			} else if (strcasecmp(token, "never") == 0) {
				*mask = (uint32)FLAG_STRUCTURE_NEVER;
			} else {
				uint8 type = Structure_TechTree_TypeByName(token);

				if (type == STRUCTURE_INVALID) return false;
				*mask |= (uint32)1 << type;
			}
		}

		if (c == '\0') break;
	}

	return true;
}

/**
 * Whether the standing tree can actually be played.
 *
 * Two ways a hand-written tree kills a match, and neither of them shows up
 * until somebody sits in front of a build list that is missing a row: a cycle
 * (A wants B, B wants A) and a building whose prerequisites can never all be
 * met.  Both are the same question -- grow the set of buildings reachable from
 * a Construction Yard until it stops growing, and see whether anything is left
 * outside it.
 *
 * The third way is the upgrade gate: a building may ask for a Construction Yard
 * upgrade level the Construction Yard does not offer, which is a row that is
 * drawn and never lights up.
 */
static bool Structure_TechTree_Validate(void)
{
	uint32 reachable = FLAG_STRUCTURE_CONSTRUCTION_YARD;
	uint16 upgradeLevels = 0;
	bool grew = true;
	uint16 i;

	for (i = 0; i < 3; i++) {
		if (g_table_structureInfo[STRUCTURE_CONSTRUCTION_YARD].upgradeCampaign[i] != 0) upgradeLevels++;
	}

	while (grew) {
		grew = false;

		for (i = 0; i < STRUCTURE_MAX; i++) {
			uint32 bit = (uint32)1 << i;
			uint32 required = g_table_structureInfo[i].o.structuresRequired;

			if ((reachable & bit) != 0) continue;
			if (required == (uint32)FLAG_STRUCTURE_NEVER) continue;
			if ((required & ~reachable) != 0) continue;

			reachable |= bit;
			grew = true;
		}
	}

	for (i = 0; i < STRUCTURE_MAX; i++) {
		if (g_table_structureInfo[i].o.structuresRequired == (uint32)FLAG_STRUCTURE_NEVER) continue;
		if ((reachable & ((uint32)1 << i)) == 0) return false;
		if (g_table_structureInfo[i].o.upgradeLevelRequired > upgradeLevels) return false;
	}

	return true;
}

static void Structure_TechTree_ApplyTree(const char *name)
{
	uint16 i;

	Structure_TechTree_TakeStock();
	Structure_TechTree_Restore();

	if (strcasecmp(name, "mp") == 0 || strcasecmp(name, "multiplayer") == 0) {
		for (i = 0; i < lengthof(s_techTreeMultiplayer); i++) {
			StructureInfo *si = &g_table_structureInfo[s_techTreeMultiplayer[i].type];

			si->o.structuresRequired   = s_techTreeMultiplayer[i].required;
			si->o.upgradeLevelRequired = s_techTreeMultiplayer[i].upgradeLevelRequired;
		}

		/* The Construction Yard's second upgrade only ever unlocked the Rocket
		 * Turret, and the Rocket Turret is bought with the House of IX now.  Left
		 * standing it would be 200 credits and twenty ticks for nothing, offered
		 * by a button that says nothing about what it buys. */
		g_table_structureInfo[STRUCTURE_CONSTRUCTION_YARD].upgradeCampaign[1] = 0;
		g_table_structureInfo[STRUCTURE_CONSTRUCTION_YARD].upgradeCampaign[2] = 0;
	}

	strncpy(s_techTreeName, name, sizeof(s_techTreeName) - 1);
	s_techTreeName[sizeof(s_techTreeName) - 1] = '\0';
}

/** --tech-tree=NAME.  The ini is shadowed by the player's own copy; a flag is not. */
void Structure_TechTree_SetTree(const char *name)
{
	strncpy(s_techTreeOverride, name, sizeof(s_techTreeOverride) - 1);
	s_techTreeOverride[sizeof(s_techTreeOverride) - 1] = '\0';
}

const char *Structure_TechTree_GetTree(void)
{
	return s_techTreeName;
}

void Structure_TechTree_Init(void)
{
	char name[16];
	char value[160];
	uint16 i;

	Structure_TechTree_TakeStock();

	if (s_techTreeOverride[0] != '\0') {
		strncpy(name, s_techTreeOverride, sizeof(name) - 1);
		name[sizeof(name) - 1] = '\0';
	} else {
		IniFile_GetString("tech_tree", "stock", name, sizeof(name));
	}

	if (strcasecmp(name, "stock") != 0 && strcasecmp(name, "mp") != 0 && strcasecmp(name, "multiplayer") != 0) {
		Warning("tech_tree: no tree called \"%s\"; stock and mp are the two there are\n", name);
		strcpy(name, "stock");
	}

	Structure_TechTree_ApplyTree(name);

	/* Key by key on top of the named tree, so a tree can be edited without
	 * being retyped.  Every building is reachable, including the ones no tree
	 * moves. */
	for (i = 0; i < STRUCTURE_MAX; i++) {
		StructureInfo *si = &g_table_structureInfo[i];
		char slug[32];
		char key[64];
		int levels;

		Structure_TechTree_KeySlug(si->o.name, slug, sizeof(slug));
		if (slug[0] == '\0') continue;

		sprintf(key, "tech_req_%s", slug);
		IniFile_GetString(key, "", value, sizeof(value));
		if (value[0] != '\0') {
			uint32 mask;

			if (Structure_TechTree_ParseList(value, &mask)) {
				si->o.structuresRequired = mask;
			} else {
				Warning("%s=\"%s\": that is not a list of buildings\n", key, value);
			}
		}

		sprintf(key, "tech_upgrade_%s", slug);
		si->o.upgradeLevelRequired = (uint16)min(max(IniFile_GetInteger(key, si->o.upgradeLevelRequired), 0), 3);

		/* Levels can be taken away but not invented: the campaign number that
		 * gates each one is the table's business, so this only ever zeroes. */
		sprintf(key, "tech_upgrade_levels_%s", slug);
		levels = IniFile_GetInteger(key, -1);
		if (levels >= 0) {
			int j;

			for (j = max(levels, 0); j < 3; j++) si->upgradeCampaign[j] = 0;
		}
	}

	if (!Structure_TechTree_Validate()) {
		Warning("tech_tree: the configured tree has a building nothing can reach; keeping stock\n");
		Structure_TechTree_ApplyTree("stock");
	}
}

/**
 * The guard.  It checks the rule, not the drawing.
 *
 * The last section is the one that matters: it asks the real
 * Structure_GetBuildable() what a real Construction Yard offers, growing the
 * house's built mask one building at a time.  A tree that only reads right in
 * the table is not a tree anybody can play -- the campaign gate, the House
 * filter and the upgrade level all sit between the mask and the build list, and
 * the Rocket Turret in particular is only where this config says it is if it
 * lights up at Construction Yard level 0.
 */
int Structure_TechTree_RunRegressionTest(void)
{
	static const struct {
		uint32 built;                                       /*!< What stands, on top of the Construction Yard. */
		uint32 expected;                                    /*!< What has to be offered on top of what came before. */
		uint32 forbidden;                                   /*!< What may not be offered yet. */
	} steps[] = {
		{ 0,
		  FLAG_STRUCTURE_WINDTRAP | FLAG_STRUCTURE_SLAB_1x1,
		  FLAG_STRUCTURE_REFINERY | FLAG_STRUCTURE_BARRACKS | FLAG_STRUCTURE_OUTPOST | FLAG_STRUCTURE_ROCKET_TURRET | FLAG_STRUCTURE_SLAB_2x2 },
		{ FLAG_STRUCTURE_WINDTRAP,
		  FLAG_STRUCTURE_REFINERY,
		  FLAG_STRUCTURE_OUTPOST | FLAG_STRUCTURE_BARRACKS | FLAG_STRUCTURE_LIGHT_VEHICLE | FLAG_STRUCTURE_TURRET },
		{ FLAG_STRUCTURE_WINDTRAP | FLAG_STRUCTURE_REFINERY,
		  FLAG_STRUCTURE_BARRACKS | FLAG_STRUCTURE_LIGHT_VEHICLE | FLAG_STRUCTURE_TURRET | FLAG_STRUCTURE_WALL | FLAG_STRUCTURE_SILO | FLAG_STRUCTURE_OUTPOST,
		  FLAG_STRUCTURE_HEAVY_VEHICLE | FLAG_STRUCTURE_REPAIR | FLAG_STRUCTURE_HIGH_TECH | FLAG_STRUCTURE_STARPORT | FLAG_STRUCTURE_HOUSE_OF_IX | FLAG_STRUCTURE_WOR_TROOPER | FLAG_STRUCTURE_ROCKET_TURRET | FLAG_STRUCTURE_PALACE },
		{ FLAG_STRUCTURE_WINDTRAP | FLAG_STRUCTURE_REFINERY | FLAG_STRUCTURE_BARRACKS,
		  FLAG_STRUCTURE_WOR_TROOPER,
		  FLAG_STRUCTURE_HIGH_TECH | FLAG_STRUCTURE_ROCKET_TURRET },
		{ FLAG_STRUCTURE_WINDTRAP | FLAG_STRUCTURE_REFINERY | FLAG_STRUCTURE_LIGHT_VEHICLE,
		  FLAG_STRUCTURE_HEAVY_VEHICLE | FLAG_STRUCTURE_REPAIR,
		  FLAG_STRUCTURE_HIGH_TECH | FLAG_STRUCTURE_HOUSE_OF_IX | FLAG_STRUCTURE_STARPORT | FLAG_STRUCTURE_ROCKET_TURRET },
		{ FLAG_STRUCTURE_WINDTRAP | FLAG_STRUCTURE_REFINERY | FLAG_STRUCTURE_OUTPOST,
		  FLAG_STRUCTURE_HIGH_TECH | FLAG_STRUCTURE_HOUSE_OF_IX | FLAG_STRUCTURE_STARPORT,
		  FLAG_STRUCTURE_ROCKET_TURRET | FLAG_STRUCTURE_PALACE },
		{ FLAG_STRUCTURE_WINDTRAP | FLAG_STRUCTURE_REFINERY | FLAG_STRUCTURE_OUTPOST | FLAG_STRUCTURE_HOUSE_OF_IX,
		  FLAG_STRUCTURE_ROCKET_TURRET,
		  FLAG_STRUCTURE_PALACE },
		{ FLAG_STRUCTURE_WINDTRAP | FLAG_STRUCTURE_REFINERY | FLAG_STRUCTURE_OUTPOST | FLAG_STRUCTURE_HOUSE_OF_IX | FLAG_STRUCTURE_HIGH_TECH | FLAG_STRUCTURE_STARPORT,
		  FLAG_STRUCTURE_PALACE,
		  FLAG_STRUCTURE_SLAB_2x2 }
	};
	char configured[16];
	uint16 campaignID = g_campaignID;
	int result = 1;
	uint16 i;

	strcpy(configured, s_techTreeName);

	/* The stock tree has to be exactly the table, or the module is quietly
	 * playing a game of its own on every machine that never set a key. */
	Structure_TechTree_ApplyTree("stock");
	for (i = 0; i < STRUCTURE_MAX; i++) {
		uint16 j;

		if (g_table_structureInfo[i].o.structuresRequired != s_techTreeStock[i].required) result = 0;
		if (g_table_structureInfo[i].o.upgradeLevelRequired != s_techTreeStock[i].upgradeLevelRequired) result = 0;
		for (j = 0; j < 3; j++) {
			if (g_table_structureInfo[i].upgradeCampaign[j] != s_techTreeStock[i].upgradeCampaign[j]) result = 0;
		}
	}
	if (!Structure_TechTree_Validate()) result = 0;

	Structure_TechTree_ApplyTree("mp");
	if (!Structure_TechTree_Validate()) result = 0;

	for (i = 0; i < lengthof(s_techTreeMultiplayer); i++) {
		const StructureInfo *si = &g_table_structureInfo[s_techTreeMultiplayer[i].type];

		if (si->o.structuresRequired != s_techTreeMultiplayer[i].required) result = 0;
		if (si->o.upgradeLevelRequired != s_techTreeMultiplayer[i].upgradeLevelRequired) result = 0;
	}

	/* The two halves of moving the Rocket Turret: it hangs off the House of IX,
	 * and it costs no Construction Yard upgrade -- which leaves the Yard with
	 * exactly one upgrade, the one the large slab needs. */
	if (g_table_structureInfo[STRUCTURE_ROCKET_TURRET].o.upgradeLevelRequired != 0) result = 0;
	if (g_table_structureInfo[STRUCTURE_CONSTRUCTION_YARD].upgradeCampaign[0] == 0) result = 0;
	if (g_table_structureInfo[STRUCTURE_CONSTRUCTION_YARD].upgradeCampaign[1] != 0) result = 0;
	if (g_table_structureInfo[STRUCTURE_SLAB_2x2].o.upgradeLevelRequired != 1) result = 0;

	/* A cycle and an orphan both have to be refused, or the validator is only
	 * an opinion. */
	g_table_structureInfo[STRUCTURE_REFINERY].o.structuresRequired = FLAG_STRUCTURE_HEAVY_VEHICLE;
	if (Structure_TechTree_Validate()) result = 0;
	Structure_TechTree_ApplyTree("mp");
	g_table_structureInfo[STRUCTURE_ROCKET_TURRET].o.upgradeLevelRequired = 2;
	if (Structure_TechTree_Validate()) result = 0;
	Structure_TechTree_ApplyTree("mp");

	/* Now play it.  Ordos on purpose: the Harkonnen WOR waiver and the Atreides
	 * WOR ban are both House identity, and neither belongs in a test of the
	 * tree.  Campaign 8 is what a match sets, and it is what makes the mask the
	 * whole gate. */
	{
		House *h = House_Allocate(HOUSE_ORDOS);
		Structure yard;
		uint32 offered = 0;

		if (h == NULL) h = House_Get_ByIndex(HOUSE_ORDOS);

		g_campaignID = 8;
		Match_Reset();
		Match_SetSlot(0, HOUSE_ORDOS, MATCH_CONTROLLER_HUMAN_LOCAL);
		Match_SetSlot(1, HOUSE_HARKONNEN, MATCH_CONTROLLER_AI);
		Match_Begin();

		memset(&yard, 0, sizeof(yard));
		yard.o.type          = STRUCTURE_CONSTRUCTION_YARD;
		yard.o.houseID       = HOUSE_ORDOS;
		yard.creatorHouseID  = HOUSE_ORDOS;
		yard.upgradeLevel    = 0;
		yard.upgradeTimeLeft = 0;

		for (i = 0; i < lengthof(steps); i++) {
			h->structuresBuilt = FLAG_STRUCTURE_CONSTRUCTION_YARD | steps[i].built;
			offered = Structure_GetBuildable(&yard);

			if ((offered & steps[i].expected) != steps[i].expected) {
				printf("tech-tree: step %u offers %08x, missing %08x\n",
				       (unsigned)i, (unsigned)offered, (unsigned)(steps[i].expected & ~offered));
				result = 0;
			}
			if ((offered & steps[i].forbidden) != 0) {
				printf("tech-tree: step %u offers %08x too early\n",
				       (unsigned)i, (unsigned)(offered & steps[i].forbidden));
				result = 0;
			}
		}

		/* The same house with the AI at the wheel is waived from the tree
		 * entirely, and that asymmetry is deliberate -- if it ever stops being
		 * true the AI stops building. */
		Match_Reset();
		Match_SetSlot(0, HOUSE_ORDOS, MATCH_CONTROLLER_AI);
		Match_Begin();
		h->structuresBuilt = FLAG_STRUCTURE_CONSTRUCTION_YARD;
		offered = Structure_GetBuildable(&yard);
		if ((offered & FLAG_STRUCTURE_HEAVY_VEHICLE) == 0) result = 0;

		Match_Reset();
		h->structuresBuilt = 0;
		g_campaignID = campaignID;
	}

	/* Leave the process holding what the player configured, not what the test
	 * was looking at. */
	Structure_TechTree_Init();
	if (strcasecmp(s_techTreeName, configured) != 0) result = 0;

	return result;
}

/**
 * Whether this structure type is concrete rather than a building.
 *
 * Walls are deliberately not in it: a wall on sand is a different decision, it
 * has no foundation to offer and it would let a player fence off open desert.
 */
static bool Structure_IsSlab(StructureType type)
{
	return (type == STRUCTURE_SLAB_1x1 || type == STRUCTURE_SLAB_2x2);
}

/**
 * The ground a slab may be poured on, over and above the rock it always could.
 *
 * Spice is included and paving over it destroys it, which is the player's
 * business: it costs them the field.  Rubble is not -- LST_DESTROYED_WALL is
 * already valid for building.  Mountain is not, because nothing crosses it.
 */
static bool Structure_SlabAllowedOn(uint16 lst)
{
	switch (lst) {
		case LST_NORMAL_SAND:
		case LST_PARTIAL_ROCK:
		case LST_ENTIRELY_DUNE:
		case LST_PARTIAL_DUNE:
		case LST_SPICE:
		case LST_THICK_SPICE:
			return true;

		default:
			return false;
	}
}

/**
 * Checks if the given position is a valid location for the given structure type.
 *
 * @param position The (packed) tile to check.
 * @param type The structure type to check the position for.
 * @return 0 if the position is not valid, 1 if the position is valid and have enough slabs, <0 if the position is valid but miss some slabs.
 */
int16 Structure_IsValidBuildLocation(uint16 position, StructureType type, uint8 houseID)
{
	const StructureInfo *si;
	const uint16 *layoutTile;
	uint8 i;
	uint16 neededSlabs;
	bool isValid;
	uint16 curPos;

	si = &g_table_structureInfo[type];
	layoutTile = g_table_structure_layoutTiles[si->layout];

	isValid = true;
	neededSlabs = 0;
	for (i = 0; i < g_table_structure_layoutTileCount[si->layout]; i++) {
		uint16 lst;

		curPos = position + layoutTile[i];

		lst = Map_GetLandscapeType(curPos);

		if (g_debugScenario) {
			if (!g_table_landscapeInfo[lst].isValidForStructure2) {
				isValid = false;
				break;
			}
		} else {
			if (!Map_IsValidPosition(curPos)) {
				isValid = false;
				break;
			}

			if (si->o.flags.notOnConcrete) {
				bool ok = g_table_landscapeInfo[lst].isValidForStructure2;

				if (!ok && s_slabOnSand && Structure_IsSlab(type)) ok = Structure_SlabAllowedOn(lst);

				if (!ok && g_validateStrictIfZero == 0) {
					isValid = false;
					break;
				}
			} else {
				if (!g_table_landscapeInfo[lst].isValidForStructure && g_validateStrictIfZero == 0) {
					isValid = false;
					break;
				}
				if (lst != LST_CONCRETE_SLAB) neededSlabs++;
			}
		}

		if (Object_GetByPackedTile(curPos) != NULL) {
			isValid = false;
			break;
		}
	}

	/* "Must touch something of your own" -- and whose is decided by the house
	 * asking, not by whoever happens to be at the keyboard.  Written against
	 * g_playerHouseID it was unsatisfiable for every AI house, and in a skirmish,
	 * where the player owns nothing at all, for everybody. */
	if (g_validateStrictIfZero == 0 && isValid && type != STRUCTURE_CONSTRUCTION_YARD && !g_debugScenario) {
		isValid = false;
		for (i = 0; i < 16; i++) {
			uint16 offset, lst;
			Structure *s;

			offset = g_table_structure_layoutTilesAround[si->layout][i];
			if (offset == 0) break;

			curPos = position + offset;
			s = Structure_Get_ByPackedTile(curPos);
			if (s != NULL) {
				if (s->o.houseID != houseID) continue;
				isValid = true;
				break;
			}

			lst = Map_GetLandscapeType(curPos);
			if (lst != LST_CONCRETE_SLAB && lst != LST_WALL) continue;
			if (g_map[curPos].houseID != houseID) continue;

			isValid = true;
			break;
		}
	}

	if (!isValid) return 0;
	if (neededSlabs == 0) return 1;
	return -neededSlabs;
}

/**
 * Activate the special weapon of a house.
 *
 * @param s The structure which launches the weapon. Has to be the Palace.
 */
void Structure_ActivateSpecial(Structure *s)
{
	House *h;

	if (s == NULL) return;
	if (s->o.type != STRUCTURE_PALACE) return;

	h = House_Get_ByIndex(s->o.houseID);
	if (!h->flags.used) return;

	switch (g_table_houseInfo[s->o.houseID].specialWeapon) {
		case HOUSE_WEAPON_MISSILE: {
			Unit *u;
			tile32 position;

			position.x = 0xFFFF;
			position.y = 0xFFFF;

			g_validateStrictIfZero++;
			u = Unit_Create(UNIT_INDEX_INVALID, UNIT_MISSILE_HOUSE, s->o.houseID, position, Tools_Random_256());
			g_validateStrictIfZero--;

			g_unitHouseMissile = u;
			if (u == NULL) break;

			s->countDown = g_table_houseInfo[s->o.houseID].specialCountDown;

			if (!h->flags.human) {
				PoolFindStruct find;

				find.houseID = HOUSE_INVALID;
				find.type    = 0xFFFF;
				find.index   = 0xFFFF;

				/* For the AI, try to find the first structure which is not ours, and launch missile to there */
				while (true) {
					Structure *sf;

					sf = Structure_Find(&find);
					if (sf == NULL) break;
					if (sf->o.type == STRUCTURE_SLAB_1x1 || sf->o.type == STRUCTURE_SLAB_2x2 || sf->o.type == STRUCTURE_WALL) continue;

					if (House_AreAllied(s->o.houseID, sf->o.houseID)) continue;

					Unit_LaunchHouseMissile(Tile_PackTile(sf->o.position));

					return;
				}

				/* We failed to find a target, so remove the missile */
				Unit_Free(u);
				g_unitHouseMissile = NULL;

				return;
			}

			/* Give the user 7 seconds to select their target */
			g_houseMissileCountdown = 7;

			/* Arming the missile is the world's business and both clients do
			 * it; being handed the crosshair is the owner's, and doing it to
			 * the other player would take their interface away for something
			 * happening on the far side of the map. */
			if (s->o.houseID == g_playerHouseID) GUI_ChangeSelectionType(SELECTIONTYPE_TARGET);
		} break;

		case HOUSE_WEAPON_FREMEN: {
			uint16 location;
			uint16 i;

			/* Find a random location to appear */
			location = Map_FindLocationTile(4, HOUSE_INVALID);

			for (i = 0; i < 5; i++) {
				Unit *u;
				tile32 position;
				uint16 orientation;
				uint16 unitType;

				Tools_Random_256();

				position = Tile_UnpackTile(location);
				position = Tile_MoveByRandom(position, 32, true);

				orientation = Tools_RandomLCG_Range(0, 3);
				unitType = (orientation == 1) ? UNIT_TROOPER : UNIT_TROOPERS;

				g_validateStrictIfZero++;
				u = Unit_Create(UNIT_INDEX_INVALID, (uint8)unitType, HOUSE_FREMEN, position, (int8)orientation);
				g_validateStrictIfZero--;

				if (u == NULL) continue;

				Unit_SetAction(u, ACTION_HUNT);
			}

			s->countDown = g_table_houseInfo[s->o.houseID].specialCountDown;
		} break;

		case HOUSE_WEAPON_SABOTEUR: {
			Unit *u;
			uint16 position;

			/* Find a spot next to the structure */
			position = Structure_FindFreePosition(s, false);

			/* If there is no spot, reset countdown */
			if (position == 0) {
				s->countDown = 1;
				return;
			}

			g_validateStrictIfZero++;
			u = Unit_Create(UNIT_INDEX_INVALID, UNIT_SABOTEUR, s->o.houseID, Tile_UnpackTile(position), Tools_Random_256());
			g_validateStrictIfZero--;

			if (u == NULL) return;

			Unit_SetAction(u, ACTION_SABOTAGE);

			s->countDown = g_table_houseInfo[s->o.houseID].specialCountDown;
		} break;

		default: break;
	}

	if (s->o.houseID == g_playerHouseID) {
		GUI_Widget_ActionPanel_Draw(true);
	}
}

/**
 * Remove the fog around a structure.
 *
 * @param s The Structure.
 */
void Structure_RemoveFog(Structure *s)
{
	const StructureInfo *si;
	tile32 position;

	if (s == NULL || s->o.houseID != g_playerHouseID) return;

	si = &g_table_structureInfo[s->o.type];

	position = s->o.position;

	/* ENHANCEMENT -- Fog is removed around the top left corner instead of the center of a structure. */
	if (g_dune2_enhanced) {
		position.x += 256 * (g_table_structure_layoutSize[si->layout].width  - 1) / 2;
		position.y += 256 * (g_table_structure_layoutSize[si->layout].height - 1) / 2;
	}

	Tile_RemoveFogInRadius(position, si->o.fogUncoverRadius);
}

/**
 * Handles destroying of a structure.
 *
 * @param s The Structure.
 */
static void Structure_Destroy(Structure *s)
{
	const StructureInfo *si;
	uint8 linkedID;
	House *h;

	if (s == NULL) return;

	if (g_debugScenario) {
		Structure_Remove(s);
		return;
	}

	/* A turret going down is the other half of the ledger: units lost to turrets
	 * are only acceptable if turrets came down for them.  In a two-House match
	 * the killer is the only other House. */
	if (Skirmish_IsActive() && (s->o.type == STRUCTURE_TURRET || s->o.type == STRUCTURE_ROCKET_TURRET)) {
		Doctrine_RecordTurretKilled(Skirmish_GetOpponent(s->o.houseID));
	}

	s->o.script.variables[0] = 1;
	s->o.flags.s.allocated = false;
	s->o.flags.s.repairing = false;
	s->o.script.delay = 0;

	Script_Reset(&s->o.script, g_scriptStructure);
	Script_Load(&s->o.script, s->o.type);

	Voice_PlayAtTile(44, s->o.position);

	linkedID = s->o.linkedID;

	if (linkedID != 0xFF) {
		if (s->o.type == STRUCTURE_CONSTRUCTION_YARD) {
			Structure_Destroy(Structure_Get_ByIndex(linkedID));
			s->o.linkedID = 0xFF;
		} else {
			while (linkedID != 0xFF) {
				Unit *u = Unit_Get_ByIndex(linkedID);

				linkedID = u->o.linkedID;

				Unit_Remove(u);
			}
		}
	}

	h = House_Get_ByIndex(s->o.houseID);
	si = &g_table_structureInfo[s->o.type];

	h->credits -= (h->creditsStorage == 0) ? h->credits : min(h->credits, (h->credits * 256 / h->creditsStorage) * si->creditsStorage / 256);

	if (!Match_IsHumanControlled(s->o.houseID)) h->credits += si->o.buildCredits + (g_campaignID > 7 ? si->o.buildCredits / 2 : 0);

	if (s->o.type != STRUCTURE_WINDTRAP) return;

	h->windtrapCount--;
}

/**
 * Damage the structure, and bring the surrounding to an explosion if needed.
 *
 * @param s The structure to damage.
 * @param damage The damage to deal to the structure.
 * @param range The range in which an explosion should be possible.
 * @return True if and only if the structure is now destroyed.
 */
bool Structure_Damage(Structure *s, uint16 damage, uint16 range)
{
	const StructureInfo *si;

	if (s == NULL) return false;
	if (damage == 0) return false;
	if (s->o.script.variables[0] == 1) return false;

	si = &g_table_structureInfo[s->o.type];

	if (Skirmish_IsActive()) {
		Skirmish_RecordDamage((uint8)s->o.houseID, min(damage, s->o.hitpoints));
	}

	if (s->o.hitpoints >= damage) {
		s->o.hitpoints -= damage;
	} else {
		s->o.hitpoints = 0;
	}

	if (s->o.hitpoints == 0) {
		uint16 score;

		score = si->o.buildCredits / 100;
		if (score < 1) score = 1;

		if (House_AreAllied(g_playerHouseID, s->o.houseID)) {
			g_scenario.destroyedAllied++;
			g_scenario.score -= score;
		} else {
			g_scenario.destroyedEnemy++;
			g_scenario.score += score;
		}

		Structure_Destroy(s);

		if (g_playerHouseID == s->o.houseID) {
			uint16 index;

			switch (s->o.houseID) {
				case HOUSE_HARKONNEN: index = 22; break;
				case HOUSE_ATREIDES:  index = 23; break;
				case HOUSE_ORDOS:     index = 24; break;
				default: index = 0xFFFF; break;
			}

			Sound_Output_Feedback(index);
		} else {
			Sound_Output_Feedback(21);
		}

		Structure_UntargetMe(s);
		return true;
	}

	if (range == 0) return false;

	Map_MakeExplosion(EXPLOSION_IMPACT_LARGE, Tile_AddTileDiff(s->o.position, g_table_structure_layoutTileDiff[si->layout]), 0, 0);
	return false;
}

/**
 * Check wether the given structure is upgradable.
 *
 * @param s The Structure to check.
 * @return True if and only if the structure is upgradable.
 */
bool Structure_IsUpgradable(Structure *s)
{
	const StructureInfo *si;

	if (s == NULL) return false;

	si = &g_table_structureInfo[s->o.type];

	if (s->o.houseID == HOUSE_HARKONNEN && s->o.type == STRUCTURE_HIGH_TECH) return false;
	if (s->o.houseID == HOUSE_ORDOS && s->o.type == STRUCTURE_HEAVY_VEHICLE && s->upgradeLevel == 1 && si->upgradeCampaign[2] > g_campaignID) return false;

	if (si->upgradeCampaign[s->upgradeLevel] != 0 && si->upgradeCampaign[s->upgradeLevel] <= g_campaignID + 1) {
		House *h;

		if (s->o.type != STRUCTURE_CONSTRUCTION_YARD) return true;
		if (s->upgradeLevel != 1) return true;

		h = House_Get_ByIndex(s->o.houseID);
		if ((h->structuresBuilt & g_table_structureInfo[STRUCTURE_ROCKET_TURRET].o.structuresRequired) == g_table_structureInfo[STRUCTURE_ROCKET_TURRET].o.structuresRequired) return true;

		return false;
	}

	if (s->o.houseID == HOUSE_HARKONNEN && s->o.type == STRUCTURE_WOR_TROOPER && s->upgradeLevel == 0 && g_campaignID > 3) return true;
	return false;
}

/**
 * Connect walls around the given position.
 *
 * @param position The packed position.
 * @param recurse Wether to recurse.
 * @return True if and only if a change happened.
 */
bool Structure_ConnectWall(uint16 position, bool recurse)
{
	static const uint8 wall[] = {
		 0,  3,  1,  2,  3,  3,  4,  5,  1,  6,  1,  7,  8,  9, 10, 11,
		 1, 12,  1, 19,  1, 16,  1, 31,  1, 28,  1, 52,  1, 45,  1, 59,
		 3,  3, 13, 20,  3,  3, 22, 32,  3,  3, 13, 53,  3,  3, 38, 60,
		 5,  6,  7, 21,  5,  6,  7, 33,  5,  6,  7, 54,  5,  6,  7, 61,
		 9,  9,  9,  9, 17, 17, 23, 34,  9,  9,  9,  9, 25, 46, 39, 62,
		11, 12, 11, 12, 13, 18, 13, 35, 11, 12, 11, 12, 13, 47, 13, 63,
		15, 15, 16, 16, 17, 17, 24, 36, 15, 15, 16, 16, 17, 17, 40, 64,
		19, 20, 21, 22, 23, 24, 25, 37, 19, 20, 21, 22, 23, 24, 25, 65,
		27, 27, 27, 27, 27, 27, 27, 27, 14, 29, 14, 55, 26, 48, 41, 66,
		29, 30, 29, 30, 29, 30, 29, 30, 31, 30, 31, 56, 31, 49, 31, 67,
		33, 33, 34, 34, 33, 33, 34, 34, 35, 35, 15, 57, 35, 35, 42, 68,
		37, 38, 39, 40, 37, 38, 39, 40, 41, 42, 43, 58, 41, 42, 43, 69,
		45, 45, 45, 45, 46, 46, 46, 46, 47, 47, 47, 47, 27, 50, 43, 70,
		49, 50, 49, 50, 51, 52, 51, 52, 53, 54, 53, 54, 55, 51, 55, 71,
		57, 57, 58, 58, 59, 59, 60, 60, 61, 61, 62, 62, 63, 63, 44, 72,
		65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 73
	};

	uint16 bits = 0;
	uint16 tileID;
	bool isDestroyedWall;
	uint8 i;
	Tile *tile;

	isDestroyedWall = Map_GetLandscapeType(position) == LST_DESTROYED_WALL;

	for (i = 0; i < 4; i++) {
		const uint16 curPos = position + g_table_mapDiff[i];

		if (recurse && Map_GetLandscapeType(curPos) == LST_WALL) Structure_ConnectWall(curPos, false);

		if (isDestroyedWall) continue;

		switch (Map_GetLandscapeType(curPos)) {
			case LST_DESTROYED_WALL: bits |= (1 << (i + 4));
				/* FALL-THROUGH */
			case LST_WALL: bits |= (1 << i);
				/* FALL-THROUGH */
			default:  break;
		}
	}

	if (isDestroyedWall) return false;

	tileID = g_wallTileID + wall[bits] + 1;

	tile = &g_map[position];
	if (tile->groundTileID == tileID) return false;

	tile->groundTileID = tileID;
	g_mapTileID[position] |= 0x8000;
	Map_Update(position, 0, false);

	return true;
}

/**
 * Get the unit linked to this structure, or NULL if there is no.
 * @param s The structure to get the linked unit from.
 * @return The linked unit, or NULL if there was none.
 */
Unit *Structure_GetLinkedUnit(Structure *s)
{
	if (s->o.linkedID == 0xFF) return NULL;
	return Unit_Get_ByIndex(s->o.linkedID);
}

/**
 * Untarget the given Structure.
 *
 * @param unit The Structure to untarget.
 */
void Structure_UntargetMe(Structure *s)
{
	PoolFindStruct find;
	uint16 encoded = Tools_Index_Encode(s->o.index, IT_STRUCTURE);

	Object_Script_Variable4_Clear(&s->o);

	find.houseID = HOUSE_INVALID;
	find.index   = 0xFFFF;
	find.type    = 0xFFFF;

	while (true) {
		Unit *u;

		u = Unit_Find(&find);
		if (u == NULL) break;

		if (u->targetMove == encoded) u->targetMove = 0;
		if (u->targetAttack == encoded) u->targetAttack = 0;
		if (u->o.script.variables[4] == encoded) Object_Script_Variable4_Clear(&u->o);
	}

	find.houseID = HOUSE_INVALID;
	find.index   = 0xFFFF;
	find.type    = 0xFFFF;

	while (true) {
		Team *t;

		t = Team_Find(&find);
		if (t == NULL) break;

		if (t->target == encoded) t->target = 0;
	}
}

/**
 * Find a free spot for units next to a structure.
 * @param s Structure that needs a free spot.
 * @param checkForSpice Spot should be as close to spice as possible.
 * @return Position of the free spot, or \c 0 if no free spot available.
 */
uint16 Structure_FindFreePosition(Structure *s, bool checkForSpice)
{
	const StructureInfo *si;
	uint16 packed;
	uint16 spicePacked;  /* Position of the spice, or 0 if not used or if no spice. */
	uint16 bestPacked;
	uint16 bestDistance; /* If > 0, distance to the spice from bestPacked. */
	uint16 i, j;

	if (s == NULL) return 0;

	si = &g_table_structureInfo[s->o.type];
	packed = Tile_PackTile(Tile_Center(s->o.position));

	spicePacked = (checkForSpice) ? Map_SearchSpice(packed, 10) : 0;
	bestPacked = 0;
	bestDistance = 0;

	i = Tools_Random_256() & 0xF;
	for (j = 0; j < 16; j++, i = (i + 1) & 0xF) {
		uint16 offset;
		uint16 curPacked;
		uint16 type;
		Tile *t;

		offset = g_table_structure_layoutTilesAround[si->layout][i];
		if (offset == 0) continue;

		curPacked = packed + offset;
		if (!Map_IsValidPosition(curPacked)) continue;

		type = Map_GetLandscapeType(curPacked);
		if (type == LST_WALL || type == LST_ENTIRELY_MOUNTAIN || type == LST_PARTIAL_MOUNTAIN) continue;

		t = &g_map[curPacked];
		if (t->hasUnit || t->hasStructure) continue;

		if (!checkForSpice) return curPacked;

		if (bestDistance == 0 || Tile_GetDistancePacked(curPacked, spicePacked) < bestDistance) {
			bestPacked = curPacked;
			bestDistance = Tile_GetDistancePacked(curPacked, spicePacked);
		}
	}

	return bestPacked;
}

/**
 * Remove the structure from the map, free it, and clean up after it.
 * @param s The structure to remove.
 */
void Structure_Remove(Structure *s)
{
	const StructureInfo *si;
	uint16 packed;
	uint16 i;
	House *h;

	if (s == NULL) return;

	si = &g_table_structureInfo[s->o.type];
	packed = Tile_PackTile(s->o.position);

	/* The pool hands this index to the next structure built. */
	Structure_SetRallyPoint(s, 0);

	for (i = 0; i < g_table_structure_layoutTileCount[si->layout]; i++) {
		Tile *t;
		uint16 curPacked = packed + g_table_structure_layoutTiles[si->layout][i];

		Animation_Stop_ByTile(curPacked);

		t = &g_map[curPacked];
		t->hasStructure = false;

		if (g_debugScenario) {
			t->groundTileID = g_mapTileID[curPacked] & 0x1FF;
			t->overlayTileID = 0;
		}
	}

	if (!g_debugScenario) {
		Animation_Start(g_table_animation_structure[0], s->o.position, si->layout, s->o.houseID, (uint8)si->iconGroup);
	}

	h = House_Get_ByIndex(s->o.houseID);

	for (i = 0; i < 5; i++) {
		if (h->ai_structureRebuild[i][0] != 0) continue;
		h->ai_structureRebuild[i][0] = s->o.type;
		h->ai_structureRebuild[i][1] = packed;
		break;
	}

	Structure_Free(s);
	Structure_UntargetMe(s);

	h->structuresBuilt = Structure_GetStructuresBuilt(h);

	House_UpdateCreditsStorage(s->o.houseID);

	if (g_debugScenario) return;

	switch (s->o.type) {
		case STRUCTURE_WINDTRAP:
			House_CalculatePowerAndCredit(h);
			break;

		case STRUCTURE_OUTPOST:
			House_UpdateRadarState(h);
			break;

		default: break;
	}
}

/**
 * Check if requested structureType can be build on the map with concrete below.
 *
 * @param structureType The type of structure to check for.
 * @param houseID The house to check for.
 * @return True if and only if there are enough slabs available on the map to
 *  build requested structure.
 */
static bool Structure_CheckAvailableConcrete(uint16 structureType, uint8 houseID)
{
	const StructureInfo *si;
	uint16 tileCount;
	uint16 i;

	si = &g_table_structureInfo[structureType];

	tileCount = g_table_structure_layoutTileCount[si->layout];

	if (structureType == STRUCTURE_SLAB_1x1 || structureType == STRUCTURE_SLAB_2x2) return true;

	for (i = 0; i < 4096; i++) {
		bool stop = true;
		uint16 j;

		for (j = 0; j < tileCount; j++) {
			uint16 packed = i + g_table_structure_layoutTiles[si->layout][j];
			/* XXX -- This can overflow, and we should check for that */

			if (Map_GetLandscapeType(packed) == LST_CONCRETE_SLAB && g_map[packed].houseID == houseID) continue;

			stop = false;
			break;
		}

		if (stop) return true;
	}

	return false;
}

/**
 * Cancel the building of object for given structure.
 *
 * @param s The Structure.
 */
static void Structure_CancelBuild(Structure *s)
{
	ObjectInfo *oi;

	if (s == NULL) return;
	if (s->o.linkedID == 0xFF) {
		Structure_Queue_Clear(s);
		return;
	}

	if (s->o.type == STRUCTURE_CONSTRUCTION_YARD) {
		Structure *s2 = Structure_Get_ByIndex(s->o.linkedID);
		oi = &g_table_structureInfo[s2->o.type].o;
		Structure_Free(s2);
	} else {
		Unit *u = Unit_Get_ByIndex(s->o.linkedID);
		oi = &g_table_unitInfo[u->o.type].o;
		Unit_Free(u);
	}

	House_Get_ByIndex(s->o.houseID)->credits += ((oi->buildTime - (s->countDown >> 8)) * 256 / oi->buildTime) * oi->buildCredits / 256;

	s->o.flags.s.onHold = false;
	s->countDown = 0;
	s->o.linkedID = 0xFF;
	Structure_Queue_Clear(s);
}

/**
 * Make the given Structure build an object.
 *
 * @param s The Structure.
 * @param objectType The type of the object to build or a special value (0xFFFD, 0xFFFE, 0xFFFF).
 * @return ??.
 */
/**
 * Run a Starport order that has crossed the wire.
 *
 * The price travels with the order rather than being recomputed here, and it
 * has to: Starport prices are drawn from a generator seeded with the *viewer's*
 * own house, so the two clients would charge the buyer two different amounts.
 * The buyer says what it is paying and both clients take that off the same
 * house.  A client could of course name a price of its choosing -- so could it
 * fabricate any other command, which is the standing bargain of lockstep
 * between two people who chose to play each other.
 *
 * @param s The Starport.
 * @param items (objectType << 8 | amount) per line of the order.
 * @param count Lines in items.
 * @param credits What the buyer says the whole order costs.
 */
void Structure_StarportOrder(Structure *s, const uint16 *items, uint16 count, uint16 credits)
{
	House *h;
	uint16 line;

	if (s == NULL || s->o.type != STRUCTURE_STARPORT) return;

	h = House_Get_ByIndex(s->o.houseID);
	if (h == NULL) return;

	/* The shop rule again, where it is authoritative: the window only decides
	 * what one player is shown, and this runs on both clients.  Checked before
	 * anything is charged and refused whole rather than line by line -- the
	 * order carries one total and no per-line price, so dropping a line would
	 * charge for it.  A window that is telling the truth cannot produce such an
	 * order in the first place. */
	for (line = 0; line < count; line++) {
		const uint16 objectType = items[line] >> 8;

		if (objectType >= UNIT_MAX) continue;
		if (!Starport_Sells(objectType)) return;
	}

	/* Both clients hold the same credits, so both reach the same verdict. */
	if (h->credits < credits) return;
	h->credits -= credits;

	for (line = 0; line < count; line++) {
		uint16 objectType = items[line] >> 8;
		uint16 amount     = items[line] & 0xFF;

		if (objectType >= UNIT_MAX) continue;

		while (amount-- != 0) {
			Unit *u;

			g_validateStrictIfZero++;
			{
				tile32 tile;
				tile.x = 0xFFFF;
				tile.y = 0xFFFF;
				u = Unit_Create(UNIT_INDEX_INVALID, (uint8)objectType, s->o.houseID, tile, 0);
			}
			g_validateStrictIfZero--;

			/* The original's refund when the pool is full, quirk and all: it
			 * hands back a Carryall's price whatever was being bought. */
			if (u == NULL) {
				h->credits += g_table_unitInfo[UNIT_CARRYALL].o.buildCredits;
				if (s->o.houseID == g_playerHouseID) {
					GUI_DisplayText(String_Get_ByIndex(STR_UNABLE_TO_CREATE_MORE), 2);
				}
				continue;
			}

			g_structureIndex = s->o.index;

			if (h->starportTimeLeft == 0) h->starportTimeLeft = Starport_DeliveryTime((uint8)h->index);

			u->o.linkedID = h->starportLinkedID & 0xFF;
			h->starportLinkedID = u->o.index;

			g_starportAvailable[objectType]--;
			if (g_starportAvailable[objectType] <= 0) g_starportAvailable[objectType] = -1;
		}
	}
}

bool Structure_BuildObject(Structure *s, uint16 objectType)
{
	const StructureInfo *si;
	const char *str;
	Object *o;
	ObjectInfo *oi;

	if (s == NULL) return false;

	si = &g_table_structureInfo[s->o.type];

	if (!si->o.flags.factory) return false;

	/* Choosing something to build stops the repair, and that is a real change to
	 * the building -- but merely *opening* the list is one player looking at
	 * their own screen, and 0xFFFF is what the click that opens it passes.  In a
	 * match the look costs nothing; the choice arrives later as its own command
	 * and stops the repair on both clients at once. */
	if (!MpTurn_IsActive() || objectType != 0xFFFF) Structure_SetRepairingState(s, 0, NULL);

	if (objectType == 0xFFFD) {
		Structure_SetUpgradingState(s, 1, NULL);
		return false;
	}

	if (objectType == 0xFFFF || objectType == 0xFFFE) {
		uint16 upgradeCost = 0;
		uint32 buildable;

		if (Structure_IsUpgradable(s) && si->o.hitpoints == s->o.hitpoints) {
			upgradeCost = (si->o.buildCredits + (si->o.buildCredits >> 15)) / 2;
		}

		if (upgradeCost != 0 && s->o.type == STRUCTURE_HIGH_TECH && s->o.houseID == HOUSE_HARKONNEN) upgradeCost = 0;
		if (s->o.type == STRUCTURE_STARPORT) upgradeCost = 0;

		buildable = Structure_GetBuildable(s);

		if (buildable == 0) {
			/* Same reason: opening an empty list must not write the structure. */
			if (!MpTurn_IsActive() || objectType != 0xFFFF) s->objectType = 0;
			return false;
		}

		if (s->o.type == STRUCTURE_CONSTRUCTION_YARD) {
			uint8 i;

			g_factoryWindowConstructionYard = true;

			for (i = 0; i < STRUCTURE_MAX; i++) {
				if ((buildable & (1 << i)) == 0) continue;
				g_table_structureInfo[i].o.available = 1;
				if (objectType != 0xFFFE) continue;
				s->objectType = i;
				return false;
			}
		} else {
			g_factoryWindowConstructionYard = false;

			if (s->o.type == STRUCTURE_STARPORT) {
				uint8 linkedID = 0xFF;
				int16 availableUnits[UNIT_MAX];
				Unit *u;
				bool loop;

				memset(availableUnits, 0, sizeof(availableUnits));

				do {
					uint8 i;

					loop = false;

					for (i = 0; i < UNIT_MAX; i++) {
						int16 unitsAtStarport = g_starportAvailable[i];

						/* The freighter carries the whole roster, but a house
						 * may only take delivery of what it is allowed to
						 * field: the Ordos Deviator is not for sale to
						 * Harkonnen merely because a Starport is standing
						 * there.  Asked of the creator rather than the current
						 * owner, which is how the factories decide it too. */
						if ((g_table_unitInfo[i].o.availableHouse & (1 << s->creatorHouseID)) == 0) {
							g_table_unitInfo[i].o.available = 0;
							continue;
						}

						/* And only what the freighter carries as ordinary
						 * merchandise: a House's own unit is that House's
						 * building to produce, not a shortcut round it. */
						if (!Starport_Sells(i)) {
							g_table_unitInfo[i].o.available = 0;
							continue;
						}

						if (unitsAtStarport == 0) {
							g_table_unitInfo[i].o.available = 0;
						} else if (unitsAtStarport < 0) {
							g_table_unitInfo[i].o.available = -1;
						} else if (unitsAtStarport > availableUnits[i]) {
							g_validateStrictIfZero++;
							u = Unit_Allocate(UNIT_INDEX_INVALID, i, s->o.houseID);
							g_validateStrictIfZero--;

							if (u != NULL) {
								loop = true;
								u->o.linkedID = linkedID;
								linkedID = u->o.index & 0xFF;
								availableUnits[i]++;
								g_table_unitInfo[i].o.available = (int8)availableUnits[i];
							} else if (availableUnits[i] == 0) g_table_unitInfo[i].o.available = -1;
						}
					}
				} while (loop);

				while (linkedID != 0xFF) {
					u = Unit_Get_ByIndex(linkedID);
					linkedID = u->o.linkedID;
					Unit_Free(u);
				}

				/* An empty Starport has to close the click, not the process.
				 * Structure_GetBuildable() answers -1 for a Starport, so the
				 * "buildable == 0" bail above can never fire for one, and the
				 * window it opens instead ends an empty list with exit(0).  A
				 * generated map has no CHOAM section to stock from, which put
				 * that exit one click away from any player who built one. */
				{
					uint8 i;
					bool anyStock = false;

					for (i = 0; i < UNIT_MAX; i++) {
						if (g_table_unitInfo[i].o.available != 0) {
							anyStock = true;
							break;
						}
					}

					if (!anyStock) return false;
				}
			} else {
				uint8 i;

				for (i = 0; i < UNIT_MAX; i++) {
					if ((buildable & (1 << i)) == 0) continue;
					g_table_unitInfo[i].o.available = 1;
					if (objectType != 0xFFFE) continue;
					s->objectType = i;
					return false;
				}
			}
		}

		if (objectType == 0xFFFF) {
			FactoryResult res;

			Sprites_UnloadTiles();

			memmove(g_palette1, g_paletteActive, 256 * 3);

			GUI_ChangeSelectionType(SELECTIONTYPE_MENTAT);

			Timer_SetTimer(TIMER_GAME, false);

			res = GUI_DisplayFactoryWindow(g_factoryWindowConstructionYard, s->o.type == STRUCTURE_STARPORT ? 1 : 0, upgradeCost);

			Timer_SetTimer(TIMER_GAME, true);

			Sprites_LoadTiles();

			GFX_SetPalette(g_palette1);

			GUI_ChangeSelectionType(SELECTIONTYPE_STRUCTURE);

			if (res == FACTORY_RESUME) return false;

			if (res == FACTORY_UPGRADE) {
				Structure_SetUpgradingState(s, 1, NULL);
				return false;
			}

			/* The build list is a screen, and choosing from it is an order.  The
			 * branch below sets s->objectType on the way out, which is this
			 * client alone deciding what the factory is making -- and it is why
			 * one player's refinery never appeared for the other: the command
			 * that follows carried a type the other client's structure had never
			 * been told about, and the two structures diverged whether or not
			 * the building went up.  In a match the choice travels instead. */
			if (MpTurn_IsActive() && res == FACTORY_BUY && s->o.type != STRUCTURE_STARPORT) {
				uint8 i;

				for (i = 0; i < 25; i++) {
					MpCommand cmd;

					if (g_factoryWindowItems[i].amount == 0) continue;

					MpCommand_Init(&cmd, MP_CMD_STRUCTURE_BUILD, s->o.houseID);
					cmd.object = s->o.index;
					cmd.value  = g_factoryWindowItems[i].objectType;
					MpCommand_Submit(&cmd);
					break;
				}

				return false;
			}

			/* The Starport buys a list rather than a single thing, and it is the
			 * one window whose prices differ between the two clients, so the
			 * order carries both: a line per item, and the total the buyer is
			 * paying.  The +/- buttons took nothing while it was open. */
			if (MpTurn_IsActive() && res == FACTORY_BUY) {
				MpCommand cmd;
				uint16 total = 0;
				uint8 i;

				MpCommand_Init(&cmd, MP_CMD_STRUCTURE_STARPORT, s->o.houseID);
				cmd.object = s->o.index;

				for (i = 0; i < 25 && cmd.count < MP_COMMAND_UNITS_MAX; i++) {
					if (g_factoryWindowItems[i].amount == 0) continue;

					cmd.unit[cmd.count++] = (uint16)((g_factoryWindowItems[i].objectType << 8) |
					                                 (g_factoryWindowItems[i].amount & 0xFF));
					total += g_factoryWindowItems[i].amount * g_factoryWindowItems[i].credits;
				}

				cmd.value = total;

				if (cmd.count != 0) MpCommand_Submit(&cmd);

				return false;
			}

			if (res == FACTORY_BUY) {
				House *h;
				uint8 i;

				h = House_Get_ByIndex(s->o.houseID);

				for (i = 0; i < 25; i++) {
					Unit *u;

					if (g_factoryWindowItems[i].amount == 0) continue;
					objectType = g_factoryWindowItems[i].objectType;

					if (s->o.type != STRUCTURE_STARPORT) {
						Structure_CancelBuild(s);

						s->objectType = objectType;

						if (!g_factoryWindowConstructionYard) continue;

						if (Structure_CheckAvailableConcrete(objectType, s->o.houseID)) continue;

						if (GUI_DisplayHint(STR_THERE_ISNT_ENOUGH_OPEN_CONCRETE_TO_PLACE_THIS_STRUCTURE_YOU_MAY_PROCEED_BUT_WITHOUT_ENOUGH_CONCRETE_THE_BUILDING_WILL_NEED_REPAIRS, g_table_structureInfo[objectType].o.spriteID) == 0) continue;

						s->objectType = objectType;

						return false;
					}

					g_validateStrictIfZero++;
					{
						tile32 tile;
						tile.x = 0xFFFF;
						tile.y = 0xFFFF;
						u = Unit_Create(UNIT_INDEX_INVALID, (uint8)objectType, s->o.houseID, tile, 0);
					}
					g_validateStrictIfZero--;

					if (u == NULL) {
						h->credits += g_table_unitInfo[UNIT_CARRYALL].o.buildCredits;
						if (s->o.houseID != g_playerHouseID) continue;
						GUI_DisplayText(String_Get_ByIndex(STR_UNABLE_TO_CREATE_MORE), 2);
						continue;
					}

					g_structureIndex = s->o.index;

					if (h->starportTimeLeft == 0) h->starportTimeLeft = Starport_DeliveryTime((uint8)h->index);

					u->o.linkedID = h->starportLinkedID & 0xFF;
					h->starportLinkedID = u->o.index;

					g_starportAvailable[objectType]--;
					if (g_starportAvailable[objectType] <= 0) g_starportAvailable[objectType] = -1;

					g_factoryWindowItems[i].amount--;
					if (g_factoryWindowItems[i].amount != 0) i--;
				}
			}
		} else {
			s->objectType = objectType;
		}
	}

	if (s->o.type == STRUCTURE_STARPORT) return true;

	if (s->objectType != objectType) {
		/* Choosing a different building gives back what the yard is holding.
		 * That is what the original game already did with the one finished
		 * building it could be sitting on; a stack of them is the same bargain
		 * repeated, and it is what keeps the stack a single type. */
		Structure_Queue_RefundReady(s);
		Structure_CancelBuild(s);
	}

	if (s->o.linkedID != 0xFF || objectType == 0xFFFF) return false;

	if (s->o.type != STRUCTURE_CONSTRUCTION_YARD) {
		Unit *nu;
		tile32 tile;
		tile.x = 0xFFFF;
		tile.y = 0xFFFF;

		oi = &g_table_unitInfo[objectType].o;
		/* Both creates fail when the pool band is full, and the check below is
		 * written expecting a NULL back.  Taking &x->o straight off the call is
		 * undefined when x is NULL, even though every compiler folds it to NULL
		 * because Object sits at offset 0 -- and undefined is a desync waiting
		 * for a different compiler (mp.md). */
		nu = Unit_Create(UNIT_INDEX_INVALID, (uint8)objectType, s->o.houseID, tile, 0);
		o = (nu != NULL) ? &nu->o : NULL;
		str = String_Get_ByIndex(g_table_unitInfo[objectType].o.stringID_full);
	} else {
		Structure *ns;

		oi = &g_table_structureInfo[objectType].o;
		ns = Structure_Create(STRUCTURE_INDEX_INVALID, (uint8)objectType, s->o.houseID, 0xFFFF);
		o = (ns != NULL) ? &ns->o : NULL;
		str = String_Get_ByIndex(g_table_structureInfo[objectType].o.stringID_full);
	}

	s->o.flags.s.onHold = false;

	if (o != NULL) {
		s->o.linkedID = o->index & 0xFF;
		s->objectType = objectType;
		s->countDown = oi->buildTime << 8;

		Structure_SetState(s, STRUCTURE_STATE_BUSY);

		if (s->o.houseID != g_playerHouseID) return true;

		GUI_DisplayText(String_Get_ByIndex(STR_PRODUCTION_OF_S_HAS_STARTED), 2, str);

		return true;
	}

	if (s->o.houseID != g_playerHouseID) return false;

	GUI_DisplayText(String_Get_ByIndex(STR_UNABLE_TO_CREATE_MORE), 2);

	return false;
}

/**
 * Sets or toggle the upgrading state of the given Structure.
 *
 * @param s The Structure.
 * @param value The upgrading state, -1 to toggle.
 * @param w The widget.
 * @return True if and only if the state changed.
 */
bool Structure_SetUpgradingState(Structure *s, int8 state, Widget *w)
{
	bool ret = false;

	if (s == NULL) return false;

	if (state == -1) state = s->o.flags.s.upgrading ? 0 : 1;

	if (state == 0 && s->o.flags.s.upgrading) {
		if (s->o.houseID == g_playerHouseID) {
			GUI_DisplayText(String_Get_ByIndex(STR_UPGRADING_STOPS), 2);
		}

		s->o.flags.s.upgrading = false;
		s->o.flags.s.onHold = false;

		GUI_Widget_MakeNormal(w, false);

		ret = true;
	}

	if (state == 0 || s->o.flags.s.upgrading || s->upgradeTimeLeft == 0) return ret;

	if (s->o.houseID == g_playerHouseID) {
		GUI_DisplayText(String_Get_ByIndex(STR_UPGRADING_STARTS), 2);
	}

	s->o.flags.s.onHold = true;
	s->o.flags.s.repairing = false;
	s->o.flags.s.upgrading = true;

	GUI_Widget_MakeSelected(w, false);

	return true;
}

/**
 * Sets or toggle the repairing state of the given Structure.
 *
 * @param s The Structure.
 * @param value The repairing state, -1 to toggle.
 * @param w The widget.
 * @return True if and only if the state changed.
 */
bool Structure_SetRepairingState(Structure *s, int8 state, Widget *w)
{
	bool ret = false;

	if (s == NULL) return false;

	/* ENHANCEMENT -- If a structure gets damaged during upgrading, pressing the "Upgrading" button silently starts the repair of the structure, and doesn't cancel upgrading. */
	if (g_dune2_enhanced && s->o.flags.s.upgrading) return false;

	if (!s->o.flags.s.allocated) state = 0;

	if (state == -1) state = s->o.flags.s.repairing ? 0 : 1;

	if (state == 0 && s->o.flags.s.repairing) {
		if (s->o.houseID == g_playerHouseID) {
			GUI_DisplayText(String_Get_ByIndex(STR_REPAIRING_STOPS), 2);
		}

		s->o.flags.s.repairing = false;
		s->o.flags.s.onHold = false;

		GUI_Widget_MakeNormal(w, false);

		ret = true;
	}

	if (state == 0 || s->o.flags.s.repairing || s->o.hitpoints == g_table_structureInfo[s->o.type].o.hitpoints) return ret;

	if (s->o.houseID == g_playerHouseID) {
		GUI_DisplayText(String_Get_ByIndex(STR_REPAIRING_STARTS), 2);
	}

	s->o.flags.s.onHold = true;
	s->o.flags.s.repairing = true;

	GUI_Widget_MakeSelected(w, false);

	return true;
}

/**
 * Auto repair: every damaged building of a House starts repairing itself.
 *
 * The switch belongs to the Repair facility -- it is bought with one and it
 * stops working when the last one falls -- but the state is the House's, so
 * several facilities show one shared setting rather than one each.  It rides in
 * a spare HouseFlags bit, which is what keeps the savegame the length it was.
 *
 * Off by default.  Repairing costs credits at exactly the rate the button does,
 * and a base that quietly spends its income on its walls is not something to
 * hand a player without asking.
 */
bool Structure_AutoRepair_IsEnabled(uint8 houseID)
{
	House *h = House_Get_ByIndex(houseID);

	if (h == NULL) return false;

	return h->flags.autoRepair;
}

/**
 * Whether the House still has the building the switch lives on.
 *
 * structuresBuilt rather than a search: it is recomputed whenever a structure
 * is built, captured or destroyed, and it counts only what is on the map.
 */
bool Structure_AutoRepair_IsAvailable(uint8 houseID)
{
	House *h = House_Get_ByIndex(houseID);

	if (h == NULL) return false;

	return (h->structuresBuilt & FLAG_STRUCTURE_REPAIR) != 0;
}

/**
 * Turn auto repair on, off, or (state -1) over.
 *
 * @return The setting it ended on.
 */
bool Structure_AutoRepair_Set(uint8 houseID, int8 state)
{
	House *h = House_Get_ByIndex(houseID);

	if (h == NULL) return false;

	if (state == -1) state = h->flags.autoRepair ? 0 : 1;

	h->flags.autoRepair = (state != 0);

	if (houseID == g_playerHouseID) {
		GUI_DisplayText(String_Get_ByIndex(h->flags.autoRepair ? STR_AUTO_REPAIR_ON : STR_AUTO_REPAIR_OFF), 2);
	}

	return h->flags.autoRepair;
}

/**
 * Start the repair of one damaged building, if auto repair is on.
 *
 * Called once per structure tick from GameLoop_Structure(), which is also where
 * the repair itself is paid for and applied -- so this only ever sets the flag
 * the player's own button sets, and everything after it is the original game's.
 *
 * Two things it deliberately does not do.  It does not set `onHold` the way the
 * button does: the tick already refuses to produce while `repairing` is set, so
 * the flag would buy nothing and would be left standing when the credits run
 * out.  And it leaves a factory that is building something alone -- repairing
 * stops production, and stopping a factory is not what somebody who asked for
 * their turrets to be saved has asked for.  The button is still there for that
 * case.
 */
static void Structure_AutoRepair_Consider(Structure *s, House *h)
{
	const StructureInfo *si;

	if (s == NULL || h == NULL) return;
	if (!h->flags.autoRepair) return;
	if (!s->o.flags.s.allocated) return;
	if (s->o.flags.s.repairing || s->o.flags.s.upgrading) return;

	si = &g_table_structureInfo[s->o.type];

	if (s->o.hitpoints == 0 || s->o.hitpoints >= si->o.hitpoints) return;
	if ((h->structuresBuilt & FLAG_STRUCTURE_REPAIR) == 0) return;

	/* Busy means "has something inside it that is being worked on": a factory
	 * with an order, a Construction Yard with a building, a Repair facility
	 * with a vehicle in it. */
	if (s->countDown != 0 && s->o.linkedID != 0xFF) return;
	if (s->o.type == STRUCTURE_CONSTRUCTION_YARD && s->countDown != 0) return;

	s->o.flags.s.repairing = true;
}

/**
 * Auto repair, checked without a map: the rule is entirely about flags.
 *
 * The half this cannot reach is the repair itself -- the credits and the five
 * hitpoints a tick -- because that is the original game's code inside
 * GameLoop_Structure() and this changes none of it.  What is new is which
 * buildings get the flag set, so that is what is checked, one condition at a
 * time.
 */
int Structure_AutoRepair_RunRegressionTest(void)
{
	House *h;
	Structure s;
	int result = 1;
	uint16 labelWidth;
	uint16 buttonWidth = 0;
	uint16 i;

	h = House_Allocate(HOUSE_ORDOS);
	if (h == NULL) h = House_Get_ByIndex(HOUSE_ORDOS);
	if (h == NULL) return 0;

	/* Off is the default, and it has to survive a damaged base. */
	h->flags.autoRepair = false;
	h->structuresBuilt = FLAG_STRUCTURE_CONSTRUCTION_YARD | FLAG_STRUCTURE_REPAIR | FLAG_STRUCTURE_TURRET;

	memset(&s, 0, sizeof(s));
	s.o.type          = STRUCTURE_TURRET;
	s.o.houseID       = HOUSE_ORDOS;
	s.o.flags.s.allocated = true;
	s.o.linkedID      = 0xFF;
	s.o.hitpoints     = 10;

	Structure_AutoRepair_Consider(&s, h);
	if (s.o.flags.s.repairing) {
		printf("auto-repair: repaired with the switch off\n");
		result = 0;
	}

	/* On, and the turret repairs. */
	h->flags.autoRepair = true;
	Structure_AutoRepair_Consider(&s, h);
	if (!s.o.flags.s.repairing) {
		printf("auto-repair: a damaged turret was left alone\n");
		result = 0;
	}

	/* The switch is bought with the building it lives on: without a Repair
	 * facility standing it does nothing, and the setting itself is kept. */
	s.o.flags.s.repairing = false;
	h->structuresBuilt = FLAG_STRUCTURE_CONSTRUCTION_YARD | FLAG_STRUCTURE_TURRET;
	Structure_AutoRepair_Consider(&s, h);
	if (s.o.flags.s.repairing) {
		printf("auto-repair: repaired without a Repair facility\n");
		result = 0;
	}
	if (!Structure_AutoRepair_IsEnabled(HOUSE_ORDOS)) {
		printf("auto-repair: losing the facility cleared the setting\n");
		result = 0;
	}
	if (Structure_AutoRepair_IsAvailable(HOUSE_ORDOS)) {
		printf("auto-repair: available without a Repair facility\n");
		result = 0;
	}
	h->structuresBuilt |= FLAG_STRUCTURE_REPAIR;

	/* An undamaged building, a building being upgraded and a rubble heap are
	 * all left alone. */
	s.o.flags.s.repairing = false;
	s.o.hitpoints = g_table_structureInfo[STRUCTURE_TURRET].o.hitpoints;
	Structure_AutoRepair_Consider(&s, h);
	if (s.o.flags.s.repairing) {
		printf("auto-repair: repaired a building at full hitpoints\n");
		result = 0;
	}

	s.o.hitpoints = 10;
	s.o.flags.s.upgrading = true;
	Structure_AutoRepair_Consider(&s, h);
	if (s.o.flags.s.repairing) {
		printf("auto-repair: interrupted an upgrade\n");
		result = 0;
	}
	s.o.flags.s.upgrading = false;

	s.o.hitpoints = 0;
	Structure_AutoRepair_Consider(&s, h);
	if (s.o.flags.s.repairing) {
		printf("auto-repair: repaired a destroyed building\n");
		result = 0;
	}

	/* A factory with something inside it keeps building it.  Repairing stops
	 * production, and stopping a factory is not what this switch is for. */
	memset(&s, 0, sizeof(s));
	s.o.type          = STRUCTURE_HEAVY_VEHICLE;
	s.o.houseID       = HOUSE_ORDOS;
	s.o.flags.s.allocated = true;
	s.o.hitpoints     = 10;
	s.o.linkedID      = 3;
	s.countDown       = 100;

	Structure_AutoRepair_Consider(&s, h);
	if (s.o.flags.s.repairing) {
		printf("auto-repair: stopped a factory that was building something\n");
		result = 0;
	}

	s.o.linkedID = 0xFF;
	s.countDown  = 0;
	Structure_AutoRepair_Consider(&s, h);
	if (!s.o.flags.s.repairing) {
		printf("auto-repair: an idle factory was left damaged\n");
		result = 0;
	}

	/* The switch itself: explicit both ways, and a toggle in between. */
	if (Structure_AutoRepair_Set(HOUSE_ORDOS, 0) || Structure_AutoRepair_IsEnabled(HOUSE_ORDOS)) result = 0;
	if (!Structure_AutoRepair_Set(HOUSE_ORDOS, -1) || !Structure_AutoRepair_IsEnabled(HOUSE_ORDOS)) result = 0;
	if (Structure_AutoRepair_Set(HOUSE_ORDOS, -1) || Structure_AutoRepair_IsEnabled(HOUSE_ORDOS)) result = 0;
	if (!Structure_AutoRepair_Set(HOUSE_ORDOS, 1) || !Structure_AutoRepair_IsEnabled(HOUSE_ORDOS)) result = 0;
	Structure_AutoRepair_Set(HOUSE_ORDOS, 0);

	/* The label has to fit the button it is centred in, or it runs off the
	 * panel and into the screen edge -- there are 60 pixels there and no more. */
	for (i = 0; i < lengthof(g_table_gameWidgetInfo); i++) {
		if (g_table_gameWidgetInfo[i].stringID == STR_AUTO_REPAIR) buttonWidth = g_table_gameWidgetInfo[i].width;
	}
	{
		/* The panel draws its buttons through GUI_DrawText_Wrapper(..., 0x121):
		 * the 6p font, one pixel of kerning taken off each character.  Measured
		 * with anything else this check means nothing. */
		Font *font = g_fontCurrent;
		int8 offset = g_fontCharOffset;

		if (g_fontNew6p != NULL) Font_Select(g_fontNew6p);
		g_fontCharOffset = -1;
		labelWidth = Font_GetStringWidth(String_Get_ByIndex(STR_AUTO_REPAIR));
		g_fontCharOffset = offset;
		if (font != NULL) Font_Select(font);
	}
	if (buttonWidth == 0) {
		printf("auto-repair: no button carries the label\n");
		result = 0;
	} else if (labelWidth > buttonWidth) {
		printf("auto-repair: the label is %u pixels wide and the button is %u\n",
		       (unsigned)labelWidth, (unsigned)buttonWidth);
		result = 0;
	}

	return result;
}

/**
 * Update the map with the right data for this structure.
 * @param s The structure to update on the map.
 */
void Structure_UpdateMap(Structure *s)
{
	const StructureInfo *si;
	uint16 layoutSize;
	const uint16 *layout;
	uint16 *iconMap;
	int i;

	if (s == NULL) return;
	if (!s->o.flags.s.used) return;
	if (s->o.flags.s.isNotOnMap) return;

	si = &g_table_structureInfo[s->o.type];

	layout = g_table_structure_layoutTiles[si->layout];
	layoutSize = g_table_structure_layoutTileCount[si->layout];

	iconMap = &g_iconMap[g_iconMap[si->iconGroup] + layoutSize + layoutSize];

	for (i = 0; i < layoutSize; i++) {
		uint16 position;
		Tile *t;

		position = Tile_PackTile(s->o.position) + layout[i];

		t = &g_map[position];
		t->houseID = s->o.houseID;
		t->hasStructure = true;
		t->index = s->o.index + 1;

		t->groundTileID = iconMap[i] + s->rotationSpriteDiff;

		if (Tile_IsUnveiled(t->overlayTileID)) t->overlayTileID = 0;

		Map_Update(position, 0, false);
	}

	s->o.flags.s.isDirty = true;

	Structure_StartAnimation(s);
}

/**
 * Mark a structure's tiles for redrawing, without writing them.
 *
 * A full-screen interface repaint ends by asking every structure to put itself
 * back on the map, and that is one client's screen deciding to write the world:
 * the tiles it stamps are the structure's *idle* frame, so a building whose
 * animation had reached the next frame is dragged back a step -- on the client
 * that repainted and not on the other.  The animation slots stay in agreement,
 * which is what made it hard to see: the checksum reported the map, and the
 * cause was a window closing.
 *
 * Nothing needs writing.  The tiles in g_map are already what the simulation
 * says they are -- the repaint lost the screen, not the map -- so the redraw
 * only has to say which tiles to paint again.  An earlier attempt at this kept
 * the tile stamping and dropped only the animation restart, which fixed the
 * random stream and left this behind.
 */
void Structure_RedrawMap(Structure *s)
{
	const StructureInfo *si;
	uint16 layoutSize;
	const uint16 *layout;
	uint16 packed;
	int i;

	if (s == NULL) return;
	if (!s->o.flags.s.used) return;
	if (s->o.flags.s.isNotOnMap) return;

	si = &g_table_structureInfo[s->o.type];

	layout = g_table_structure_layoutTiles[si->layout];
	layoutSize = g_table_structure_layoutTileCount[si->layout];
	packed = Tile_PackTile(s->o.position);

	for (i = 0; i < layoutSize; i++) {
		Map_Update(packed + layout[i], 0, false);
	}

	s->o.flags.s.isDirty = true;
}

void Structure_StartAnimation(Structure *s)
{
	const StructureInfo *si;

	if (s == NULL) return;

	si = &g_table_structureInfo[s->o.type];

	if (s->state >= STRUCTURE_STATE_IDLE) {
		uint16 animationIndex = (s->state > STRUCTURE_STATE_READY) ? STRUCTURE_STATE_READY : s->state;

		if (si->animationIndex[animationIndex] == 0xFF) {
			Animation_Start(NULL, s->o.position, si->layout, s->o.houseID, (uint8)si->iconGroup);
		} else {
			uint8 animationID = si->animationIndex[animationIndex];

			assert(animationID < 29);
			Animation_Start(g_table_animation_structure[animationID], s->o.position, si->layout, s->o.houseID, (uint8)si->iconGroup);
		}
	} else {
		Animation_Start(g_table_animation_structure[1], s->o.position, si->layout, s->o.houseID, (uint8)si->iconGroup);
	}
}

uint32 Structure_GetBuildable(Structure *s)
{
	const StructureInfo *si;
	uint32 structuresBuilt;
	uint32 ret = 0;
	int i;

	if (s == NULL) return 0;

	si = &g_table_structureInfo[s->o.type];

	structuresBuilt = House_Get_ByIndex(s->o.houseID)->structuresBuilt;

	switch (s->o.type) {
		case STRUCTURE_LIGHT_VEHICLE:
		case STRUCTURE_HEAVY_VEHICLE:
		case STRUCTURE_HIGH_TECH:
		case STRUCTURE_WOR_TROOPER:
		case STRUCTURE_BARRACKS:
			for (i = 0; i < UNIT_MAX; i++) {
				g_table_unitInfo[i].o.available = 0;
			}

			for (i = 0; i < 8; i++) {
				UnitInfo *ui;
				uint16 upgradeLevelRequired;
				uint8 unitType = si->buildableUnits[i];

				if (unitType == UNIT_INVALID) continue;

				if (unitType == UNIT_TRIKE && s->creatorHouseID == HOUSE_ORDOS) unitType = UNIT_RAIDER_TRIKE;

				ui = &g_table_unitInfo[unitType];
				upgradeLevelRequired = ui->o.upgradeLevelRequired;

				if (unitType == UNIT_SIEGE_TANK && s->creatorHouseID == HOUSE_ORDOS) upgradeLevelRequired--;

				if ((structuresBuilt & ui->o.structuresRequired) != ui->o.structuresRequired) continue;
				if ((ui->o.availableHouse & (1 << s->creatorHouseID)) == 0) continue;

				if (s->upgradeLevel >= upgradeLevelRequired) {
					ui->o.available = 1;

					ret |= (1 << unitType);
					continue;
				}

				if (s->upgradeTimeLeft != 0 && s->upgradeLevel + 1 >= upgradeLevelRequired) {
					ui->o.available = -1;
				}
			}
			return ret;

		case STRUCTURE_CONSTRUCTION_YARD:
			for (i = 0; i < STRUCTURE_MAX; i++) {
				StructureInfo *localsi = &g_table_structureInfo[i];
				uint16 availableCampaign;
				uint32 structuresRequired;

				localsi->o.available = 0;

				availableCampaign = localsi->o.availableCampaign;
				structuresRequired = localsi->o.structuresRequired;

				if (i == STRUCTURE_WOR_TROOPER && s->o.houseID == HOUSE_HARKONNEN && g_campaignID >= 1) {
					structuresRequired &= ~(1 << STRUCTURE_BARRACKS);
					availableCampaign = 2;
				}

				/* The prerequisite and upgrade rules are the player's; the AI is
				 * waived from both.  Asking g_playerHouseID which is which makes
				 * the answer depend on who is watching -- so a house that is
				 * played is held to the rules on every client, rather than on
				 * the one client whose chair it happens to be. */
				if ((structuresBuilt & structuresRequired) == structuresRequired || !Match_IsHumanControlled(s->o.houseID)) {
					if (s->o.houseID != HOUSE_HARKONNEN && i == STRUCTURE_LIGHT_VEHICLE) {
						availableCampaign = 2;
					}

					if (g_campaignID >= availableCampaign - 1 && (localsi->o.availableHouse & (1 << s->o.houseID)) != 0) {
						if (s->upgradeLevel >= localsi->o.upgradeLevelRequired || !Match_IsHumanControlled(s->o.houseID)) {
							localsi->o.available = 1;

							ret |= (1 << i);
						} else if (s->upgradeTimeLeft != 0 && s->upgradeLevel + 1 >= localsi->o.upgradeLevelRequired) {
							localsi->o.available = -1;
						}
					}
				}
			}
			return ret;

		case STRUCTURE_STARPORT:
			return -1;

		default:
			return 0;
	}
}

/**
 * The house is under attack in the form of a structure being hit.
 * @param houseID The house who is being attacked.
 */
void Structure_HouseUnderAttack(uint8 houseID)
{
	PoolFindStruct find;
	House *h;

	h = House_Get_ByIndex(houseID);

	if (!Match_IsHumanControlled(houseID) && h->flags.doneFullScaleAttack) return;
	h->flags.doneFullScaleAttack = true;

	if (h->flags.human) {
		if (h->timerStructureAttack != 0) return;

		Sound_Output_Feedback(48);

		h->timerStructureAttack = 8;
		return;
	}

	/* ENHANCEMENT -- Dune2 originally only searches for units with type 0 (Carry-all). In result, the rest of this function does nothing. */
	if (!g_dune2_enhanced) return;

	find.houseID = houseID;
	find.index   = 0xFFFF;
	find.type    = 0xFFFF;

	while (true) {
		const UnitInfo *ui;
		Unit *u;

		u = Unit_Find(&find);
		if (u == NULL) break;

		ui = &g_table_unitInfo[u->o.type];

		if (ui->bulletType == UNIT_INVALID) continue;

		/* XXX -- Dune2 does something odd here. What was their intention? */
		if ((u->actionID == ACTION_GUARD && u->actionID == ACTION_AMBUSH) || u->actionID == ACTION_AREA_GUARD) Unit_SetAction(u, ACTION_HUNT);
	}
}

/**
 * Find the next object to build.
 * @param s The structure in which we can build something.
 * @return The type (either UnitType or StructureType) of what we should build next.
 */
uint16 Structure_AI_PickNextToBuild(Structure *s)
{
	PoolFindStruct find;
	/* Structure_GetBuildable() returns a 32 bit mask; as a uint16 this
	 * silently dropped every structure type above 15, which is why the AI
	 * never picked an Outpost, Silo or Rocket Turret. */
	uint32 buildable;
	uint16 type;
	House *h;
	int i;

	if (s == NULL) return 0xFFFF;

	h = House_Get_ByIndex(s->o.houseID);
	buildable = Structure_GetBuildable(s);

	if (s->o.type == STRUCTURE_CONSTRUCTION_YARD) {
		for (i = 0; i < 5; i++) {
			type = h->ai_structureRebuild[i][0];

			if (type == 0) continue;
			if ((buildable & (1 << type)) == 0) continue;

			return type;
		}

		/* Rebuilding lost structures comes first; with nothing to rebuild, a
		 * skirmish AI works its way through its base plan instead. */
		if (Skirmish_IsActive()) {
			type = Skirmish_Plan_PickNext(h);

			if (type != 0xFFFF && (buildable & (1 << type)) != 0) return type;
		}

		return 0xFFFF;
	}

	if (s->o.type == STRUCTURE_HIGH_TECH) {
		/* One carryall is all the stock AI ever keeps; an economy plan may ask
		 * for more, and they are what makes distant spice worth mining. */
		if (Skirmish_AI_WantsCarryall(h) && (buildable & FLAG_UNIT_CARRYALL) != 0) return UNIT_CARRYALL;

		find.houseID = s->o.houseID;
		find.index   = 0xFFFF;
		find.type    = UNIT_CARRYALL;

		while (true) {
			Unit *u;

			u = Unit_Find(&find);
			if (u == NULL) break;

			buildable &= ~FLAG_UNIT_CARRYALL;
		}
	}

	/* The Starport is a factory as far as the AI loop is concerned, but nothing
	 * is built there -- it is shopped at. */
	if (s->o.type == STRUCTURE_STARPORT) {
		Skirmish_AI_StarportOrder(h, s);
		return 0xFFFF;
	}

	if (s->o.type == STRUCTURE_HEAVY_VEHICLE) {
		/* A skirmish AI has to grow its own economy; the campaign AI is fed
		 * harvesters by its scenario and keeps the original behaviour. */
		if (Skirmish_AI_FactoryWantsHarvester(h, s) && (buildable & FLAG_UNIT_HARVESTER) != 0) return UNIT_HARVESTER;

		buildable &= ~FLAG_UNIT_HARVESTER;
		buildable &= ~FLAG_UNIT_MCV;
	}

	/* A skirmish house builds an army, not a stack of whatever scores highest.
	 * The loop below is the original rule -- take the maximum of priorityBuild --
	 * which produces exactly one unit type per factory forever. */
	type = Skirmish_IsActive() ? Skirmish_AI_PickUnit(h, buildable) : 0xFFFF;

	if (type == 0xFFFF) {
		for (i = 0; i < UNIT_MAX; i++) {
			if ((buildable & (1 << i)) == 0) continue;

			if ((Tools_Random_256() % 4) == 0) type = i;

			if (type != 0xFFFF) {
				if (g_table_unitInfo[i].o.priorityBuild <= g_table_unitInfo[type].o.priorityBuild) continue;
			}

			type = i;
		}
	}

	if (!Skirmish_AI_AllowUnit(h, type)) return 0xFFFF;

	return type;
}
