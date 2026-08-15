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
#include "doctrine.h"
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
/** Ticks between two spice blooms.  Five thousand is roughly a blooms-worth of
 *  mining by a grown economy, so the map holds level rather than growing. */
#define SKIRMISH_BLOOM_TICKS 5000

/** How often the refinery queue is looked at, in game ticks. */
#define SKIRMISH_QUEUE_SAMPLE_TICKS 60
/** Harvesters a skirmish AI keeps trying to reach. */
#define SKIRMISH_HARVESTER_TARGET 3
/** Unit cap per skirmish house. */
#define SKIRMISH_UNIT_MAX 25
/** Unit cap per house in a war: two of these must fit the ground band of the pool. */
#define SKIRMISH_WAR_UNIT_MAX 90

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
#define SKIRMISH_BASE_WIDTH  24
#define SKIRMISH_BASE_HEIGHT 20
/** Middle of the 62x62 map, which is what a base is oriented against. */
#define SKIRMISH_MAP_CENTER  31

/**
 * Depth of the band kept clear along the two edges facing the middle of the map.
 * Buildings start behind it; the defence line is built into it.
 *
 * Without the band the Construction Yard sat on the corner nearest the enemy and
 * the first thing an attack met was the Yard itself.  Four tiles is two lines
 * with a tile of daylight: a picket of walls right on the edge and the turrets
 * behind it, still inside the plateau so nothing degrades on sand.
 */
#define SKIRMISH_BASE_DEFENCE 4
#define SKIRMISH_DEFENCE_WALL_INSET   0
#define SKIRMISH_DEFENCE_TURRET_INSET 2
/**
 * Gap between two neighbours on a defence line.  Deliberately not 1: a solid
 * wall would seal the base, and the units inside -- harvesters included -- would
 * have nowhere to drive out.  A picket absorbs a charge without being a gate.
 */
#define SKIRMISH_DEFENCE_SPACING      2

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

	/* Which half of the map the rectangle landed in.  Everything that has a
	 * direction -- the build order's growth, the spice seeded for this house --
	 * is expressed against the middle of the map through these two, so all four
	 * corners come out as mirror images of each other. */
	uint16 rectX, rectY;
	bool   eastSide, southSide;

	/* Layout cursor, only used while building the plan.  It is base-local:
	 * (0,0) is the corner of the rectangle facing the middle of the map. */
	uint16 cursorX, cursorY;
	uint16 rowHeight;
	/* Slots handed out on the two defence lines, walls and turrets separately. */
	uint16 defenceWall, defenceTurret;
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
static uint32 s_nextBloom;                                  /*!< Game tick the next spice bloom is due. */
static uint16 s_carryallTarget[HOUSE_MAX];
static uint32 s_tripStart[UNIT_INDEX_MAX];
static uint16 s_starportBought[HOUSE_MAX][2];               /*!< [0] harvesters, [1] carryalls. */
/* Losses by cause.  "Nobody is shooting at them" and "everybody is shooting at
 * them and missing" look identical on screen; these tell them apart. */
static uint16 s_killedByFire[HOUSE_MAX];
static uint16 s_killedByTracks[HOUSE_MAX];
/* Hitpoints knocked off this House's own units and structures, cumulative.  In a
 * two-house match "damage this House took" is "damage the other House dealt",
 * which is the only way to attribute it at all: neither Unit_Damage() nor
 * Structure_Damage() is told who fired. */
static uint32 s_damageTaken[HOUSE_MAX];
/* Units this house has ever built, by type.  The army mix is judged against this
 * rather than against what is alive: a type that keeps dying would otherwise
 * read as "we have none of those" for ever, and the rule would order it again
 * and again -- which is the single-unit-type behaviour it exists to replace. */
static uint16 s_unitsBuilt[HOUSE_MAX][UNIT_MAX];
/* Whether each house is currently attacking or currently building up. */
static bool s_waveOpen[HOUSE_MAX];
/* Alternation on a house's only Heavy Factory: harvester, army, harvester. */
static bool s_harvesterTurn[HOUSE_MAX];

/* What the army is shooting at, cumulative.  Shots, not orders: an order that
 * never turns into a shot is exactly the failure these are here to catch. */
static uint32 s_shotsTurret[HOUSE_MAX];
static uint32 s_shotsStructure[HOUSE_MAX];
static uint32 s_shotsUnit[HOUSE_MAX];
/* Shots at a building while an enemy turret stood within reach.  This is the
 * one that says "walked past it" rather than "there was none": a low turret
 * share can mean either, and only this tells them apart. */
static uint32 s_shotsBypass[HOUSE_MAX];

void Skirmish_RecordBuilt(uint8 houseID, uint16 unitType)
{
	if (!s_active || houseID >= HOUSE_MAX || unitType >= UNIT_MAX) return;

	if (s_unitsBuilt[houseID][unitType] < 0xFFFF) s_unitsBuilt[houseID][unitType]++;
}

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
	/* The second production line, and it belongs here rather than at the end of
	 * the plan.  One Heavy Factory has to serve both the harvester fleet and the
	 * army, and the fleet wins: no house of either doctrine built a single tank
	 * before t125000 while the sixteenth harvester was still owed.  A factory is
	 * economy spending, so this costs the army budget nothing. */
	STRUCTURE_HEAVY_VEHICLE,
	STRUCTURE_WINDTRAP,
	STRUCTURE_REPAIR,
	STRUCTURE_STARPORT,
	STRUCTURE_WINDTRAP,
	STRUCTURE_ROCKET_TURRET,
	STRUCTURE_ROCKET_TURRET,
	STRUCTURE_HOUSE_OF_IX,
	STRUCTURE_PALACE,

	/* A third line, and a second light one.  One Heavy Factory is a hard ceiling
	 * on how fast a house replaces what it loses: a Siege Tank takes as long to
	 * build whether the house has 2000 credits or 8000, so a grown economy spends
	 * the back half of a match unable to turn money into an army. */
	STRUCTURE_HEAVY_VEHICLE,
	STRUCTURE_LIGHT_VEHICLE,

	/* The forward line, and it is deliberately long.  A pair of turrets is a
	 * speed bump; what stops a team is enough of them that the team dies inside
	 * their combined range, and they are cheap next to the tanks they kill -- a
	 * Rocket Turret is 250 credits against a Siege Tank's 600.
	 *
	 * Walls interleave with them rather than going up first: a wall on its own
	 * kills nothing, it only buys the turrets behind it another few seconds of
	 * shooting.  Every entry here is military spending, so the whole line waits
	 * behind the same budget switch the army does -- see Skirmish_War_Charge(). */
	STRUCTURE_WINDTRAP,
	STRUCTURE_ROCKET_TURRET,
	STRUCTURE_ROCKET_TURRET,
	STRUCTURE_WALL,
	STRUCTURE_WALL,
	STRUCTURE_ROCKET_TURRET,
	STRUCTURE_ROCKET_TURRET,
	STRUCTURE_WALL,
	STRUCTURE_WALL,
	STRUCTURE_WINDTRAP,
	STRUCTURE_ROCKET_TURRET,
	STRUCTURE_ROCKET_TURRET,
	STRUCTURE_WALL,
	STRUCTURE_WALL,
	STRUCTURE_TURRET,
	STRUCTURE_TURRET,
	STRUCTURE_WALL,
	STRUCTURE_WALL,
	STRUCTURE_WINDTRAP,
	STRUCTURE_ROCKET_TURRET,
	STRUCTURE_ROCKET_TURRET,
	STRUCTURE_WALL,
	STRUCTURE_WALL,
	STRUCTURE_TURRET,
	STRUCTURE_TURRET,
	STRUCTURE_WALL,
	STRUCTURE_WALL
};

/** Whether a structure belongs on the forward line rather than in the base. */
static bool Skirmish_IsDefenceStructure(uint8 type)
{
	return (type == STRUCTURE_WALL || type == STRUCTURE_TURRET || type == STRUCTURE_ROCKET_TURRET);
}

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
		case STRUCTURE_WALL:
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
	/* Bigger cohorts than the campaign's, because these have to read as waves.
	 * A team of three leaves almost as soon as it is formed and the next one is
	 * out before it lands, which is a stream, not an attack. */
	{ MOVEMENT_FOOT,    6, 10 },
	{ MOVEMENT_WHEELED, 4, 6 },
	{ MOVEMENT_TRACKED, 5, 8 },
	{ MOVEMENT_TRACKED, 5, 8 }
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
	s_nextBloom = 0;
	memset(s_carryallTarget, 0, sizeof(s_carryallTarget));
	memset(s_tripStart, 0, sizeof(s_tripStart));
	memset(s_starportBought, 0, sizeof(s_starportBought));
	memset(s_killedByFire, 0, sizeof(s_killedByFire));
	memset(s_killedByTracks, 0, sizeof(s_killedByTracks));
	memset(s_damageTaken, 0, sizeof(s_damageTaken));
	memset(s_unitsBuilt, 0, sizeof(s_unitsBuilt));
	memset(s_waveOpen, 0, sizeof(s_waveOpen));
	memset(s_harvesterTurn, 0, sizeof(s_harvesterTurn));
	memset(s_shotsTurret, 0, sizeof(s_shotsTurret));
	memset(s_shotsStructure, 0, sizeof(s_shotsStructure));
	memset(s_shotsUnit, 0, sizeof(s_shotsUnit));
	memset(s_shotsBypass, 0, sizeof(s_shotsBypass));
	Doctrine_Reset();
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
/**
 * Where a skirmish house has its Construction Yard.
 *
 * A spectator opens on the first base, and on a 62x62 map the other one is a
 * single building forty tiles away -- which looks a lot like the second AI never
 * turned up.  This is what the camera jump keys off.
 * @return The packed tile, or 0xFFFF when there is no such base.
 */
uint16 Skirmish_GetBaseOrigin(uint8 index)
{
	if (!s_active || index >= SKIRMISH_PLAYER_MAX || s_bases[index].entryCount == 0) return 0xFFFF;

	return s_bases[index].origin;
}

/**
 * Where a house gathers what it is not currently attacking with.
 *
 * Behind its own defence line, on the two faces that look at the middle of the
 * map -- which is where the turrets are, so a reserve standing here is standing
 * with them rather than in the open.  Six tiles inside the plateau corner
 * nearest the centre: clear of the wall picket in the four-tile defence band,
 * and still on rock.
 */
uint16 Skirmish_GetBaseRally(uint8 houseID)
{
	PoolFindStruct find;
	uint8 index;

	if (!s_active) return 0;

	/* Stand at the most forward gun, not at the middle of the line.
	 *
	 * The centre of mass of the turrets looks right and is not: the line runs
	 * along two faces of the plateau, so its centroid falls in the corner
	 * *between* them, measured three tiles off the nearest gun and further from
	 * most.  Three tiles is the difference between infantry standing in the
	 * picket and infantry standing behind it, which is the difference between the
	 * guns being covered and not.
	 *
	 * The turret nearest the middle of the map is the one an attack reaches
	 * first, so that is where the reserve belongs; Doctrine_Rally() spreads them
	 * from there along the rest of the line. */
	{
		const uint16 centre = Tile_PackXY(SKIRMISH_MAP_CENTER, SKIRMISH_MAP_CENTER);
		uint16 best = 0;
		uint16 bestDistance = 0xFFFF;

		find.houseID = houseID;
		find.index   = 0xFFFF;
		find.type    = 0xFFFF;

		while (true) {
			const Structure *s = Structure_Find(&find);
			uint16 packed, d;

			if (s == NULL) break;
			if (s->o.type != STRUCTURE_TURRET && s->o.type != STRUCTURE_ROCKET_TURRET) continue;
			if (s->o.flags.s.isNotOnMap) continue;

			packed = Tile_PackTile(s->o.position);
			d = Tile_GetDistancePacked(packed, centre);
			if (d >= bestDistance) continue;

			bestDistance = d;
			best = packed;
		}

		if (best != 0) return best;
	}

	/* No line yet: the corner of the plateau it is going to be built on. */
	for (index = 0; index < SKIRMISH_PLAYER_MAX; index++) {
		const SkirmishBase *b = &s_bases[index];
		uint16 x, y;

		if (b->entryCount == 0 || b->houseID != houseID) continue;

		x = b->eastSide  ? (uint16)(b->rectX + SKIRMISH_DEFENCE_TURRET_INSET + 1)
		                 : (uint16)(b->rectX + SKIRMISH_BASE_WIDTH  - SKIRMISH_DEFENCE_TURRET_INSET - 1);
		y = b->southSide ? (uint16)(b->rectY + SKIRMISH_DEFENCE_TURRET_INSET + 1)
		                 : (uint16)(b->rectY + SKIRMISH_BASE_HEIGHT - SKIRMISH_DEFENCE_TURRET_INSET - 1);

		return Tile_PackXY(x, y);
	}

	return 0;
}

/** Tiles between a tile and this House's nearest turret, or 0xFFFF for none. */
uint16 Skirmish_GetTurretDistance(uint8 houseID, uint16 packed)
{
	PoolFindStruct find;
	uint16 best = 0xFFFF;

	if (!s_active || packed == 0) return 0xFFFF;

	find.houseID = houseID;
	find.index   = 0xFFFF;
	find.type    = 0xFFFF;

	while (true) {
		const Structure *s = Structure_Find(&find);
		uint16 d;

		if (s == NULL) break;
		if (s->o.type != STRUCTURE_TURRET && s->o.type != STRUCTURE_ROCKET_TURRET) continue;
		if (s->o.flags.s.isNotOnMap) continue;

		d = Tile_GetDistancePacked(packed, Tile_PackTile(s->o.position));
		if (d < best) best = d;
	}

	return best;
}

/** The other House in a two-House match, or HOUSE_INVALID. */
uint8 Skirmish_GetOpponent(uint8 houseID)
{
	uint8 index;

	if (!s_active) return HOUSE_INVALID;

	for (index = 0; index < SKIRMISH_PLAYER_MAX; index++) {
		const SkirmishBase *b = &s_bases[index];

		if (b->entryCount == 0 || b->houseID == houseID) continue;
		return b->houseID;
	}

	return HOUSE_INVALID;
}

/** The House holding base slot @p index, or HOUSE_INVALID. */
uint8 Skirmish_GetBaseHouse(uint8 index)
{
	if (!s_active || index >= SKIRMISH_PLAYER_MAX || s_bases[index].entryCount == 0) return HOUSE_INVALID;

	return s_bases[index].houseID;
}

/**
 * The plateau a House was given, by House rather than by slot.
 *
 * A doctrine needs this to reason about the faces of an enemy base: the defence
 * line only covers the two that look at the middle of the map, and the shape of
 * the rectangle is what says which two those are.
 */
bool Skirmish_GetBaseRect(uint8 houseID, uint16 *x, uint16 *y, uint16 *width, uint16 *height)
{
	uint8 index;

	if (!s_active) return false;

	for (index = 0; index < SKIRMISH_PLAYER_MAX; index++) {
		if (s_bases[index].entryCount == 0 || s_bases[index].houseID != houseID) continue;

		if (x != NULL)      *x = s_bases[index].rectX;
		if (y != NULL)      *y = s_bases[index].rectY;
		if (width != NULL)  *width = SKIRMISH_BASE_WIDTH;
		if (height != NULL) *height = SKIRMISH_BASE_HEIGHT;

		return true;
	}

	return false;
}

uint16 Skirmish_GetUnitsBuilt(uint8 houseID, uint16 unitType)
{
	if (houseID >= HOUSE_MAX || unitType >= UNIT_MAX) return 0;

	return s_unitsBuilt[houseID][unitType];
}

static uint16 Skirmish_CountUnits(uint8 houseID, uint16 type);

uint16 Skirmish_CountUnitsOfType(uint8 houseID, uint16 unitType)
{
	if (houseID >= HOUSE_MAX || unitType >= UNIT_MAX) return 0;

	return Skirmish_CountUnits(houseID, unitType);
}

/** Combat units this House has standing, counting the ones inside a building. */
uint16 Skirmish_CountCombatUnits(uint8 houseID)
{
	PoolFindStruct find;
	uint16 count = 0;
	uint16 oldValidate = g_validateStrictIfZero;

	if (houseID >= HOUSE_MAX) return 0;

	g_validateStrictIfZero = 1;

	find.houseID = houseID;
	find.index   = 0xFFFF;
	find.type    = 0xFFFF;

	while (true) {
		const Unit *u = Unit_Find(&find);

		if (u == NULL) break;
		if (!Skirmish_IsMilitaryUnit(u->o.type)) continue;
		if (!g_table_unitInfo[u->o.type].o.flags.priority) continue;   /* Bullets are not an army. */

		count++;
	}

	g_validateStrictIfZero = oldValidate;

	return count;
}

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
 * Reserve room for one structure inside the base rectangle, packing rows with a
 * one tile gap so units can still move through the base.
 *
 * The packing runs in base-local coordinates and is mirrored onto the map on the
 * way out, so every base starts at the corner of its rectangle nearest the
 * middle of the map and grows away from it.  Without that mirroring the corner a
 * house drew decided the match: the layout always started at the rectangle's
 * top-left, which for a bottom-right base is the corner facing the spice in the
 * middle and for a top-left base is the one facing away from it -- most of a
 * base rectangle of difference in how far the first harvester has to drive,
 * every trip, for the whole match.
 * @return The packed top-left tile, or 0xFFFF when the base is full.
 */
/**
 * Turn a base-local slot into the packed map tile it stands on.
 *
 * A western base grows east to west, so its slots are counted back from the
 * eastern edge; a structure is anchored by its top-left tile, so a mirrored slot
 * steps back by its own size as well.
 */
static uint16 Skirmish_Layout_Map(const SkirmishBase *b, uint16 lx, uint16 ly, uint16 width, uint16 height)
{
	const uint16 x = b->eastSide  ? (b->rectX + lx) : (b->rectX + SKIRMISH_BASE_WIDTH  - lx - width);
	const uint16 y = b->southSide ? (b->rectY + ly) : (b->rectY + SKIRMISH_BASE_HEIGHT - ly - height);

	return Tile_PackXY(x, y);
}

/**
 * Reserve the next slot on the forward defence line.
 *
 * Two lines run along the two edges of the plateau that face the middle of the
 * map: walls right on the edge, turrets a couple of tiles behind them.  Slots
 * alternate between the two arms of that L so both faces of the base thicken
 * together instead of one being finished before the other is started.
 * @return The packed tile, or 0xFFFF when the line is full.
 */
static uint16 Skirmish_Layout_Defence(SkirmishBase *b, uint8 type)
{
	const bool wall  = (type == STRUCTURE_WALL);
	const uint16 inset = wall ? SKIRMISH_DEFENCE_WALL_INSET : SKIRMISH_DEFENCE_TURRET_INSET;
	uint16 *slot = wall ? &b->defenceWall : &b->defenceTurret;
	uint16 lx, ly;

	if ((*slot & 1) == 0) {
		lx = inset + (*slot >> 1) * SKIRMISH_DEFENCE_SPACING;
		ly = inset;
	} else {
		lx = inset;
		ly = inset + ((*slot >> 1) + 1) * SKIRMISH_DEFENCE_SPACING;
	}

	if (lx >= SKIRMISH_BASE_WIDTH || ly >= SKIRMISH_BASE_HEIGHT) return 0xFFFF;

	(*slot)++;

	return Skirmish_Layout_Map(b, lx, ly, 1, 1);
}

static uint16 Skirmish_Layout_Next(SkirmishBase *b, uint8 type)
{
	const StructureInfo *si = &g_table_structureInfo[type];
	const XYSize *size = &g_table_structure_layoutSize[si->layout];
	uint16 lx;

	if (b->cursorX + size->width > SKIRMISH_BASE_WIDTH) {
		b->cursorX   = SKIRMISH_BASE_DEFENCE;
		b->cursorY  += b->rowHeight + 1;
		b->rowHeight = 0;
	}

	if (b->cursorY + size->height > SKIRMISH_BASE_HEIGHT) return 0xFFFF;

	lx = b->cursorX;

	b->cursorX  += size->width + 1;
	b->rowHeight = max(b->rowHeight, size->height);

	return Skirmish_Layout_Map(b, lx, b->cursorY, size->width, size->height);
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
/**
 * Concrete one tile.  Silently skips what is already paved.
 *
 * Only the footprint is billed.  Charging for the apron as well looks like the
 * honest accounting and is not: the bill falls due the moment a structure
 * completes, which is exactly when the house has just spent everything it had,
 * so it takes whatever is left and the house never accumulates.  Measured on one
 * map it cost a base sixteen harvesters and nineteen defence structures, leaving
 * it with one of each -- for three thousand credits of concrete spread over a
 * match earning eighteen thousand.  The apron is an artefact of how the plan
 * arranges a base, not a decision anybody made, so it is not charged for.
 */
static void Skirmish_LaySlab(House *h, uint16 packed, bool charge)
{
	const uint16 cost = g_table_structureInfo[STRUCTURE_SLAB_1x1].o.buildCredits;
	Tile *t = &g_map[packed];

	if (Tile_IsOutOfMap(packed)) return;
	if (Map_GetLandscapeType(packed) == LST_CONCRETE_SLAB) return;

	t->groundTileID = g_builtSlabTileID;
	t->houseID      = h->index;

	g_mapTileID[packed] |= 0x8000;

	if (Map_IsPositionUnveiled(packed)) t->overlayTileID = 0;

	Map_Update(packed, 0, false);

	if (charge) h->credits -= min(h->credits, cost);
}

static void Skirmish_LaySlabs(House *h, uint16 position, uint8 structureType)
{
	const StructureInfo *si = &g_table_structureInfo[structureType];
	const SkirmishBase *b;
	uint16 i;

	if (h == NULL) return;

	/* A wall overwrites the ground tile it stands on, so concrete underneath one
	 * is paid for and immediately lost -- but its apron is what the rule reads,
	 * and that is worth having. */
	if (structureType != STRUCTURE_WALL) {
		for (i = 0; i < g_table_structure_layoutTileCount[si->layout]; i++) {
			Skirmish_LaySlab(h, position + g_table_structure_layoutTiles[si->layout][i], true);
		}
	}

	/* And an apron one tile wide around it.
	 *
	 * The plan leaves a tile of daylight between neighbours so units can drive
	 * through the base, and that gap is exactly what breaks the "must touch
	 * something of your own" rule: a structure's ring reaches one tile, the gap
	 * is one tile, so the ring lands on bare rock and the next building over is
	 * out of reach.  Paving the ring joins the base into one connected slab as it
	 * grows, so every structure placed after the Construction Yard has something
	 * of its own to touch -- walls and turrets on the forward line included. */
	b = Skirmish_GetBase((uint8)h->index);
	if (b == NULL) return;

	for (i = 0; i < 16; i++) {
		const int16 offset = g_table_structure_layoutTilesAround[si->layout][i];
		uint16 packed;
		uint16 x, y;

		if (offset == 0) break;

		packed = position + offset;
		x = Tile_GetPackedX(packed);
		y = Tile_GetPackedY(packed);

		/* Inside the plateau only.  Concrete poured onto the sand outside it
		 * would be paid for and then erode, and it would draw the outline of the
		 * base for anyone watching. */
		if (x < b->rectX || x >= b->rectX + SKIRMISH_BASE_WIDTH) continue;
		if (y < b->rectY || y >= b->rectY + SKIRMISH_BASE_HEIGHT) continue;

		Skirmish_LaySlab(h, packed, false);
	}
}

static void Skirmish_Plan_Create(SkirmishBase *b)
{
	uint8 i;

	/* Behind the defence band, not on the corner: the band is what the enemy
	 * arrives at, and it belongs to the walls and turrets. */
	b->cursorX   = SKIRMISH_BASE_DEFENCE;
	b->cursorY   = SKIRMISH_BASE_DEFENCE;
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

		if (Skirmish_IsDefenceStructure(type)) {
			position = Skirmish_Layout_Defence(b, type);
			/* A full defence line is not a full base: keep reading the plan, the
			 * economy behind it still has room. */
			if (position == 0xFFFF) continue;
		} else {
			position = Skirmish_Layout_Next(b, type);
			if (position == 0xFFFF) break;
		}

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
	uint16 harvesters = 0;
	uint16 queued = 0;
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

	/* Count the ones inside a refinery too.  Unit_Find() hides them, and they are
	 * exactly the harvesters somebody else would be queueing behind: without this
	 * a house with two harvesters looks like a house with one whenever one of
	 * them is unloading. */
	g_validateStrictIfZero++;

	while (true) {
		const Unit *u = Unit_Find(&find);

		if (u == NULL) break;

		harvesters++;
		if (Unit_Harvester_IsQueued(u)) queued++;
	}

	g_validateStrictIfZero--;

	/* A queue, not a commute.  This used to count any harvester carrying
	 * anything, a harvester merely driving home included, so a base with one
	 * harvester and one refinery reported a queue at itself and kept asking for
	 * refineries that nothing would ever fill.  What counts now is a full
	 * harvester standing at a refinery that refuses it -- and one harvester
	 * cannot queue behind itself, whatever state the refinery is in. */
	harvesterWaiting = (queued > 0 && harvesters >= 2);

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

	/* Strictly in plan order.  A demand-driven refinery is appended to the end
	 * and waits its turn there, and that is deliberate: giving it priority over
	 * the entries in front of it was measured on one map to take a base from
	 * twenty standing defence structures to one, because a refinery is almost
	 * always pending and the defence line is almost always behind it. */
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

	/* Composition, before the budget: a doctrine that asked for a mix has to be
	 * able to refuse the type that is already over its share. */
	if (Doctrine_GetForHouse((uint8)h->index) != DOCTRINE_LEGACY && !Doctrine_AllowUnit(h, unitType)) return false;

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

/**
 * Whether *this* Heavy Factory should put a harvester on the line.
 *
 * Structure_AI_PickNextToBuild() answers "does the house want a harvester" and
 * returns one unconditionally while it does, and the fleet target in a war is
 * sixteen.  The Heavy Factory is also the only place a tank can come from, so
 * the whole of it goes on harvesters until the fleet is complete -- and then
 * again every time one dies.  Measured over a match: not one assault unit built
 * by either house before t125000, against 31 raid units from the Light Factory
 * by t100000.  That is why an attack was infantry and quads: the tanks did not
 * exist yet, in either doctrine.
 *
 * Two ways out, and this takes both.  With more than one Heavy Factory the
 * first one keeps the fleet and the rest are free for the army.  With only one,
 * it alternates once the economy is standing -- the first few harvesters still
 * come out back to back, because an economy with no harvesters has no army
 * either.
 */
#define SKIRMISH_HARVESTER_FIRST 4

bool Skirmish_AI_FactoryWantsHarvester(const House *h, const Structure *s)
{
	PoolFindStruct find;
	uint16 lowest = 0xFFFF;
	uint16 factories = 0;

	if (!s_active || h == NULL || s == NULL) return Skirmish_AI_WantsHarvester(h);
	if (!Skirmish_AI_WantsHarvester(h)) return false;

	find.houseID = (uint8)h->index;
	find.index   = 0xFFFF;
	find.type    = STRUCTURE_HEAVY_VEHICLE;

	while (true) {
		const Structure *f = Structure_Find(&find);

		if (f == NULL) break;
		if (f->o.flags.s.isNotOnMap) continue;

		factories++;
		if (f->o.index < lowest) lowest = f->o.index;
	}

	/* More than one line: the oldest keeps the fleet, the others are the army's. */
	if (factories > 1) return (s->o.index == lowest);

	/* The opening is economy first, no argument. */
	if (Skirmish_CountUnits((uint8)h->index, UNIT_HARVESTER) < SKIRMISH_HARVESTER_FIRST) return true;

	/* One line, and the army is allowed to spend: take turns. */
	if (!Skirmish_War_MilitaryAllowed(Skirmish_GetBase((uint8)h->index))) return true;

	s_harvesterTurn[h->index] = !s_harvesterTurn[h->index];

	return s_harvesterTurn[h->index];
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
/**
 * How the army should be mixed, as relative weights per unit type.
 *
 * The engine picks units by taking the maximum of priorityBuild over whatever a
 * factory can make, which is not a composition rule but a single winner: on the
 * Heavy Factory the Siege Tank's 130 beats the Launcher's 100 forever, so a
 * skirmish never saw a rocket launcher, and the Deviator's 50 -- read before the
 * Tank's 80 -- could not be reached at all.  A house therefore fielded one unit
 * type and lost to whatever countered it.
 *
 * These are shares of the army, not priorities, and zero means "somebody else
 * decides": harvesters, carryalls and the MCV are handled by their own rules.
 */
static const uint8 s_armyMix[UNIT_MAX] = {
	0,   /* Carryall     -- Skirmish_AI_WantsCarryall() */
	10,  /* Ornithopter */
	25,  /* Infantry */
	25,  /* Troopers */
	10,  /* Soldier */
	10,  /* Trooper */
	0,   /* Saboteur     -- a Palace special, never built */
	20,  /* Launcher     -- the long arm, and the whole reason for this table */
	15,  /* Deviator */
	30,  /* Tank */
	25,  /* Siege Tank */
	15,  /* Devastator */
	15,  /* Sonic Tank */
	25,  /* Trike */
	25,  /* Raider Trike */
	20,  /* Quad */
	0,   /* Harvester    -- Skirmish_AI_WantsHarvester() */
	0    /* MCV */
};

/**
 * Choose the unit a factory should build next: the one furthest below its share
 * of the army.
 *
 * Compared within the set this factory can actually build, so the Barracks and
 * the Heavy Factory each balance their own half without needing separate tables.
 * @return The unit type, or 0xFFFF to let the caller fall back to the engine.
 */
uint16 Skirmish_AI_PickUnit(const House *h, uint32 buildable)
{
	uint16 count[UNIT_MAX];
	uint32 weightSum = 0;
	uint32 total = 0;
	uint16 best = 0xFFFF;
	int32  bestScore = 0;
	uint16 i;

	if (!s_active || h == NULL) return 0xFFFF;

	/* Composition is a doctrine's business: what an army should be made of is
	 * inseparable from what it is going to be asked to do with it. */
	if (Doctrine_GetForHouse((uint8)h->index) != DOCTRINE_LEGACY) return Doctrine_PickUnit(h, buildable);

	for (i = 0; i < UNIT_MAX; i++) {
		count[i] = 0;
		if ((buildable & (1u << i)) == 0 || s_armyMix[i] == 0) continue;

		count[i] = s_unitsBuilt[h->index][i];
		weightSum += s_armyMix[i];
		total     += count[i];
	}

	if (weightSum == 0) return 0xFFFF;

	for (i = 0; i < UNIT_MAX; i++) {
		int32 want, have, score;

		if ((buildable & (1u << i)) == 0 || s_armyMix[i] == 0) continue;

		/* Shares scaled by 1000 so integer division keeps its resolution. */
		want = (int32)(s_armyMix[i] * 1000 / weightSum);
		have = (total == 0) ? 0 : (int32)(count[i] * 1000 / total);
		score = want - have;

		if (best != 0xFFFF && score <= bestScore) continue;

		best      = i;
		bestScore = score;
	}

	return best;
}

/**
 * Whether this house may start a new attack right now.
 *
 * Four teams each deciding for itself when it is ready produces four
 * independent trickles, and four trickles out of phase are a river: the shape
 * was visible in the damage but never resolved into anything.  A house holds its
 * teams until it has a wave's worth of units standing ready, then every team
 * takes a target in the same tick and they leave together.  The window stays
 * open until the wave is spent, and closes to let the next one build.
 *
 * Teams already committed are not affected -- they keep the target they have.
 */
bool Skirmish_LegacyWaveReady(uint8 houseID, uint16 waveSize)
{
	uint16 waiting = 0;
	uint16 committed = 0;
	PoolFindStruct find;

	if (!s_active || houseID >= HOUSE_MAX) return true;

	find.houseID = houseID;
	find.index   = 0xFFFF;
	find.type    = 0xFFFF;

	while (true) {
		const Team *t = Team_Find(&find);

		if (t == NULL) break;
		if (t->target != 0) committed += t->members; else waiting += t->members;
	}

	if (s_waveOpen[houseID]) {
		/* Spent: what is left of it is the seed of the next one. */
		if (committed * 3 < waveSize) s_waveOpen[houseID] = false;
		return s_waveOpen[houseID];
	}

	if (waiting >= waveSize) s_waveOpen[houseID] = true;

	return s_waveOpen[houseID];
}

bool Skirmish_AI_WaveReady(uint8 houseID)
{
	return Doctrine_WaveReady(houseID);
}

/**
 * Count one shot, classified by what it was aimed at.
 *
 * Turrets are counted apart from the rest of the base because they are the one
 * thing that is always on the way in: a wave that reaches a factory without
 * having shot at the turrets it drove past did not fight, it commuted.
 */
void Skirmish_RecordShot(uint8 houseID, uint16 target)
{
	const Structure *s;

	if (!s_active || houseID >= HOUSE_MAX) return;

	if (Tools_Index_GetType(target) == IT_UNIT) {
		s_shotsUnit[houseID]++;
		return;
	}

	s = Tools_Index_GetStructure(target);
	if (s == NULL) return;

	if (s->o.type == STRUCTURE_TURRET || s->o.type == STRUCTURE_ROCKET_TURRET) {
		s_shotsTurret[houseID]++;
		return;
	}

	s_shotsStructure[houseID]++;

	/* Was a turret in reach at that moment?  Then this shot went past it. */
	{
		PoolFindStruct find;
		uint16 reach = 8;

		find.houseID = HOUSE_INVALID;
		find.index   = 0xFFFF;
		find.type    = 0xFFFF;

		while (true) {
			const Structure *t = Structure_Find(&find);

			if (t == NULL) break;
			if (t->o.type != STRUCTURE_TURRET && t->o.type != STRUCTURE_ROCKET_TURRET) continue;
			if (House_AreAllied(houseID, t->o.houseID)) continue;
			if (Tile_GetDistanceRoundedUp(s->o.position, t->o.position) > reach) continue;

			s_shotsBypass[houseID]++;
			break;
		}
	}
}

/**
 * Combat units of this house with nothing whatsoever to do.
 *
 * No target, nowhere to be.  The first version of this asked the same question
 * only of units on a committed team, and read 1 bad sample in 164 while forty
 * vehicles stood at the factory door -- because a unit no team ever recruited
 * has no team, and was skipped.  That is not a detail: engine teams recruit one
 * unit per script pass and cap at 32 members between them, so more than half of
 * a grown army is outside them by construction, and the metric was blind to
 * exactly the population it was meant to police.
 *
 * So: every military unit, wave or no wave.  A unit standing in the base with
 * nothing to shoot is still a unit that was paid for and is not being used.
 */
static uint16 Skirmish_CountIdleAttackers(uint8 houseID, bool onWaveOnly)
{
	PoolFindStruct find;
	uint16 idle = 0;

	find.houseID = houseID;
	find.index   = 0xFFFF;
	find.type    = 0xFFFF;

	while (true) {
		const Unit *u = Unit_Find(&find);

		if (u == NULL) break;
		if (u->o.flags.s.isNotOnMap) continue;
		if (!Skirmish_IsMilitaryUnit(u->o.type)) continue;
		if (!g_table_unitInfo[u->o.type].o.flags.priority) continue;   /* Bullets are not an army. */
		if (onWaveOnly && !Doctrine_IsOnWave(u)) continue;

		if (Tools_Index_IsValid(u->targetAttack)) continue;
		if (u->targetMove != 0) continue;
		if (u->currentDestination.x != 0 || u->currentDestination.y != 0) continue;

		idle++;
	}

	return idle;
}

/** Loaded harvesters standing still with nowhere booked to unload. */
static uint16 Skirmish_CountStalledHarvesters(uint8 houseID)
{
	PoolFindStruct find;
	uint16 stalled = 0;

	find.houseID = houseID;
	find.index   = 0xFFFF;
	find.type    = UNIT_HARVESTER;

	while (true) {
		const Unit *u = Unit_Find(&find);

		if (u == NULL) break;
		if (u->o.flags.s.isNotOnMap || u->amount < 100) continue;
		if (u->targetMove != 0) continue;
		if (u->currentDestination.x != 0 || u->currentDestination.y != 0) continue;

		stalled++;
	}

	return stalled;
}

/** Refineries of this house standing free right now. */
static uint16 Skirmish_CountFreeRefineries(uint8 houseID)
{
	PoolFindStruct find;
	uint16 free = 0;

	find.houseID = houseID;
	find.index   = 0xFFFF;
	find.type    = STRUCTURE_REFINERY;

	while (true) {
		const Structure *s = Structure_Find(&find);

		if (s == NULL) break;
		if (s->o.flags.s.isNotOnMap) continue;
		if (s->state != STRUCTURE_STATE_IDLE || s->o.linkedID != 0xFF) continue;

		free++;
	}

	return free;
}

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
/**
 * Put a demand-driven entry into the plan, in front of the defence line.
 *
 * Not at the end, and not at the front either.  At the end it sits behind the
 * twenty-odd walls and turrets, and Skirmish_Economy_SampleQueue() will not ask
 * for a second refinery while the first is still pending -- so the base holds at
 * one refinery for as long as the line takes to build, which is most of the
 * match.  At the front it outranks the line permanently, and since a refinery is
 * almost always pending the line never gets built at all: measured at twenty
 * standing defence structures down to one.
 *
 * In front of the line and behind the economy is the only place that is neither.
 * One refinery at a time overtakes the turrets, and the turrets carry on.
 */
static void Skirmish_Plan_Insert(SkirmishBase *b, uint8 type, uint16 position)
{
	uint16 at = b->entryCount;
	uint16 i;

	for (i = 0; i < b->entryCount; i++) {
		if (b->entries[i].taken) continue;
		if (!Skirmish_IsDefenceStructure(b->entries[i].type)) continue;

		at = i;
		break;
	}

	for (i = b->entryCount; i > at; i--) {
		b->entries[i] = b->entries[i - 1];
	}

	b->entries[at].type     = type;
	b->entries[at].taken    = false;
	b->entries[at].position = position;
	b->entryCount++;
}

/**
 * Add a structure the base has turned out to need to its plan.
 *
 * Everything here is demand-driven -- a refinery the harvesters are queueing
 * for, a Hi-Tech to replace a lost carryall -- and goes in through
 * Skirmish_Plan_Insert(), which decides where.
 */
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

		Skirmish_Plan_Insert(b, STRUCTURE_WINDTRAP, position);

		if (b->entryCount >= SKIRMISH_PLAN_MAX) return false;
	}

	position = Skirmish_Layout_Next(b, type);
	if (position == 0xFFFF) return false;

	Skirmish_Plan_Insert(b, type, position);

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
/**
 * Drop a spice bloom on the open sand and set it off on the spot.
 *
 * Dune II has no spice regrowth whatsoever.  Two AIs mining continuously strip a
 * 62x62 map inside a long match, and what is left is a frozen board: full
 * armies, standing refineries, and nothing to put in them.  The engine does have
 * a mechanism for new spice -- a bloom fills a circle when something shoots it --
 * but nothing in UNIT.EMC makes an AI shoot one, so a bloom left standing is a
 * bloom that stays there all match.  Detonating it immediately gets the spice
 * without depending on the script to notice, and the tremor still shows where.
 */
static void Skirmish_SpiceBloom(void)
{
	uint16 attempt;

	if (g_timerGame < s_nextBloom) return;
	s_nextBloom = g_timerGame + SKIRMISH_BLOOM_TICKS;

	/* Bounded rather than exhaustive: a map with no free sand left is a map that
	 * does not need one of these, and the next period will try again. */
	for (attempt = 0; attempt < 32; attempt++) {
		const uint16 packed = Tile_PackXY(Tools_RandomLCG_Range(2, 59), Tools_RandomLCG_Range(2, 59));
		const uint16 lst = Map_GetLandscapeType(packed);

		if (lst != LST_NORMAL_SAND && lst != LST_ENTIRELY_DUNE && lst != LST_PARTIAL_DUNE) continue;

		/* Map_Bloom_ExplodeSpice() removes whatever is standing on the tile, and
		 * a harvester deleted from under its house is not a surprise a
		 * measurement should contain. */
		if (Object_GetByPackedTile(packed) != NULL) continue;

		Map_Bloom_ExplodeSpice(packed, HOUSE_INVALID);
		return;
	}
}

void Skirmish_Economy_Tick(House *h)
{
	SkirmishBase *b;

	if (!s_active || h == NULL) return;

	/* Match-global, and this is the only per-tick hook the module has.  Its own
	 * timer makes it fire once a period however many houses call in. */
	Skirmish_SpiceBloom();

	b = Skirmish_GetBase((uint8)h->index);
	if (b == NULL) return;

	Skirmish_Economy_SampleQueue(b, h);

	/* The war layer.  Deliberately after the economy sample: what a doctrine may
	 * spend has already been decided by the time it decides what to do. */
	Doctrine_Tick(h);
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

	/* Derived from where the rectangle landed rather than passed in, so all four
	 * corners and the solo case come out right without the caller having to
	 * say. */
	b->eastSide  = (rectX + SKIRMISH_BASE_WIDTH  / 2 > SKIRMISH_MAP_CENTER);
	b->southSide = (rectY + SKIRMISH_BASE_HEIGHT / 2 > SKIRMISH_MAP_CENTER);

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

	Doctrine_HouseStart(houseID);

	/* Teams are what makes an AI attack at all: on their own, factory units
	 * roll out and guard.  One team per movement type, because a team only ever
	 * recruits units that move like it does.  Economy mode has no enemy.
	 *
	 * Only doctrine A wants them.  B drives its units from C on its own clock,
	 * and an engine team underneath would fight it for the same fields -- so it
	 * gets none, and TEAM.EMC never sees its army. */
	if (!Doctrine_UsesEngineTeams(houseID)) return;

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

	/* Two plateaus plus the jitter have to fit across 62 tiles without meeting in
	 * the middle: 2*(2 + 4 + 24) is 60. */
	margin  = 2;
	jitterX = Tools_RandomLCG_Range(0, 4);
	jitterY = Tools_RandomLCG_Range(0, 4);

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
	 * decided by which corner the generator happened to favour.
	 *
	 * One field just beyond each of the two rectangle edges that face the middle
	 * of the map, next to the corner the Construction Yard now stands on.  These
	 * used to be offset right and down from the Yard unconditionally, which for
	 * a bottom-right base put both fields past the map edge, where
	 * Skirmish_SeedSpiceField() drops them without a word: that house started
	 * every match with only whatever the generator had scattered nearby. */
	for (i = 0; i < SKIRMISH_PLAYER_MAX; i++) {
		const SkirmishBase *b = &s_bases[i];
		uint16 outerX, outerY, innerX, innerY;

		if (b->entryCount == 0) continue;

		outerX = b->eastSide  ? b->rectX - 5 : b->rectX + SKIRMISH_BASE_WIDTH  + 4;
		outerY = b->southSide ? b->rectY - 5 : b->rectY + SKIRMISH_BASE_HEIGHT + 4;
		innerX = b->eastSide  ? b->rectX + 4 : b->rectX + SKIRMISH_BASE_WIDTH  - 5;
		innerY = b->southSide ? b->rectY + 4 : b->rectY + SKIRMISH_BASE_HEIGHT - 5;

		Skirmish_SeedSpiceField(outerX, innerY, 40);
		Skirmish_SeedSpiceField(innerX, outerY, 40);
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
	/* The doctrine tag goes on the war line rather than in its own overlay row:
	 * the whole reason to watch a match live is to see which phase an attack is
	 * in while it happens, and there is one line per house on screen. */
	{
		char doctrine[80];

		if (!Doctrine_GetSummary(b->houseID, doctrine, sizeof(doctrine))) strcpy(doctrine, "?");

		snprintf(buf, length, "[%s] %s mil%u%% %u/%uc %uc h%u u%u s%u@%u%% val%u %u/%u %s",
		         doctrine,
		         g_table_houseInfo[b->houseID].name,
		         (unsigned)((g_timerGame - g_tickScenarioStart >= b->plan.militarySwitchTick)
		                    ? b->plan.militaryShareLate : b->plan.militaryShare),
		         (unsigned)b->militarySpent, (unsigned)b->economySpent,
		         h->credits,
		         Skirmish_CountUnits(b->houseID, UNIT_HARVESTER), h->unitCount,
		         alive, (unsigned)((hitpointsMax != 0) ? hitpoints * 100 / hitpointsMax : 100),
		         (unsigned)Skirmish_War_GetValue(index),
		         done, b->entryCount, building);
	}

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

	/* Doctrine B has no teams to print: its roster is its own, and this is the
	 * line the self-test reads to see what an AI is doing with its army. */
	if (!Doctrine_UsesEngineTeams(b->houseID)) return Doctrine_GetSummary(b->houseID, buf, length);

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

void Skirmish_RecordDamage(uint8 houseID, uint16 damage)
{
	if (!s_active || houseID >= HOUSE_MAX) return;

	s_damageTaken[houseID] += damage;
}

/**
 * One sampled row of a match, for plotting how it actually went.
 *
 * Everything here is a level except damageTaken and spiceRefined, which are
 * cumulative on purpose: a rate is the difference between two samples, and
 * taking the difference of a running total is exact, while sampling a rate
 * directly would miss whatever happened between two samples.  Credits are a
 * level and a different question from spice refined -- a house sitting on a pile
 * of credits is one that cannot spend them, which is a diagnosis in itself.
 *
 * @return False when there is no such skirmish house.
 */
bool Skirmish_GetTelemetry(uint8 index, char *buf, uint16 length)
{
	const SkirmishBase *b;
	const House *h;
	PoolFindStruct find;
	uint16 refineries = 0;
	uint16 combatStructures = 0;
	uint16 combatUnits = 0;
	uint32 combatHitpoints = 0;
	uint16 oldValidate = g_validateStrictIfZero;

	if (!s_active || index >= SKIRMISH_PLAYER_MAX || buf == NULL) return false;

	b = &s_bases[index];
	if (b->entryCount == 0) return false;

	h = House_Get_ByIndex(b->houseID);

	find.houseID = b->houseID;
	find.index   = 0xFFFF;
	find.type    = 0xFFFF;

	while (true) {
		const Structure *s = Structure_Find(&find);

		if (s == NULL) break;
		if (s->o.flags.s.isNotOnMap) continue;

		if (s->o.type == STRUCTURE_REFINERY) refineries++;
		if (Skirmish_IsMilitaryStructure((uint8)s->o.type)) combatStructures++;
	}

	/* Count the ones inside a refinery or a repair bay too: they are alive, and
	 * a fleet that vanishes at every unload is not a useful curve. */
	g_validateStrictIfZero = 1;

	find.houseID = b->houseID;
	find.index   = 0xFFFF;
	find.type    = 0xFFFF;

	while (true) {
		const Unit *u = Unit_Find(&find);

		if (u == NULL) break;
		if (!Skirmish_IsMilitaryUnit(u->o.type)) continue;
		if (!g_table_unitInfo[u->o.type].o.flags.priority) continue;   /* Bullets are not an army. */

		combatUnits++;
		combatHitpoints += u->o.hitpoints;
	}

	g_validateStrictIfZero = oldValidate;

	{
		char doctrine[64];
		char production[96];

		if (!Doctrine_GetTelemetry(b->houseID, doctrine, sizeof(doctrine))) strcpy(doctrine, "0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
		if (!Doctrine_GetProduction(b->houseID, production, sizeof(production))) strcpy(production, "0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");

		snprintf(buf, length, "%s,%u,%u,%u,%u,%u,%u,%u,%u,%d,%u,%u,%u,%u,%u,%u,%u,%u,%s,%s",
		         g_table_houseInfo[b->houseID].name,
		         refineries, combatStructures,
		         Skirmish_CountUnits(b->houseID, UNIT_HARVESTER),
		         combatUnits, (unsigned)combatHitpoints,
		         (unsigned)s_damageTaken[b->houseID],
		         (unsigned)s_harvested[b->houseID],
		         h->credits,
		         (int)h->powerProduction - (int)h->powerUsage,
		         (unsigned)s_shotsTurret[b->houseID],
		         (unsigned)s_shotsStructure[b->houseID],
		         (unsigned)s_shotsUnit[b->houseID],
		         (unsigned)s_shotsBypass[b->houseID],
		         Skirmish_CountIdleAttackers(b->houseID, false),
		         Skirmish_CountIdleAttackers(b->houseID, true),
		         Skirmish_CountStalledHarvesters(b->houseID),
		         Skirmish_CountFreeRefineries(b->houseID),
		         doctrine, production);
	}

	return true;
}

/**
 * Record a unit death, by what killed it.
 *
 * Only two things kill a foot unit in this game: a weapon, and a tracked vehicle
 * driving over it.  Which of the two is doing the work is the difference between
 * "the AI is defending" and "the AI is walking through them on its way somewhere
 * else", and nothing on screen distinguishes them.
 */
void Skirmish_RecordKill(uint8 houseID, bool crushed)
{
	if (!s_active || houseID >= HOUSE_MAX) return;

	if (crushed) {
		s_killedByTracks[houseID]++;
	} else {
		s_killedByFire[houseID]++;
	}
}

/**
 * Losses per House, split by cause.
 * @return False when nothing has died yet.
 */
bool Skirmish_GetCasualties(char *buf, uint16 length)
{

	uint16 used = 0;
	uint8 i;

	if (!s_active || buf == NULL || length == 0) return false;

	buf[0] = '\0';

	for (i = 0; i < HOUSE_MAX; i++) {
		int written;

		if (s_killedByFire[i] == 0 && s_killedByTracks[i] == 0) continue;
		if (used + 40 >= length) break;

		written = snprintf(buf + used, length - used, "%s%s lost %u shot / %u crushed",
		                   (used == 0) ? "casualties: " : ", ",
		                   g_table_houseInfo[i].name, s_killedByFire[i], s_killedByTracks[i]);

		if (written <= 0) break;
		used += (uint16)written;
	}

	return (used != 0);
}

/**
 * Units on the map that belong to nobody playing.
 *
 * The Atreides Palace summons five HOUSE_FREMEN troopers on ACTION_HUNT every
 * 300 ticks, the Ordos one a Saboteur, and the AI fires its special the moment
 * it comes off cooldown (Structure_Update()).  They are not the summoner's
 * units, so they cost nothing against the military share and show up under a
 * House that has no base -- which on screen reads as an army arriving from
 * nowhere.  Anything here is outside the experiment.
 * @return False when there are none.
 */
bool Skirmish_GetBystanders(char *buf, uint16 length)
{
	uint16 count[HOUSE_MAX];
	PoolFindStruct find;
	uint16 used = 0;
	uint16 oldValidate = g_validateStrictIfZero;
	uint8 i;

	if (!s_active || buf == NULL || length == 0) return false;

	buf[0] = '\0';
	memset(count, 0, sizeof(count));

	g_validateStrictIfZero = 1;

	find.houseID = HOUSE_INVALID;
	find.index   = 0xFFFF;
	find.type    = 0xFFFF;

	while (true) {
		const Unit *u = Unit_Find(&find);

		if (u == NULL) break;
		if (u->o.houseID >= HOUSE_MAX) continue;
		if (Skirmish_GetBase((uint8)u->o.houseID) != NULL) continue;

		count[u->o.houseID]++;
	}

	g_validateStrictIfZero = oldValidate;

	for (i = 0; i < HOUSE_MAX; i++) {
		int written;

		if (count[i] == 0) continue;
		if (used + 24 >= length) break;

		written = snprintf(buf + used, length - used, "%s%s %u",
		                   (used == 0) ? "bystanders: " : ", ",
		                   g_table_houseInfo[i].name, count[i]);

		if (written <= 0) break;
		used += (uint16)written;
	}

	return (used != 0);
}

/**
 * Draw a line per AI over the viewport, so the match can be followed without
 * clicking around.
 */
/**
 * The one-line-per-house version, sized for the screen rather than for a log.
 *
 * The full summary is about a hundred and thirty characters; the viewport fits
 * roughly fifty, and GUI_DrawText_Wrapper() wraps rather than clipping, so the
 * two houses' lines landed on top of each other and neither could be read.
 */
static bool Skirmish_GetOverlayLine(uint8 index, char *buf, uint16 length)
{
	const SkirmishBase *b;
	const House *h;
	char doctrine[64];
	uint16 done = 0;
	uint16 i;

	if (!s_active || index >= SKIRMISH_PLAYER_MAX || buf == NULL) return false;

	b = &s_bases[index];
	if (b->entryCount == 0) return false;

	h = House_Get_ByIndex(b->houseID);

	for (i = 0; i < b->entryCount; i++) {
		if (b->entries[i].taken) done++;
	}

	if (!Doctrine_GetSummary(b->houseID, doctrine, sizeof(doctrine))) strcpy(doctrine, "?");

	snprintf(buf, length, "%.3s %s h%u u%u s%u/%u v%u",
	         g_table_houseInfo[b->houseID].name, doctrine,
	         Skirmish_CountUnits(b->houseID, UNIT_HARVESTER), h->unitCount,
	         done, b->entryCount, (unsigned)Skirmish_War_GetValue(index));

	return true;
}

void Skirmish_DrawStatusOverlay(void)
{
	uint8 i;

	if (!s_active) return;

	for (i = 0; i < SKIRMISH_PLAYER_MAX; i++) {
		char line[80];

		if (!Skirmish_GetOverlayLine(i, line, sizeof(line))) continue;

		/* Through a "%s", not as the format itself.  GUI_DrawText_Wrapper() is
		 * printf-shaped, and the summary line contains a literal percent sign --
		 * "mil90%" -- so passing it directly ate everything after it and the
		 * overlay showed the House name and nothing else. */
		GUI_DrawText_Wrapper("%s", 2, 42 + i * 8, 0xFF, 0, 0x22, line);
	}
}
