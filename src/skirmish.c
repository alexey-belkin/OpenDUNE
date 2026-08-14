/** @file src/skirmish.c %Skirmish (AI vs AI) game setup and base build plans.
 *
 * A skirmish is not a scenario: there is no INI file, the landscape is
 * generated from a random seed and both bases are laid out here.  The human is
 * a spectator (HOUSE_MERCENARY, no assets), which matters more than it looks:
 * nearly every AI branch in the engine is written as
 * "houseID != g_playerHouseID", so an AI house may never be the player's.
 *
 * Only the Construction Yard is placed for real.  Everything else is handed to
 * the AI as a *plan*: an ordered list of (type, position) the Construction Yard
 * works through, which is the same mechanism the engine already uses to rebuild
 * destroyed structures (House.ai_structureRebuild), only longer and with the
 * tech tree actually enforced -- see Skirmish_Plan_PickNext().
 */

#include <stdio.h>
#include <string.h>
#include "types.h"
#include "os/common.h"
#include "os/math.h"

#include "skirmish.h"

#include "audio/sound.h"
#include "gui/gui.h"
#include "house.h"
#include "map.h"
#include "opendune.h"
#include "pool/house.h"
#include "pool/pool.h"
#include "pool/structure.h"
#include "pool/team.h"
#include "pool/unit.h"
#include "scenario.h"
#include "sprites.h"
#include "structure.h"
#include "team.h"
#include "tile.h"
#include "timer.h"
#include "tools.h"
#include "unit.h"

/** Credits every skirmish house starts with. */
#define SKIRMISH_START_CREDITS 1500
/** Most carryalls the adaptive rule will ask for. */
#define SKIRMISH_CARRYALL_MAX 6
/** Harvesters on the map before the first carryall is worth building. */
#define SKIRMISH_CARRYALL_FIRST 3
/** How often the refinery queue is looked at, in game ticks. */
#define SKIRMISH_QUEUE_SAMPLE_TICKS 60
/** Harvesters a skirmish AI keeps trying to reach. */
#define SKIRMISH_HARVESTER_TARGET 3
/** Unit cap per skirmish house. */
#define SKIRMISH_UNIT_MAX 25
/** Unit cap per house in a war: two of these plus frigates must fit the 102 slot pool. */
#define SKIRMISH_WAR_UNIT_MAX 40

/* The economy settled on by the search -- see economy.md.  A war strategy varies
 * how much of what this earns goes into the army, not the economy itself. */
#define SKIRMISH_WAR_HARVESTERS 16
#define SKIRMISH_WAR_REFINERY_WAIT 5
#define SKIRMISH_WAR_CARRYALL_WAIT 1200
/* The split settled on by the war search -- see war.md.  Nothing on the army
 * until the economy is standing, then most of the income: the switch point is
 * what matters, and it is absolute (when the base is built) rather than a
 * fraction of the match. */
#define SKIRMISH_WAR_SHARE 0
#define SKIRMISH_WAR_SHARE_LATE 90
#define SKIRMISH_WAR_SWITCH_TICK 35000
/** Campaign the skirmish pretends to be, so the full tech tree is available. */
#define SKIRMISH_CAMPAIGN 8

/** Size of the rock plateau carved out for one base. */
#define SKIRMISH_BASE_WIDTH  20
#define SKIRMISH_BASE_HEIGHT 16

typedef struct SkirmishPlanEntry {
	uint8  type;                                            /*!< StructureType to build. */
	bool   taken;                                           /*!< The Construction Yard has already been given this entry. */
	uint16 position;                                        /*!< Packed tile the structure is planned for. */
} SkirmishPlanEntry;

typedef struct SkirmishBase {
	uint8  houseID;                                         /*!< House owning this base. */
	uint16 creditsNoSilo;                                   /*!< Starting credits the house may hold without spice storage. */
	uint16 origin;                                          /*!< Packed tile of the Construction Yard. */
	uint16 entryCount;
	SkirmishPlanEntry entries[SKIRMISH_PLAN_MAX];

	uint16 historyCount;                                    /*!< Structures placed so far. */
	uint8  history[SKIRMISH_PLAN_MAX];                      /*!< The order they were placed in, which is what we are here to judge. */

	SkirmishEconomyPlan plan;                               /*!< The strategy this house is playing. */

	/* The war ledger.  Every credit a factory spends is booked to one of these,
	 * and the military share is enforced against the first -- see
	 * Skirmish_War_MilitaryAllowed(). */
	uint32 militarySpent;
	uint32 economySpent;

	/* Layout cursor, only used while building the plan. */
	uint16 rectX, rectY;
	uint16 cursorX, cursorY;
	uint16 rowHeight;
} SkirmishBase;

static bool s_active = false;
static SkirmishBase s_bases[SKIRMISH_PLAYER_MAX];

/* Economy mode: one house, no enemy, no combat units.  The plan comes from the
 * search instead of from s_blueprint, and the only thing that counts is how much
 * spice the house gets out of the ground. */
static bool s_economyMode = false;
/* War mode: two houses, both running a tuned economy, both allowed to spend a
 * share of it on an army.  What is being searched here is that share. */
static bool s_warMode = false;
static uint32 s_harvested[HOUSE_MAX];
static uint32 s_refineryLoad[STRUCTURE_INDEX_MAX_SOFT];
static uint16 s_refineryWait[HOUSE_MAX];
static uint32 s_nextQueueSample[HOUSE_MAX];
static uint16 s_carryallTarget[HOUSE_MAX];
static uint32 s_tripStart[UNIT_INDEX_MAX];
static uint16 s_starportBought[HOUSE_MAX][2];               /*!< [0] harvesters, [1] carryalls. */

/**
 * The base build order.  It is deliberately prerequisite-consistent top to
 * bottom, but Skirmish_Plan_PickNext() still verifies every entry: the engine's
 * own Structure_GetBuildable() skips the tech tree entirely for AI houses, so
 * the order below is a preference, not a guarantee.
 */
static const uint8 s_blueprint[] = {
	STRUCTURE_WINDTRAP,
	STRUCTURE_REFINERY,
	STRUCTURE_OUTPOST,
	STRUCTURE_WINDTRAP,
	STRUCTURE_LIGHT_VEHICLE,
	STRUCTURE_HEAVY_VEHICLE,
	STRUCTURE_BARRACKS,     /* Skipped for Houses which cannot build it. */
	STRUCTURE_WOR_TROOPER,  /* Idem. */
	STRUCTURE_WINDTRAP,
	STRUCTURE_TURRET,
	STRUCTURE_TURRET,
	STRUCTURE_SILO,
	STRUCTURE_HIGH_TECH,
	STRUCTURE_WINDTRAP,
	STRUCTURE_REPAIR,
	STRUCTURE_STARPORT,
	STRUCTURE_WINDTRAP,
	STRUCTURE_ROCKET_TURRET,
	STRUCTURE_ROCKET_TURRET,
	STRUCTURE_HOUSE_OF_IX,
	STRUCTURE_PALACE
};

/**
 * Which side of the ledger a structure falls on.
 *
 * The dividing line is what the building is *for*, not who ends up using it: a
 * Heavy Factory is booked to the economy because harvesters come out of it, a
 * Barracks is war because nothing else does.  Turrets are war too -- defence is
 * military spending, it just does not move.
 */
static bool Skirmish_IsMilitaryStructure(uint8 type)
{
	switch (type) {
		case STRUCTURE_BARRACKS:
		case STRUCTURE_WOR_TROOPER:
		case STRUCTURE_TURRET:
		case STRUCTURE_ROCKET_TURRET:
		case STRUCTURE_REPAIR:
		case STRUCTURE_HOUSE_OF_IX:
		case STRUCTURE_PALACE:
			return true;

		default:
			return false;
	}
}

/** Everything that does not mine, ferry or deploy is a war expense. */
static bool Skirmish_IsMilitaryUnit(uint16 type)
{
	return (type != UNIT_HARVESTER && type != UNIT_CARRYALL && type != UNIT_MCV);
}

/**
 * The attack groups every skirmish House keeps.  Two tracked teams because
 * tanks are what a base ends up producing most of; the sizes are in the same
 * range the campaign scenarios use.
 */
static const struct {
	uint8  movementType;
	uint16 minMembers;
	uint16 maxMembers;
} s_teamPlan[] = {
	{ MOVEMENT_FOOT,    4, 8 },
	{ MOVEMENT_WHEELED, 2, 4 },
	{ MOVEMENT_TRACKED, 3, 6 },
	{ MOVEMENT_TRACKED, 3, 6 }
};

bool Skirmish_IsActive(void)
{
	return s_active;
}

bool Skirmish_IsEconomyMode(void)
{
	return s_active && s_economyMode;
}

void Skirmish_Economy_AddHarvested(uint8 houseID, uint16 credits, uint16 structureIndex)
{
	if (!s_active || houseID >= HOUSE_MAX) return;

	s_harvested[houseID] += credits;

	/* Per refinery, so "is the second refinery ever used?" is a measurement
	 * rather than an argument. */
	if (structureIndex < lengthof(s_refineryLoad)) s_refineryLoad[structureIndex] += credits;
}

/**
 * What each refinery unloaded, in the order the structures were built.  An idle
 * refinery shows up as a zero.
 */
bool Skirmish_GetRefineryLoad(char *buf, uint16 length)
{
	PoolFindStruct find;
	uint16 used = 0;

	if (!s_active || buf == NULL || length == 0) return false;

	buf[0] = '\0';

	find.houseID = HOUSE_INVALID;
	find.index   = 0xFFFF;
	find.type    = STRUCTURE_REFINERY;

	while (used + 24 < length) {
		const Structure *s = Structure_Find(&find);
		int written;

		if (s == NULL) break;

		written = snprintf(buf + used, length - used, "%srefinery#%u %u",
		                   (used == 0) ? "" : ", ", s->o.index,
		                   (unsigned)((s->o.index < lengthof(s_refineryLoad)) ? s_refineryLoad[s->o.index] : 0));

		if (written <= 0) break;
		used += (uint16)written;
	}

	return true;
}

uint32 Skirmish_Economy_GetHarvested(uint8 houseID)
{
	if (houseID >= HOUSE_MAX) return 0;

	return s_harvested[houseID];
}

void Skirmish_Reset(void)
{
	s_active = false;
	s_economyMode = false;
	s_warMode = false;
	memset(s_bases, 0, sizeof(s_bases));
	memset(s_harvested, 0, sizeof(s_harvested));
	memset(s_refineryLoad, 0, sizeof(s_refineryLoad));
	memset(s_refineryWait, 0, sizeof(s_refineryWait));
	memset(s_nextQueueSample, 0, sizeof(s_nextQueueSample));
	memset(s_carryallTarget, 0, sizeof(s_carryallTarget));
	memset(s_tripStart, 0, sizeof(s_tripStart));
	memset(s_starportBought, 0, sizeof(s_starportBought));
}

static SkirmishBase *Skirmish_GetBase(uint8 houseID)
{
	uint8 i;

	if (!s_active) return NULL;

	for (i = 0; i < SKIRMISH_PLAYER_MAX; i++) {
		if (s_bases[i].entryCount != 0 && s_bases[i].houseID == houseID) return &s_bases[i];
	}

	return NULL;
}

/**
 * Whether this house may still spend on the war.
 *
 * The split between economy and army is not a build order, it is a budget: of
 * every credit the house has ever had -- its starting capital plus everything it
 * has refined since -- at most militaryShare percent may end up in soldiers,
 * tanks and turrets.  Everything else has nowhere to go but the economy, so one
 * number decides the whole balance, and a strategy that overspends early simply
 * has no army budget left when the enemy arrives.
 *
 * The share is allowed to change once, which is what makes "boom then arm" and
 * "rush then expand" expressible as the same kind of plan.
 */
static bool Skirmish_War_MilitaryAllowed(const SkirmishBase *b)
{
	uint32 income;
	uint32 allowance;
	uint16 share;

	if (!s_warMode) return !s_economyMode;

	share = (g_timerGame - g_tickScenarioStart >= b->plan.militarySwitchTick)
		? b->plan.militaryShareLate : b->plan.militaryShare;

	if (share == 0) return false;
	if (share >= 100) return true;

	income    = SKIRMISH_START_CREDITS + s_harvested[b->houseID];
	allowance = income * share / 100;

	return (b->militarySpent < allowance);
}

/**
 * Book a credit a factory just spent to the side of the ledger it belongs to.
 *
 * Called from the one place in the engine where production is paid for, credit
 * by credit as the countdown runs, so a half-finished tank is half-charged --
 * which is what makes the share behave like a spending rate rather than a
 * checkout gate.
 */
void Skirmish_War_Charge(const Structure *s, uint16 credits)
{
	SkirmishBase *b;
	bool military;

	if (!s_active || s == NULL || credits == 0) return;

	b = Skirmish_GetBase((uint8)s->o.houseID);
	if (b == NULL) return;

	if (s->o.type == STRUCTURE_CONSTRUCTION_YARD) {
		military = Skirmish_IsMilitaryStructure((uint8)s->objectType);
	} else if (s->o.type == STRUCTURE_REPAIR) {
		military = true;
	} else {
		military = Skirmish_IsMilitaryUnit(s->objectType);
	}

	if (military) {
		b->militarySpent += credits;
	} else {
		b->economySpent += credits;
	}
}

uint32 Skirmish_War_GetSpent(uint8 index, bool military)
{
	if (index >= SKIRMISH_PLAYER_MAX) return 0;

	return military ? s_bases[index].militarySpent : s_bases[index].economySpent;
}

/**
 * Every credit this house has ever had: its starting capital plus everything it
 * has refined.  This is what the military share is a share *of*, so it is also
 * the only denominator that says whether the share was reached.
 */
uint32 Skirmish_War_GetIncome(uint8 index)
{
	if (index >= SKIRMISH_PLAYER_MAX || s_bases[index].entryCount == 0) return 0;

	return SKIRMISH_START_CREDITS + s_harvested[s_bases[index].houseID];
}

/**
 * Whether a tile counts as rock for the purpose of picking the right rock
 * sprite. Mountains count too, exactly like Map_CreateLandscape() does.
 */
static bool Skirmish_IsRockNeighbour(uint16 x, uint16 y, const SkirmishBase *b)
{
	uint16 lst;

	if (x >= 64 || y >= 64) return false;

	if (x >= b->rectX && x < b->rectX + SKIRMISH_BASE_WIDTH &&
	    y >= b->rectY && y < b->rectY + SKIRMISH_BASE_HEIGHT) return true;

	lst = Map_GetLandscapeType(Tile_PackXY(x, y));

	return (lst == LST_ENTIRELY_ROCK || lst == LST_MOSTLY_ROCK || lst == LST_PARTIAL_ROCK ||
	        lst == LST_ENTIRELY_MOUNTAIN || lst == LST_PARTIAL_MOUNTAIN);
}

/**
 * Turn the base rectangle into solid rock.  Structures may be built on sand,
 * but they degrade there, which would make every skirmish a race against
 * erosion instead of a test of the build order.
 */
static void Skirmish_CarveRock(const SkirmishBase *b)
{
	uint16 x, y;

	for (y = b->rectY; y < b->rectY + SKIRMISH_BASE_HEIGHT; y++) {
		for (x = b->rectX; x < b->rectX + SKIRMISH_BASE_WIDTH; x++) {
			const uint16 packed = Tile_PackXY(x, y);
			uint16 spriteID = 0;

			/* Same neighbour encoding as the landscape generator: up, right,
			 * down, left; sprite 0 of the rock group is the isolated tile. */
			if (Skirmish_IsRockNeighbour(x, y - 1, b)) spriteID |= 1;
			if (Skirmish_IsRockNeighbour(x + 1, y, b)) spriteID |= 2;
			if (Skirmish_IsRockNeighbour(x, y + 1, b)) spriteID |= 4;
			if (Skirmish_IsRockNeighbour(x - 1, y, b)) spriteID |= 8;
			spriteID++;

			spriteID = g_iconMap[g_iconMap[ICM_ICONGROUP_LANDSCAPE] + spriteID] & 0x1FF;

			g_map[packed].groundTileID = spriteID;
			g_mapTileID[packed] = spriteID;
		}
	}
}

/**
 * Reserve room for one structure inside the base rectangle, packing rows left
 * to right with a one tile gap so units can still move through the base.
 * @return The packed top-left tile, or 0xFFFF when the base is full.
 */
static uint16 Skirmish_Layout_Next(SkirmishBase *b, uint8 type)
{
	const StructureInfo *si = &g_table_structureInfo[type];
	const XYSize *size = &g_table_structure_layoutSize[si->layout];
	uint16 packed;

	if (b->cursorX + size->width > b->rectX + SKIRMISH_BASE_WIDTH) {
		b->cursorX  = b->rectX;
		b->cursorY += b->rowHeight + 1;
		b->rowHeight = 0;
	}

	if (b->cursorY + size->height > b->rectY + SKIRMISH_BASE_HEIGHT) return 0xFFFF;

	packed = Tile_PackXY(b->cursorX, b->cursorY);

	b->cursorX += size->width + 1;
	b->rowHeight = max(b->rowHeight, size->height);

	return packed;
}

/**
 * Pour concrete over the footprint of a planned structure and bill the House
 * for it.
 *
 * A structure standing on bare ground is placed with a hitpoint penalty and the
 * degrades flag, so a base without concrete slowly falls apart.  The AI has no
 * notion of laying slabs -- it only ever orders whole structures -- so the plan
 * pours them itself, right before the building lands, which is also when it is
 * visible on screen.
 */
static void Skirmish_LaySlabs(House *h, uint16 position, uint8 structureType)
{
	const StructureInfo *si = &g_table_structureInfo[structureType];
	const uint16 cost = g_table_structureInfo[STRUCTURE_SLAB_1x1].o.buildCredits;
	uint16 i;

	if (h == NULL) return;

	for (i = 0; i < g_table_structure_layoutTileCount[si->layout]; i++) {
		const uint16 packed = position + g_table_structure_layoutTiles[si->layout][i];
		Tile *t = &g_map[packed];

		if (Tile_IsOutOfMap(packed)) continue;
		if (Map_GetLandscapeType(packed) == LST_CONCRETE_SLAB) continue;

		t->groundTileID = g_builtSlabTileID;
		t->houseID      = h->index;

		g_mapTileID[packed] |= 0x8000;

		if (Map_IsPositionUnveiled(packed)) t->overlayTileID = 0;

		Map_Update(packed, 0, false);

		h->credits -= min(h->credits, cost);
	}
}

static void Skirmish_Plan_Create(SkirmishBase *b)
{
	uint8 i;

	b->cursorX   = b->rectX;
	b->cursorY   = b->rectY;
	b->rowHeight = 0;

	/* The Construction Yard is the only structure that exists from the start;
	 * it takes the first slot so the rest of the base grows around it. */
	b->origin = Skirmish_Layout_Next(b, STRUCTURE_CONSTRUCTION_YARD);

	/* Every mode runs exactly the plan it was handed.  No spare Windtraps behind
	 * it: the plan compiles power in already, and a Windtrap nobody needs is
	 * 300 credits the plan is not being charged for. */
	for (i = 0; i < b->plan.buildCount; i++) {
		const uint8 type = b->plan.build[i];
		uint16 position;

		if (b->entryCount == SKIRMISH_PLAN_MAX) break;
		if (type >= STRUCTURE_MAX) continue;
		if ((g_table_structureInfo[type].o.availableHouse & (1 << b->houseID)) == 0) continue;

		position = Skirmish_Layout_Next(b, type);
		if (position == 0xFFFF) break;

		b->entries[b->entryCount].type     = type;
		b->entries[b->entryCount].taken    = false;
		b->entries[b->entryCount].position = position;
		b->entryCount++;
	}
}

/**
 * The default strategy: the tuned economy, and a middling army budget.
 *
 * This is what a plain --skirmish plays and what the war search starts from, so
 * everything that varies in a search varies against it.
 */
void Skirmish_MakeDefaultPlan(SkirmishEconomyPlan *plan)
{
	uint16 i;

	memset(plan, 0, sizeof(*plan));

	for (i = 0; i < lengthof(s_blueprint) && i < SKIRMISH_PLAN_MAX; i++) {
		plan->build[plan->buildCount++] = s_blueprint[i];
	}

	plan->harvesterTarget    = SKIRMISH_WAR_HARVESTERS;
	plan->refineryWait       = SKIRMISH_WAR_REFINERY_WAIT;
	plan->carryallWait       = SKIRMISH_WAR_CARRYALL_WAIT;
	plan->militaryShare      = SKIRMISH_WAR_SHARE;
	plan->militaryShareLate  = SKIRMISH_WAR_SHARE_LATE;
	plan->militarySwitchTick = SKIRMISH_WAR_SWITCH_TICK;
}

/**
 * Find the next structure of the plan the House can actually start now.
 *
 * Unlike the stock AI this honours structuresRequired, so what shows up on the
 * map is the order this function chose, not whatever the plan happened to list
 * first.  Two rules override plan order:
 *  - power first, because a house in deficit loses hitpoints on everything;
 *  - skip what we cannot pay for when something cheaper is available, or the
 *    whole queue stalls behind one expensive entry.
 *
 * @param h The House whose Construction Yard is asking.
 * @return The StructureType to build, or 0xFFFF when there is nothing to do.
 */
/**
 * Add another refinery when harvesters are queueing for the one there is.
 *
 * A fixed plan has to guess how many refineries the game will end up needing,
 * and the answer changes with the length of the match: at eight harvesters one
 * refinery is idle most of the time, at thirty it is the queue.  So do not
 * guess.  Count the moments when a full harvester has nowhere to unload, and
 * once that has happened often enough, put another refinery in the plan.
 *
 * @param b The base to extend.
 * @param h Its house.
 */
static bool Skirmish_Economy_RefineryFree(const House *h);
static bool Skirmish_Plan_Append(SkirmishBase *b, House *h, uint8 type);
static void Skirmish_Economy_SampleCarryalls(SkirmishBase *b, House *h, bool refineryFree);

static void Skirmish_Economy_SampleQueue(SkirmishBase *b, House *h)
{
	PoolFindStruct find;
	bool refineryFree = false;
	bool harvesterWaiting = false;
	uint16 position;

	if (b->plan.refineryWait == 0 && b->plan.carryallWait == 0) return;

	/* Own cadence.  Hanging this off "the Construction Yard is asking what to
	 * build" sampled a few times a minute -- the yard is usually busy -- and the
	 * queue could sit there for the whole match without the rule ever seeing it. */
	if (g_timerGame < s_nextQueueSample[h->index]) return;
	s_nextQueueSample[h->index] = g_timerGame + SKIRMISH_QUEUE_SAMPLE_TICKS;

	if (b->plan.refineryWait == 0 || b->entryCount >= SKIRMISH_PLAN_MAX) {
		/* The carryall rule still wants to know whether a refinery was free. */
		refineryFree = Skirmish_Economy_RefineryFree(h);
		Skirmish_Economy_SampleCarryalls(b, h, refineryFree);
		return;
	}

	/* One at a time.  A queue does not clear until the new refinery is standing,
	 * so without this the rule keeps reacting to the same queue and buries the
	 * economy in refineries it cannot pay for -- measured as a collapse from
	 * 150000 refined spice to 1500. */
	for (position = 0; position < b->entryCount; position++) {
		if (b->entries[position].type != STRUCTURE_REFINERY) continue;
		if (b->entries[position].taken) continue;

		return;
	}

	refineryFree = Skirmish_Economy_RefineryFree(h);

	Skirmish_Economy_SampleCarryalls(b, h, refineryFree);

	if (refineryFree) return;

	find.houseID = h->index;
	find.index   = 0xFFFF;
	find.type    = UNIT_HARVESTER;

	while (true) {
		const Unit *u = Unit_Find(&find);

		if (u == NULL) break;
		if (u->o.flags.s.isNotOnMap) continue;   /* Already inside a refinery. */
		if (u->amount == 0) continue;            /* Nothing to unload. */
		/* Full, or on its way home with a load: either way it is a harvester
		 * that wants a refinery and cannot have one.  Insisting on a full 100
		 * missed most of the queue -- a harvester sent home early by the
		 * recovery layer carries whatever it had. */
		if (u->amount < 100 && u->actionID != ACTION_RETURN) continue;

		harvesterWaiting = true;
		break;
	}

	if (!harvesterWaiting) {
		if (s_refineryWait[h->index] != 0) s_refineryWait[h->index]--;
		return;
	}

	if (++s_refineryWait[h->index] < b->plan.refineryWait) return;

	s_refineryWait[h->index] = 0;

	/* Through Plan_Append, which pays for the power first.  A refinery draws 30,
	 * and a house in deficit runs every structure at half hitpoints -- including
	 * the refineries, whose unload rate is computed from their hitpoints.  Adding
	 * refineries without windtraps halves the economy instead of growing it, and
	 * it showed up as runs that scored no better than the rule being off. */
	Skirmish_Plan_Append(b, h, STRUCTURE_REFINERY);
}

uint16 Skirmish_Plan_PickNext(House *h)
{
	SkirmishBase *b;
	uint16 fallback = 0xFFFF;
	uint16 i;

	if (h == NULL) return 0xFFFF;

	b = Skirmish_GetBase((uint8)h->index);
	if (b == NULL) return 0xFFFF;

	for (i = 0; i < b->entryCount; i++) {
		const SkirmishPlanEntry *e = &b->entries[i];
		const StructureInfo *si = &g_table_structureInfo[e->type];

		if (e->taken) continue;
		if ((h->structuresBuilt & si->o.structuresRequired) != si->o.structuresRequired) continue;

		/* A windtrap is worth jumping the queue for: without power every
		 * structure of the House caps at half its hitpoints. */
		if (e->type == STRUCTURE_WINDTRAP && h->powerProduction < h->powerUsage + 20) return e->type;

		/* A Barracks the house cannot afford to keep supplied with soldiers is
		 * worse than no Barracks: the same budget rule that gates units gates the
		 * buildings that only exist to make them, so an economic strategy walks
		 * straight past them to the next refinery. */
		if (Skirmish_IsMilitaryStructure(e->type) && !Skirmish_War_MilitaryAllowed(b)) continue;

		if (fallback == 0xFFFF) fallback = e->type;
		if (si->o.buildCredits > h->credits) continue;

		return e->type;
	}

	return fallback;
}

/**
 * Hand out the planned position for a structure the AI just finished.
 * @param h The House that built it.
 * @param structureType The type that came out of the Construction Yard.
 * @return The packed tile it was planned for, or 0xFFFF when it was not ours.
 */
uint16 Skirmish_Plan_TakePosition(House *h, uint8 structureType)
{
	SkirmishBase *b;
	uint16 i;

	if (h == NULL) return 0xFFFF;

	b = Skirmish_GetBase((uint8)h->index);
	if (b == NULL) return 0xFFFF;

	for (i = 0; i < b->entryCount; i++) {
		SkirmishPlanEntry *e = &b->entries[i];

		if (e->taken) continue;
		if (e->type != structureType) continue;

		e->taken = true;
		if (b->historyCount < SKIRMISH_PLAN_MAX) b->history[b->historyCount++] = structureType;

		Skirmish_LaySlabs(h, e->position, structureType);

		return e->position;
	}

	return 0xFFFF;
}

/**
 * Whether a skirmish AI should put a harvester on the line next.
 *
 * The stock AI never builds one: Structure_AI_PickNextToBuild() masks the
 * harvester out of the Heavy Factory, leaving the house with the single free
 * harvester its Refinery came with.  That is a trickle economy, and a base plan
 * this long stalls on it long before the Palace.
 */
static uint16 Skirmish_CountUnits(uint8 houseID, uint16 type)
{
	PoolFindStruct find;
	uint16 count = 0;
	uint16 oldValidate = g_validateStrictIfZero;

	/* Unit_Find() hides units that are not on the map, and a harvester unloading
	 * inside a refinery is exactly that.  Counting only the visible ones made the
	 * fleet look smaller than it was, so the factory kept overshooting its
	 * target -- and made harvesters look like they were dying in a mode where
	 * nothing can kill them. */
	g_validateStrictIfZero = 1;

	find.houseID = houseID;
	find.index   = 0xFFFF;
	find.type    = type;

	while (Unit_Find(&find) != NULL) count++;

	g_validateStrictIfZero = oldValidate;

	return count;
}

/**
 * Whether a skirmish AI may put a combat unit on the line.
 *
 * Units accumulate against House.unitCountMax, and a house that fills its cap
 * with tanks can never add a harvester afterwards -- which is exactly what
 * happened before this rule: bases reached the Heavy Factory already capped and
 * spent the rest of the match on one harvester.
 */
bool Skirmish_AI_AllowUnit(const House *h, uint16 unitType)
{
	const SkirmishBase *b;

	if (!s_active || h == NULL) return true;

	b = Skirmish_GetBase((uint8)h->index);
	if (b == NULL) return true;

	if (!Skirmish_IsMilitaryUnit(unitType)) return true;

	/* Economy mode measures spice, not war: a soldier built here is credits
	 * burned and a unit slot spent, so nothing but the economy is allowed.  In a
	 * war the same question is a budget question -- has this house spent its
	 * share on fighting already? */
	if (!Skirmish_War_MilitaryAllowed(b)) return false;

	/* Whatever the budget says, keep room in the unit cap for the economy: a
	 * house that fills its slots with tanks can never add a harvester
	 * afterwards, and then it has neither. */
	if (!Skirmish_AI_WantsHarvester(h)) return true;

	return (h->unitCount + SKIRMISH_HARVESTER_TARGET < h->unitCountMax);
}

bool Skirmish_AI_WantsHarvester(const House *h)
{
	const SkirmishBase *b;

	if (!s_active || h == NULL) return false;

	b = Skirmish_GetBase((uint8)h->index);
	if (b == NULL) return false;

	return Skirmish_CountUnits((uint8)h->index, UNIT_HARVESTER) < b->plan.harvesterTarget;
}

/**
 * Whether the Hi-Tech factory should put another carryall on the line.
 *
 * The stock AI builds exactly one: Structure_AI_PickNextToBuild() masks the
 * carryall out as soon as the house owns any.  Carryalls are what keep distant
 * spice worth mining, so an economy plan gets to ask for more.
 */
bool Skirmish_AI_WantsCarryall(const House *h)
{
	const SkirmishBase *b;
	uint16 target;

	if (!s_active || h == NULL) return false;

	b = Skirmish_GetBase((uint8)h->index);
	if (b == NULL) return false;

	/* With the rule on, the fleet size is whatever the road has justified so
	 * far; with it off it is the fixed number the plan asked for. */
	target = (b->plan.carryallWait != 0) ? s_carryallTarget[h->index] : b->plan.carryallTarget;

	return Skirmish_CountUnits((uint8)h->index, UNIT_CARRYALL) < target;
}

/**
 * Place a Starport order for the economy plan.
 *
 * Buying is the fourth way to get a harvester, and the interesting one: it skips
 * the Heavy Factory queue entirely, at a price that swings between 40% and 160%
 * of the build cost.  This mirrors the FACTORY_BUY branch of
 * Structure_BuildObject(), which is player-only because the original AI never
 * shopped.
 * @return True when an order was placed.
 */
bool Skirmish_AI_StarportOrder(House *h, Structure *s)
{
	const SkirmishBase *b;
	uint16 type;
	uint16 slot;
	uint16 price;
	Unit *u;

	if (!s_active || h == NULL || s == NULL) return false;
	if (s->o.type != STRUCTURE_STARPORT) return false;

	b = Skirmish_GetBase((uint8)h->index);
	if (b == NULL) return false;

	/* One frigate carries one order list; wait for the delivery in flight. */
	if (h->starportLinkedID != UNIT_INDEX_INVALID) return false;

	if (s_starportBought[h->index][0] < b->plan.starportHarvesters) {
		type = UNIT_HARVESTER;
		slot = 0;
	} else if (s_starportBought[h->index][1] < b->plan.starportCarryalls) {
		type = UNIT_CARRYALL;
		slot = 1;
	} else {
		return false;
	}

	if (g_starportAvailable[type] == 0) return false;

	/* GUI_FactoryWindow_CalculateStarportPrice() in the same words; it is static
	 * in the GUI layer and this is not a GUI. */
	price = (g_table_unitInfo[type].o.buildCredits / 10) * 4 +
	        (g_table_unitInfo[type].o.buildCredits / 10) * (Tools_RandomLCG_Range(0, 6) + Tools_RandomLCG_Range(0, 6));
	if (price > 999) price = 999;

	if (h->credits < price) return false;

	g_validateStrictIfZero++;
	{
		tile32 tile;

		tile.x = 0xFFFF;
		tile.y = 0xFFFF;
		u = Unit_Create(UNIT_INDEX_INVALID, (uint8)type, (uint8)h->index, tile, 0);
	}
	g_validateStrictIfZero--;

	if (u == NULL) return false;

	h->credits -= price;

	u->o.linkedID = h->starportLinkedID & 0xFF;
	h->starportLinkedID = u->o.index;
	g_structureIndex = s->o.index;

	if (h->starportTimeLeft == 0) h->starportTimeLeft = g_table_houseInfo[h->index].starportDeliveryTime;

	g_starportAvailable[type]--;
	if (g_starportAvailable[type] <= 0) g_starportAvailable[type] = -1;

	s_starportBought[h->index][slot]++;

	return true;
}

/**
 * The credit ceiling of a skirmish house.
 *
 * GameLoop_House() clamps every non-player house to its spice storage, which
 * for a house that owns nothing but a Construction Yard is zero -- the starting
 * credits would be wiped before the first Windtrap is paid for.  Give AI houses
 * the same grace the human gets (g_playerCreditsNoSilo): hold the starting sum
 * until real storage exceeds it.
 */
/** Is any refinery of this house standing free right now? */
static bool Skirmish_Economy_RefineryFree(const House *h)
{
	PoolFindStruct find;

	find.houseID = h->index;
	find.index   = 0xFFFF;
	find.type    = STRUCTURE_REFINERY;

	while (true) {
		const Structure *s = Structure_Find(&find);

		if (s == NULL) break;
		if (s->o.flags.s.isNotOnMap) continue;
		if (s->state != STRUCTURE_STATE_IDLE || s->o.linkedID != 0xFF) continue;

		return true;
	}

	return false;
}

/** Append a structure to a running plan, with the power it needs. */
static bool Skirmish_Plan_Append(SkirmishBase *b, House *h, uint8 type)
{
	const StructureInfo *si = &g_table_structureInfo[type];
	uint16 position;

	if (b->entryCount >= SKIRMISH_PLAN_MAX) return false;

	/* Same rule the plan compiler follows: pay for the power first, or the whole
	 * base drops to half hitpoints to feed the new building. */
	if ((int16)h->powerProduction - (int16)h->powerUsage < si->powerUsage) {
		position = Skirmish_Layout_Next(b, STRUCTURE_WINDTRAP);
		if (position == 0xFFFF) return false;

		b->entries[b->entryCount].type     = STRUCTURE_WINDTRAP;
		b->entries[b->entryCount].taken    = false;
		b->entries[b->entryCount].position = position;
		b->entryCount++;

		if (b->entryCount >= SKIRMISH_PLAN_MAX) return false;
	}

	position = Skirmish_Layout_Next(b, type);
	if (position == 0xFFFF) return false;

	b->entries[b->entryCount].type     = type;
	b->entries[b->entryCount].taken    = false;
	b->entries[b->entryCount].position = position;
	b->entryCount++;

	return true;
}

/**
 * Grow the carryall fleet by what harvesters actually lose on the road.
 *
 * The first carryall is worth having once there are enough harvesters to keep it
 * busy.  After that the trigger is evidence rather than a number: a harvester
 * that has been full for longer than carryallWait ticks *while a refinery stands
 * free* is not queueing, it is driving -- and driving is what a carryall
 * removes.  Time spent waiting for a busy refinery is the other rule's problem.
 */
static void Skirmish_Economy_SampleCarryalls(SkirmishBase *b, House *h, bool refineryFree)
{
	PoolFindStruct find;
	uint16 harvesters;
	bool slowTrip = false;

	if (b->plan.carryallWait == 0) return;

	harvesters = Skirmish_CountUnits((uint8)h->index, UNIT_HARVESTER);

	if (harvesters <= SKIRMISH_CARRYALL_FIRST) return;

	/* The factory that builds them has to exist first. */
	if ((h->structuresBuilt & (1 << STRUCTURE_HIGH_TECH)) == 0) {
		uint16 i;

		for (i = 0; i < b->entryCount; i++) {
			if (b->entries[i].type == STRUCTURE_HIGH_TECH && !b->entries[i].taken) return;
		}

		if (s_carryallTarget[h->index] == 0) {
			s_carryallTarget[h->index] = 1;
			Skirmish_Plan_Append(b, h, STRUCTURE_HIGH_TECH);
		}

		return;
	}

	if (s_carryallTarget[h->index] == 0) s_carryallTarget[h->index] = 1;
	if (s_carryallTarget[h->index] >= SKIRMISH_CARRYALL_MAX) return;

	/* Only count driving time, not queueing time. */
	if (!refineryFree) return;

	/* One at a time: a carryall under construction is already the answer to the
	 * trip that triggered it. */
	if (Skirmish_CountUnits((uint8)h->index, UNIT_CARRYALL) < s_carryallTarget[h->index]) return;

	find.houseID = h->index;
	find.index   = 0xFFFF;
	find.type    = UNIT_HARVESTER;

	while (true) {
		const Unit *u = Unit_Find(&find);
		uint32 *start;

		if (u == NULL) break;
		if (u->o.index >= UNIT_INDEX_MAX) continue;

		start = &s_tripStart[u->o.index];

		if (u->o.flags.s.isNotOnMap || u->amount < 100) {
			*start = 0;
			continue;
		}

		if (*start == 0) {
			*start = g_timerGame;
			continue;
		}

		if (g_timerGame - *start < b->plan.carryallWait) continue;

		*start = g_timerGame;
		slowTrip = true;
	}

	if (!slowTrip) return;

	s_carryallTarget[h->index]++;
}

/**
 * Per-house economy upkeep, called from the house loop so it runs whatever the
 * Construction Yard happens to be doing.
 */
void Skirmish_Economy_Tick(House *h)
{
	SkirmishBase *b;

	if (!s_active || h == NULL) return;

	b = Skirmish_GetBase((uint8)h->index);
	if (b == NULL) return;

	Skirmish_Economy_SampleQueue(b, h);
}

uint16 Skirmish_House_MaxCredits(const House *h)
{
	SkirmishBase *b;

	if (h == NULL) return 0;

	b = Skirmish_GetBase((uint8)h->index);
	if (b == NULL) return h->creditsStorage;

	if (h->creditsStorage > b->creditsNoSilo) b->creditsNoSilo = 0;

	return max(h->creditsStorage, b->creditsNoSilo);
}

/**
 * Add a spice field around a tile.  Map_ChangeSpiceAmount() refuses anything
 * that is not sand, dune or spice, so this quietly skips rock and structures.
 */
static void Skirmish_SeedSpiceField(uint16 centerX, uint16 centerY, uint16 tiles)
{
	uint16 i;

	for (i = 0; i < tiles; i++) {
		const uint16 x = centerX + Tools_RandomLCG_Range(0, 6) - 3;
		const uint16 y = centerY + Tools_RandomLCG_Range(0, 6) - 3;
		uint16 packed;

		if (x >= 64 || y >= 64) continue;

		packed = Tile_PackXY(x, y);
		if (!Map_IsValidPosition(packed)) continue;

		Map_ChangeSpiceAmount(packed, 1);
	}
}

/**
 * Give the map enough spice for a long match.
 *
 * Map_CreateLandscape() scatters between zero and 47 small fields, which is
 * tuned for a campaign scenario that also hands the player a fixed income and
 * ends in half an hour.  Two AIs mining continuously strip that in a few
 * thousand ticks, and Dune II has no spice regrowth: once it is gone both
 * economies stop for good and the match freezes with full armies standing
 * around.
 */
static void Skirmish_SeedSpice(uint16 fields, uint16 tilesPerField)
{
	uint16 i;

	for (i = 0; i < fields; i++) {
		Skirmish_SeedSpiceField(Tools_RandomLCG_Range(2, 60), Tools_RandomLCG_Range(2, 60), tilesPerField);
	}
}

static void Skirmish_SetupBase(SkirmishBase *b, uint8 houseID, uint16 rectX, uint16 rectY, const SkirmishEconomyPlan *plan)
{
	House *h;
	Structure *s;
	uint8 i;

	memset(b, 0, sizeof(SkirmishBase));

	b->houseID       = houseID;
	b->rectX         = rectX;
	b->rectY         = rectY;
	b->creditsNoSilo = SKIRMISH_START_CREDITS;

	if (plan != NULL) {
		b->plan = *plan;
	} else {
		Skirmish_MakeDefaultPlan(&b->plan);
	}

	Skirmish_CarveRock(b);
	Skirmish_Plan_Create(b);

	h = House_Allocate(houseID);
	if (h == NULL) return;

	h->credits           = SKIRMISH_START_CREDITS;
	h->creditsQuota      = 0;
	/* Economy mode is a laboratory: let a plan buy as many harvesters as it can
	 * pay for and find out where that stops paying, instead of stopping at a
	 * cap borrowed from a combat match.  The unit pool holds 102. */
	h->unitCountMax      = s_economyMode ? 90 : (s_warMode ? SKIRMISH_WAR_UNIT_MAX : SKIRMISH_UNIT_MAX);
	h->flags.human       = false;
	/* Normally set when the human first sees the house; nobody is going to see
	 * these two, and without it the AI never builds or repairs anything. */
	h->flags.isAIActive  = true;

	Skirmish_LaySlabs(h, b->origin, STRUCTURE_CONSTRUCTION_YARD);

	s = Structure_Create(STRUCTURE_INDEX_INVALID, STRUCTURE_CONSTRUCTION_YARD, houseID, b->origin);
	if (s == NULL) return;

	s->o.flags.s.degrades = false;
	s->state = STRUCTURE_STATE_IDLE;

	/* Teams are what makes an AI attack at all: on their own, factory units
	 * roll out and guard.  One team per movement type, because a team only ever
	 * recruits units that move like it does.  Economy mode has no enemy. */
	for (i = 0; !s_economyMode && i < lengthof(s_teamPlan); i++) {
		Team_Create(houseID, TEAM_ACTION_NORMAL, s_teamPlan[i].movementType,
		            s_teamPlan[i].minMembers, s_teamPlan[i].maxMembers);
	}
}

/**
 * Start a skirmish: generate the map, place both bases and hand control to the
 * game loop.  The caller is responsible for the surrounding mode switching.
 */
/**
 * Set up a match.  Pass HOUSE_INVALID as the second house for a solo economy
 * run, and a seed of 0 to pick a random map.  A plan of NULL means the default
 * blueprint; in a war both houses get one, and they need not be the same.
 */
static bool Skirmish_StartInternal(uint8 houseID1, uint8 houseID2, uint32 seed, const SkirmishEconomyPlan *plan1, const SkirmishEconomyPlan *plan2, bool war)
{
	uint16 jitterX, jitterY;
	uint16 margin;
	int i;

	if (houseID1 >= HOUSE_MAX) return false;
	if (houseID2 != HOUSE_INVALID && (houseID2 >= HOUSE_MAX || houseID1 == houseID2)) return false;

	Skirmish_Reset();

	Sound_Output_Feedback(0xFFFE);

	Game_Init();
	UnitSelection_ClearControlGroups();

	/* After Game_Init(): it resets this module, so a mode set before it would be
	 * wiped and the run would quietly fall back to the standard base. */
	s_warMode     = war;
	s_economyMode = (!war && houseID2 == HOUSE_INVALID);

	g_validateStrictIfZero++;

	if (seed == 0) seed = ((uint32)Tools_RandomLCG_Range(0, 0x7FFF) << 15) | Tools_RandomLCG_Range(0, 0x7FFF);

	memset(&g_scenario, 0, sizeof(Scenario));
	/* No win or lose flags: a skirmish is watched, not won. */
	g_scenario.mapSeed  = seed;
	g_scenario.mapScale = 0; /* 62x62, the largest map the engine has. */
	strcpy(g_scenario.pictureBriefing, "HARVEST.WSA");
	strcpy(g_scenario.pictureWin,      "WIN1.WSA");
	strcpy(g_scenario.pictureLose,     "LOSTBILD.WSA");

	for (i = 0; i < 16; i++) {
		g_scenario.reinforcement[i].unitID = UNIT_INDEX_INVALID;
	}

	g_campaignID    = SKIRMISH_CAMPAIGN;
	g_scenarioID    = 1;
	g_playerHouseID = HOUSE_MERCENARY;

	Sprites_LoadTiles();
	Map_CreateLandscape(seed);

	/* The spectator owns nothing, but the GUI needs a house to point at. */
	g_playerHouse = House_Allocate(HOUSE_MERCENARY);
	if (g_playerHouse == NULL) {
		g_validateStrictIfZero--;
		return false;
	}
	g_playerHouse->flags.human = true;
	g_playerCreditsNoSilo = 0;

	s_active = true;

	margin  = 2;
	jitterX = Tools_RandomLCG_Range(0, 6);
	jitterY = Tools_RandomLCG_Range(0, 6);

	if (houseID2 == HOUSE_INVALID) {
		/* Solo: the base sits in a corner all the same, so a plan is scored on
		 * the same geometry it would face in a match. */
		Skirmish_SetupBase(&s_bases[0], houseID1, margin + jitterX, margin + jitterY, plan1);
	} else if (Tools_RandomLCG_Range(0, 1) == 0) {
		/* Top-left versus bottom-right. */
		Skirmish_SetupBase(&s_bases[0], houseID1, margin + jitterX, margin + jitterY, plan1);
		Skirmish_SetupBase(&s_bases[1], houseID2, 62 - SKIRMISH_BASE_WIDTH - margin - jitterX, 62 - SKIRMISH_BASE_HEIGHT - margin - jitterY, plan2);
	} else {
		/* Top-right versus bottom-left. */
		Skirmish_SetupBase(&s_bases[0], houseID1, 62 - SKIRMISH_BASE_WIDTH - margin - jitterX, margin + jitterY, plan1);
		Skirmish_SetupBase(&s_bases[1], houseID2, margin + jitterX, 62 - SKIRMISH_BASE_HEIGHT - margin - jitterY, plan2);
	}

	Skirmish_SeedSpice(60, 30);

	/* Both sides need something to mine within reach of home, or the opening is
	 * decided by which corner the generator happened to favour. */
	for (i = 0; i < SKIRMISH_PLAYER_MAX; i++) {
		uint16 originX, originY;

		if (s_bases[i].entryCount == 0) continue;

		originX = Tile_GetPackedX(s_bases[i].origin);
		originY = Tile_GetPackedY(s_bases[i].origin);

		Skirmish_SeedSpiceField(originX + SKIRMISH_BASE_WIDTH + 4, originY + 4, 40);
		Skirmish_SeedSpiceField(originX + 4, originY + SKIRMISH_BASE_HEIGHT + 4, 40);
	}

	/* The spectator sees everything: without this the whole match happens
	 * behind the veil. */
	for (i = 0; i < 64 * 64; i++) {
		g_map[i].isUnveiled = true;
	}

	g_selectionRectanglePosition = s_bases[0].origin;
	g_selectionPosition          = s_bases[0].origin;
	Map_SetViewportPosition(s_bases[0].origin);
	g_minimapPosition            = g_viewportPosition;

	/* Nothing stocks the Starport outside a scenario's CHOAM section, and an
	 * empty Starport is a plan option that silently does nothing. */
	if (s_economyMode) {
		g_starportAvailable[UNIT_HARVESTER] = 5;
		g_starportAvailable[UNIT_CARRYALL]  = 5;
	}

	g_tickScenarioStart = g_timerGame;

	g_validateStrictIfZero--;

	Game_Prepare();

	return true;
}

bool Skirmish_Start(uint8 houseID1, uint8 houseID2)
{
	return Skirmish_StartInternal(houseID1, houseID2, 0, NULL, NULL, true);
}

bool Skirmish_StartEconomy(uint8 houseID, uint32 seed, const SkirmishEconomyPlan *plan)
{
	if (plan == NULL || plan->buildCount == 0) return false;

	return Skirmish_StartInternal(houseID, HOUSE_INVALID, seed, plan, NULL, false);
}

bool Skirmish_StartWar(uint8 houseID1, uint8 houseID2, uint32 seed, const SkirmishEconomyPlan *plan1, const SkirmishEconomyPlan *plan2)
{
	if (houseID2 == HOUSE_INVALID) return false;

	return Skirmish_StartInternal(houseID1, houseID2, seed, plan1, plan2, true);
}

/**
 * What a house still owns, priced at what it cost to build.
 *
 * A match that runs out of ticks before it runs out of bases still has a result:
 * whoever is left holding more of the map's value was winning.  Hitpoints scale
 * the price, so a base ground down to rubble scores as the rubble it is.
 */
uint32 Skirmish_War_GetValue(uint8 index)
{
	PoolFindStruct find;
	uint32 value = 0;
	uint16 oldValidate = g_validateStrictIfZero;

	if (!s_active || index >= SKIRMISH_PLAYER_MAX || s_bases[index].entryCount == 0) return 0;

	find.houseID = s_bases[index].houseID;
	find.index   = 0xFFFF;
	find.type    = 0xFFFF;

	while (true) {
		const Structure *s = Structure_Find(&find);
		const StructureInfo *si;

		if (s == NULL) break;
		if (s->o.flags.s.isNotOnMap) continue;

		si = &g_table_structureInfo[s->o.type];
		if (si->o.hitpoints == 0) continue;

		value += (uint32)si->o.buildCredits * s->o.hitpoints / si->o.hitpoints;
	}

	/* Units inside a refinery or a repair bay are still assets. */
	g_validateStrictIfZero = 1;

	find.houseID = s_bases[index].houseID;
	find.index   = 0xFFFF;
	find.type    = 0xFFFF;

	while (true) {
		const Unit *u = Unit_Find(&find);
		const UnitInfo *ui;

		if (u == NULL) break;

		ui = &g_table_unitInfo[u->o.type];
		if (ui->o.hitpoints == 0) continue;

		value += (uint32)ui->o.buildCredits * u->o.hitpoints / ui->o.hitpoints;
	}

	g_validateStrictIfZero = oldValidate;

	return value;
}

/**
 * A house is out of the match when the last thing it can build from is gone.
 * Stray units left wandering are not a comeback, and waiting for the last
 * soldier to be hunted down would double the length of every simulation.
 */
bool Skirmish_War_IsDefeated(uint8 index)
{
	PoolFindStruct find;

	if (!s_active || index >= SKIRMISH_PLAYER_MAX || s_bases[index].entryCount == 0) return false;

	find.houseID = s_bases[index].houseID;
	find.index   = 0xFFFF;
	find.type    = 0xFFFF;

	while (true) {
		const Structure *s = Structure_Find(&find);

		if (s == NULL) break;
		if (s->o.flags.s.isNotOnMap) continue;
		if (s->o.type == STRUCTURE_SLAB_1x1 || s->o.type == STRUCTURE_SLAB_2x2 || s->o.type == STRUCTURE_WALL) continue;

		return false;
	}

	return true;
}

/**
 * One line status of a skirmish house: credits, power, plan progress and what
 * the Construction Yard is working on right now.
 * @return False when there is no such skirmish house.
 */
bool Skirmish_GetSummary(uint8 index, char *buf, uint16 length)
{
	const SkirmishBase *b;
	const House *h;
	PoolFindStruct find;
	const char *building = "-";
	uint16 done = 0;
	uint16 alive = 0;
	uint32 hitpoints = 0;
	uint32 hitpointsMax = 0;
	uint16 i;

	if (!s_active || index >= SKIRMISH_PLAYER_MAX || buf == NULL) return false;

	b = &s_bases[index];
	if (b->entryCount == 0) return false;

	h = House_Get_ByIndex(b->houseID);

	for (i = 0; i < b->entryCount; i++) {
		if (b->entries[i].taken) done++;
	}

	find.houseID = b->houseID;
	find.index   = 0xFFFF;
	find.type    = STRUCTURE_CONSTRUCTION_YARD;

	while (true) {
		const Structure *s = Structure_Find(&find);

		if (s == NULL) break;
		if (s->o.linkedID == 0xFF) continue;

		building = g_table_structureInfo[Structure_Get_ByIndex(s->o.linkedID)->o.type].o.name;
		break;
	}

	/* Standing structures, which is plan progress minus whatever the enemy has
	 * blown up since. */
	find.houseID = b->houseID;
	find.index   = 0xFFFF;
	find.type    = 0xFFFF;

	while (true) {
		const Structure *s = Structure_Find(&find);

		if (s == NULL) break;
		if (s->o.type == STRUCTURE_SLAB_1x1 || s->o.type == STRUCTURE_SLAB_2x2 || s->o.type == STRUCTURE_WALL) continue;
		if (s->o.flags.s.isNotOnMap) continue;

		alive++;
		hitpoints += s->o.hitpoints;
		hitpointsMax += g_table_structureInfo[s->o.type].o.hitpoints;
	}

	if (s_economyMode) {
		/* Refined spice is the whole point here, so it leads. */
		snprintf(buf, length, "%s refined %u | %uc h%u u%u s%u %u/%u %s",
		         g_table_houseInfo[b->houseID].name, (unsigned)s_harvested[b->houseID],
		         h->credits, Skirmish_CountUnits(b->houseID, UNIT_HARVESTER), h->unitCount,
		         alive, done, b->entryCount, building);

		return true;
	}

	/* The war line leads with the split that is being searched: the share in
	 * force right now, and what the house has actually spent on the army against
	 * what it has spent on the economy. */
	snprintf(buf, length, "%s mil%u%% %u/%uc %uc h%u u%u s%u@%u%% val%u %u/%u %s",
	         g_table_houseInfo[b->houseID].name,
	         (unsigned)((g_timerGame - g_tickScenarioStart >= b->plan.militarySwitchTick)
	                    ? b->plan.militaryShareLate : b->plan.militaryShare),
	         (unsigned)b->militarySpent, (unsigned)b->economySpent,
	         h->credits,
	         Skirmish_CountUnits(b->houseID, UNIT_HARVESTER), h->unitCount,
	         alive, (unsigned)((hitpointsMax != 0) ? hitpoints * 100 / hitpointsMax : 100),
	         (unsigned)Skirmish_War_GetValue(index),
	         done, b->entryCount, building);

	return true;
}

/**
 * The structures a skirmish house has placed, in the order it chose them.
 * @return False when there is no such skirmish house.
 */
bool Skirmish_GetBuildOrder(uint8 index, char *buf, uint16 length)
{
	const SkirmishBase *b;
	uint16 used = 0;
	uint16 i;

	if (!s_active || index >= SKIRMISH_PLAYER_MAX || buf == NULL || length == 0) return false;

	b = &s_bases[index];
	if (b->entryCount == 0) return false;

	buf[0] = '\0';

	for (i = 0; i < b->historyCount; i++) {
		const char *name = g_table_structureInfo[b->history[i]].o.name;
		uint16 len = (uint16)strlen(name);

		if (used + len + 2 >= length) break;

		if (used != 0) {
			buf[used++] = ',';
			buf[used++] = ' ';
		}

		memcpy(buf + used, name, len);
		used += len;
		buf[used] = '\0';
	}

	return true;
}

/**
 * Spice left on the map, counting thick spice twice.  There is no regrowth in
 * Dune II, so this number only ever falls; when it reaches zero both economies
 * are finished, whatever the credit counters say.
 */
uint16 Skirmish_GetMapSpice(void)
{
	uint16 count = 0;
	uint16 packed;

	for (packed = 0; packed < 64 * 64; packed++) {
		const uint16 lst = Map_GetLandscapeType(packed);

		if (lst == LST_SPICE) count++;
		if (lst == LST_THICK_SPICE) count += 2;
	}

	return count;
}

/**
 * The attack teams of a skirmish house: members, size window and whether they
 * currently hold a target.  This is the first thing to look at when nobody is
 * attacking.
 * @return False when there is no such skirmish house.
 */
bool Skirmish_GetTeams(uint8 index, char *buf, uint16 length)
{
	static const char *movementName[MOVEMENT_MAX] = { "foot", "track", "harv", "wheel", "air", "worm" };

	const SkirmishBase *b;
	PoolFindStruct find;
	uint16 used = 0;

	if (!s_active || index >= SKIRMISH_PLAYER_MAX || buf == NULL || length == 0) return false;

	b = &s_bases[index];
	if (b->entryCount == 0) return false;

	buf[0] = '\0';

	find.houseID = b->houseID;
	find.index   = 0xFFFF;
	find.type    = 0xFFFF;

	while (used + 24 < length) {
		const Team *t = Team_Find(&find);
		int written;

		if (t == NULL) break;

		written = snprintf(buf + used, length - used, "%s%s %u/%u-%u%s",
		                   (used == 0) ? "teams: " : ", ",
		                   (t->movementType < MOVEMENT_MAX) ? movementName[t->movementType] : "?",
		                   t->members, t->minMembers, t->maxMembers,
		                   (t->target != 0) ? "*" : "");

		if (written <= 0) break;
		used += (uint16)written;
	}

	return true;
}

/**
 * Draw a line per AI over the viewport, so the match can be followed without
 * clicking around.
 */
void Skirmish_DrawStatusOverlay(void)
{
	uint8 i;

	if (!s_active) return;

	for (i = 0; i < SKIRMISH_PLAYER_MAX; i++) {
		char line[80];

		if (!Skirmish_GetSummary(i, line, sizeof(line))) continue;

		GUI_DrawText_Wrapper(line, 2, 42 + i * 8, 0xFF, 0, 0x22);
	}
}
