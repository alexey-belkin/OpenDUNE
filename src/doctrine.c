/** @file src/doctrine.c War doctrines: interchangeable AI battle strategies.
 *
 * Everything here is about an army that already exists: how it is grouped, when
 * it leaves, where it goes, what it shoots.  Earning the credits that paid for
 * it is the economy layer's job and stays in skirmish.c, because a battle
 * strategy is only comparable against another one when both are fed the same
 * income.
 *
 * Two doctrines live side by side:
 *
 *   A -- legacy.  What the fork did before this file existed: the engine's own
 *        Team objects driven by TEAM.EMC, one house-wide wave gate, and a fixed
 *        army mix.  Kept unchanged, as the baseline every later idea is judged
 *        against.
 *
 *   B -- echelon.  Roles instead of movement types, a muster at home, a line of
 *        departure outside the enemy's turret envelope, artillery suppression
 *        under assault escort, and only then the assault.
 *
 * "--doctrine=A,B" puts one on each side of the same map with the same seed.
 * That is the point of the file: an AI change that cannot be played against
 * what it replaced is an opinion.
 *
 * Doctrine B does not use Team at all.  It cannot: TEAM.EMC re-issues orders
 * roughly once every thousand ticks (Delay(600) at five ticks a unit, on a loop
 * that itself runs every five to twelve ticks), and recruits exactly one unit
 * per pass.  A wave needs decisions two orders of magnitude more often than
 * that, so B keeps its own roster and drives its units directly.
 */

#include <stdio.h>
#include <string.h>
#include "types.h"
#include "os/common.h"
#include "os/math.h"
#include "os/strings.h"

#include "doctrine.h"

#include "house.h"
#include "map.h"
#include "pool/house.h"
#include "pool/pool.h"
#include "pool/structure.h"
#include "pool/team.h"
#include "pool/unit.h"
#include "skirmish.h"
#include "structure.h"
#include "team.h"
#include "tile.h"
#include "timer.h"
#include "tools.h"
#include "unit.h"

/** Phases of one attack.  Every question about waiting under fire has a
 *  different answer in each of them, which is why they are separate states. */
typedef enum DoctrinePhase {
	PHASE_MUSTER   = 0,                                     /*!< Forming up at home.  Nobody is under fire. */
	PHASE_APPROACH = 1,                                     /*!< Marching to the line of departure, arrival times synchronised. */
	PHASE_SUPPRESS = 2,                                     /*!< Artillery taking the turret line down from outside its reach. */
	PHASE_ASSAULT  = 3                                      /*!< In.  Nobody waits for anybody from here on. */
} DoctrinePhase;

/**
 * The knobs of a doctrine.  Kept in a table rather than in #defines so a variant
 * is a new row: that is how C, D and the rest are meant to arrive.
 */
typedef struct DoctrineParams {
	const char *name;
	const char *summary;

	/* A only. */
	uint16 waveSize;                                        /*!< Units standing ready before the wave gate opens. */

	/* B only. */
	uint16 minWave;                                         /*!< Attackers below which a wave is not worth forming. */
	uint16 garrisonKeep;                                    /*!< Wave-capable units held back to defend the base. */
	uint16 ldStandoff;                                      /*!< Tiles between the line of departure and the nearest turret. */
	uint16 columnHold;                                      /*!< Hold the leaders once the column is longer than this. */
	uint16 columnRelease;                                   /*!< ...and let them go again below this. */
	uint16 releasePercent;                                  /*!< Share of the wave's strength at the LD that releases it. */
	uint16 abortPercent;                                    /*!< Strength below which the wave gives up and re-forms. */
	uint32 graceTicks;                                      /*!< Longest a phase may wait for stragglers. */
	uint16 escortDistance;                                  /*!< How far behind the artillery the assault stands. */
	uint16 picketPercent;                                   /*!< Most of the reserve that may be posted to flank spice.  0 disables it. */
	uint16 picketFrom;                                      /*!< Reserve size above which posting starts at all. */
	uint16 etaSlack;                                        /*!< Arrival-time difference treated as "together". */
	uint16 assaultRatio;                                    /*!< Percent of the enemy's defence a wave must be worth before it goes in. */
	uint32 assaultTicks;                                    /*!< Longest one assault may run before the wave is spent. */
	uint16 garrisonCap;                                     /*!< Garrison-role units alive before the Barracks is told to stop. */
	uint8  roleShare[DOCTRINE_ROLE_MAX];                    /*!< Army composition the factories build towards. */
} DoctrineParams;

static const DoctrineParams s_doctrine[DOCTRINE_MAX] = {
	{
		"A", "legacy: engine teams, one wave gate, fixed army mix",
		12,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		{ 0, 0, 0, 0 }
	},
	{
		"B", "echelon: roles, line of departure, artillery suppression",
		0,
		/* minWave */        6,
		/* garrisonKeep */   0,
		/* ldStandoff */     14,
		/* columnHold */     10,
		/* columnRelease */  6,
		/* releasePercent */ 60,
		/* abortPercent */   40,
		/* graceTicks */     1500,
		/* escortDistance */ 3,
		/* picketPercent */  30,
		/* picketFrom */     20,
		/* etaSlack */       600,
		/* assaultRatio */   100,
		/* assaultTicks */   12000,
		/* garrisonCap */    10,
		/* artillery, assault, raid, garrison */
		{ 20, 45, 20, 15 }
	}
};

/** Which doctrine each house is playing.  Indexed by houseID. */
static uint8 s_doctrineOf[HOUSE_MAX];
/** As selected on the command line, by base index rather than by House. */
static uint8 s_doctrineByIndex[SKIRMISH_PLAYER_MAX];

/** Per-unit bookkeeping for doctrine B.  Cleared when a unit dies, because the
 *  pool hands the slot straight to the next unit built. */
static uint8 s_unitRole[UNIT_INDEX_MAX];
static uint8 s_unitOnWave[UNIT_INDEX_MAX];
/** Roles a house has ever been able to build.  The share of one it cannot reach
 *  yet belongs to the others, or the whole army waits on a technology. */
static uint8 s_roleSeen[HOUSE_MAX][DOCTRINE_ROLE_MAX];
/* Production diagnostics.  "The base is not building tanks" is a sentence with
 * at least four different causes -- nothing asked for one, the veto refused it,
 * the factory is gone, or the credits went elsewhere -- and they are not
 * distinguishable from the army that comes out. */
static uint32 s_pickedRole[HOUSE_MAX][DOCTRINE_ROLE_MAX];
static uint32 s_vetoedRole[HOUSE_MAX][DOCTRINE_ROLE_MAX];
/* Times a unit had to be pushed back out of an enemy turret's envelope.  The
 * direct measure of the rule: it should fall towards zero as units learn to
 * stop wandering in, and every one of them is a free shot given away. */
/** Coarse hostile-presence map, one cell per 4x4 tiles.  Rebuilt on a timer
 *  because a harvester asks this from inside a map-wide search. */
#define DANGER_CELLS 16
static uint8  s_danger[HOUSE_MAX][DANGER_CELLS * DANGER_CELLS];
static uint32 s_dangerUntil[HOUSE_MAX];

static uint32 s_turretZone[HOUSE_MAX];
/* Entries, as distinct from time spent.  The counter above ticks once per unit
 * tick and so measures how long units stand in an envelope; the goal is about
 * how often they cross into one, which is a different number and the only one a
 * rule can be held to.  s_inZone remembers which side of the line each unit was
 * on last tick. */
static uint32 s_turretEntries[HOUSE_MAX];
static uint8  s_inZone[UNIT_INDEX_MAX];
/* Ticks during which a turned-back unit is left alone: without it the doctrine
 * re-issues on its next pass the very order that sent it there. */
static uint32 s_turnedBack[UNIT_INDEX_MAX];
/* Assault units posted to a flank spice field instead of standing in the yard.
 * They are still wave material -- a muster takes them back -- but until then
 * they hold ground that pays, off the line the attacks run down. */
static uint8  s_unitPicket[UNIT_INDEX_MAX];
/* Where each unit stood at its last check, so a turret finished on top of a unit
 * is not booked as the unit walking into one. */
static uint16 s_lastTile[UNIT_INDEX_MAX];
/* The tile a unit was told to leave by, kept until it is either reached or no
 * longer clear.  Re-deciding it every call is re-planning, not walking. */
static uint16 s_exitTile[UNIT_INDEX_MAX];
/* Tick each unit was last hit by a turret, so a death can be attributed to one.
 * Nothing in Unit_Damage() is told who fired; the bullet knows, and this is
 * where that is written down before it is lost. */
static uint32 s_hitByTurret[UNIT_INDEX_MAX];
/* Units lost to turrets, split by whether they were where they were supposed to
 * be.  The first must be zero: a unit killed by a turret outside a wave's
 * assault was somewhere it had no business being. */
static uint32 s_killedByTurretLoose[HOUSE_MAX];
static uint32 s_killedByTurretAssault[HOUSE_MAX];
/* Enemy turrets destroyed.  Against the two above it answers the only question
 * that matters about attacking a defence line: was it paid for. */
static uint32 s_turretsKilled[HOUSE_MAX];
/* Harvesters lost, and enemy harvesters killed.  The two halves of "money likes
 * quiet": ours should fall, theirs should rise. */
static uint32 s_harvesterLost[HOUSE_MAX];
static uint32 s_harvesterKilled[HOUSE_MAX];
/* Harvesters lost before t100000.  The goal is zero: everything before that
 * tick is the economy being built, and a harvester lost then is worth several
 * lost later. */
static uint32 s_harvesterLostEarly[HOUSE_MAX];
/* Doctrine ticks with at least one harvester within eight tiles of something
 * that shoots.  Losses are the outcome; this is the exposure that produces them,
 * and it moves long before the outcome does -- which is what makes it usable as
 * a test rather than as a post-mortem. */
static uint32 s_harvesterExposed[HOUSE_MAX];
static uint32 s_harvesterSamples[HOUSE_MAX];
/* Cohesion, sampled while an assault is under way: how many of the House's
 * attackers were on the wave, against how many it had.  "They do not all go
 * together" is a complaint about this ratio and nothing else. */
static uint32 s_cohesionOn[HOUSE_MAX];
static uint32 s_cohesionAll[HOUSE_MAX];

typedef struct DoctrineHouse {
	uint8  phase;
	uint32 phaseStart;
	uint32 nextTick;

	uint16 objective;                                       /*!< Encoded structure the wave is out to kill. */
	uint16 suppressTarget;                                  /*!< Encoded turret the artillery is working on. */
	uint16 ldPacked;                                        /*!< Line of departure. */
	uint16 musterPacked;                                    /*!< Where the next wave forms up. */

	uint32 waveHpStart;                                     /*!< Wave strength when it left, for the abort test. */
	uint32 waveHpLast;                                      /*!< Last sample, so a drop reads as contact. */

	/* Telemetry. */
	uint16 waveCount;
	uint16 atLD;
	uint16 columnLength;
	uint32 wavesLaunched;
	uint32 wavesAborted;
	uint32 wavesDeclined;                                   /*!< Counted itself against the line and stayed home. */
	uint32 firstAssault;                                    /*!< Tick the first assault began, or 0. */
	uint32 declineUntil;
	uint32 attackStrength;                                  /*!< Last reading of the trigger, so it can be read off the screen. */
	uint32 defenceStrength;                                    /*!< Not worth re-asking before this tick. */
	uint32 suppressShots;
	uint8  enemy;
} DoctrineHouse;

static DoctrineHouse s_house[HOUSE_MAX];

/** Army size below which composition is not enforced at all. */
#define DOCTRINE_MIX_FLOOR 12

/** How often a doctrine re-decides, in game ticks.  Two orders of magnitude
 *  more often than TEAM.EMC, which is the whole reason B exists. */
#define DOCTRINE_TICK 30

/* -------------------------------------------------------------------------- */
/* Selection                                                                   */
/* -------------------------------------------------------------------------- */

DoctrineID Doctrine_ParseName(const char *name)
{
	uint8 i;

	if (name == NULL) return DOCTRINE_INVALID;

	for (i = 0; i < DOCTRINE_MAX; i++) {
		if (strcasecmp(s_doctrine[i].name, name) == 0) return (DoctrineID)i;
	}

	return DOCTRINE_INVALID;
}

const char *Doctrine_GetName(DoctrineID id)
{
	if (id >= DOCTRINE_MAX) return "?";
	return s_doctrine[id].name;
}

void Doctrine_SetForIndex(uint8 index, DoctrineID id)
{
	if (index >= SKIRMISH_PLAYER_MAX || id >= DOCTRINE_MAX) return;
	s_doctrineByIndex[index] = (uint8)id;
}

/**
 * Read "--doctrine=B" or "--doctrine=A,B".  One name gives both houses the same
 * doctrine; two names put one on each side, which is the A/B match.
 */
bool Doctrine_ParseArgument(const char *arg)
{
	char buf[64];
	char *comma;
	DoctrineID first, second;

	if (arg == NULL || *arg == '\0') return false;

	strncpy(buf, arg, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';

	comma = strchr(buf, ',');
	if (comma != NULL) *comma = '\0';

	first = Doctrine_ParseName(buf);
	if (first == DOCTRINE_INVALID) return false;

	/* "--doctrine=B," is one name with a stray comma, not two.  Rejecting it
	 * left both houses on the default, silently as far as anyone watching the
	 * game is concerned -- the warning goes to a terminal nobody is reading
	 * while a GUI match is on screen. */
	second = first;
	if (comma != NULL && comma[1] != '\0') {
		second = Doctrine_ParseName(comma + 1);
		if (second == DOCTRINE_INVALID) return false;
	}

	Doctrine_SetForIndex(0, first);
	Doctrine_SetForIndex(1, second);

	return true;
}

void Doctrine_GetSelection(char *buf, uint16 length)
{
	if (buf == NULL || length == 0) return;

	snprintf(buf, length, "%s,%s",
	         Doctrine_GetName((DoctrineID)s_doctrineByIndex[0]),
	         Doctrine_GetName((DoctrineID)s_doctrineByIndex[1]));
}

DoctrineID Doctrine_GetForHouse(uint8 houseID)
{
	if (houseID >= HOUSE_MAX) return DOCTRINE_LEGACY;
	return (DoctrineID)s_doctrineOf[houseID];
}

static const DoctrineParams *Doctrine_ParamsOf(uint8 houseID)
{
	return &s_doctrine[Doctrine_GetForHouse(houseID)];
}

/* When the current match started, on a clock that never restarts.
 *
 * g_timerGame runs for the life of the process, and a search plays hundreds of
 * matches inside one: everything the doctrine phrases as "by tick N" was true of
 * the first match and of no other.  Resetting the global was tried and broke the
 * economy outright -- too much of the engine stamps absolute ticks -- so the
 * baseline is kept here and every deadline is measured against it. */
static uint32 s_matchStart;

/** Ticks since this match began. */
static uint32 Doctrine_Elapsed(void)
{
	return (g_timerGame > s_matchStart) ? (g_timerGame - s_matchStart) : 0;
}

void Doctrine_Reset(void)
{
	s_matchStart = g_timerGame;
	memset(s_doctrineOf, DOCTRINE_LEGACY, sizeof(s_doctrineOf));
	memset(s_unitRole, DOCTRINE_ROLE_NONE, sizeof(s_unitRole));
	memset(s_unitOnWave, 0, sizeof(s_unitOnWave));
	memset(s_roleSeen, 0, sizeof(s_roleSeen));
	memset(s_pickedRole, 0, sizeof(s_pickedRole));
	memset(s_vetoedRole, 0, sizeof(s_vetoedRole));
	memset(s_turretZone, 0, sizeof(s_turretZone));
	memset(s_turretEntries, 0, sizeof(s_turretEntries));
	memset(s_inZone, 0, sizeof(s_inZone));
	memset(s_turnedBack, 0, sizeof(s_turnedBack));
	memset(s_unitPicket, 0, sizeof(s_unitPicket));
	memset(s_lastTile, 0, sizeof(s_lastTile));
	memset(s_exitTile, 0, sizeof(s_exitTile));
	memset(s_hitByTurret, 0, sizeof(s_hitByTurret));
	memset(s_killedByTurretLoose, 0, sizeof(s_killedByTurretLoose));
	memset(s_killedByTurretAssault, 0, sizeof(s_killedByTurretAssault));
	memset(s_turretsKilled, 0, sizeof(s_turretsKilled));
	memset(s_harvesterLost, 0, sizeof(s_harvesterLost));
	memset(s_harvesterKilled, 0, sizeof(s_harvesterKilled));
	memset(s_harvesterLostEarly, 0, sizeof(s_harvesterLostEarly));
	memset(s_harvesterExposed, 0, sizeof(s_harvesterExposed));
	memset(s_harvesterSamples, 0, sizeof(s_harvesterSamples));
	memset(s_cohesionOn, 0, sizeof(s_cohesionOn));
	memset(s_cohesionAll, 0, sizeof(s_cohesionAll));
	memset(s_danger, 0, sizeof(s_danger));
	memset(s_dangerUntil, 0, sizeof(s_dangerUntil));
	memset(s_house, 0, sizeof(s_house));
}

/** Bind the doctrine chosen for a base slot to the House that took it. */
void Doctrine_HouseStart(uint8 houseID)
{
	uint8 index;

	if (houseID >= HOUSE_MAX) return;

	for (index = 0; index < SKIRMISH_PLAYER_MAX; index++) {
		if (Skirmish_GetBaseHouse(index) != houseID) continue;
		s_doctrineOf[houseID] = s_doctrineByIndex[index];
		break;
	}

	memset(&s_house[houseID], 0, sizeof(DoctrineHouse));
	s_house[houseID].phase = PHASE_MUSTER;
}

bool Doctrine_UsesEngineTeams(uint8 houseID)
{
	return (Doctrine_GetForHouse(houseID) == DOCTRINE_LEGACY);
}

void Doctrine_ForgetUnit(uint16 unitIndex)
{
	if (unitIndex >= UNIT_INDEX_MAX) return;
	s_unitRole[unitIndex] = DOCTRINE_ROLE_NONE;
	s_unitOnWave[unitIndex] = 0;
	s_inZone[unitIndex] = 0;
	s_exitTile[unitIndex] = 0;
	s_hitByTurret[unitIndex] = 0;
	s_unitPicket[unitIndex] = 0;
}

/* -------------------------------------------------------------------------- */
/* Roles                                                                       */
/* -------------------------------------------------------------------------- */

/**
 * Which role a unit type belongs to.
 *
 * The split is by what the unit can do about a turret, and by how fast it gets
 * there -- not by movementType, which mixes both.  MOVEMENT_TRACKED spans the
 * Launcher at an effective 13.1 tiles of speed and the Devastator at 4.4: a
 * three-fold spread inside one "type", which is why arrival times were never
 * going to line up while teams were cut that way.
 *
 * A gun Turret acquires at 5 tiles and a Rocket Turret at 8 (the arguments to
 * FindTargetUnit in BUILD.EMC).  Exactly two units in the game reach further
 * than 8: the Launcher at 9, and the Sonic Tank at 8 for a draw.  Those two are
 * the artillery, and they are the only reason a base's defence line can be taken
 * down without feeding it.
 */
static uint8 Doctrine_RoleOf(uint16 type)
{
	switch (type) {
		case UNIT_LAUNCHER:
		case UNIT_SONIC_TANK:
			return DOCTRINE_ROLE_ARTILLERY;

		/* Ordos can build neither of the above -- the Launcher's availableHouse
		 * covers everyone but Ordos, and the Sonic Tank is Atreides only -- so
		 * the one House whose whole identity is indirection has no way to touch a
		 * turret line without standing inside it.  Its answer is the Saboteur,
		 * and the numbers say it is a better one: entering a structure calls
		 * Structure_Damage(s, 500), against a Rocket Turret's 200 hitpoints, so
		 * one of them removes any turret in the game outright.  It walks at 17.5
		 * on sand, faster than a Tank, and Unit_GetTileSpeed() gives it 255
		 * through a wall.  It is artillery that costs itself instead of costing
		 * time. */
		case UNIT_SABOTEUR:
			return DOCTRINE_ROLE_ARTILLERY;

		case UNIT_TANK:
		case UNIT_SIEGE_TANK:
		case UNIT_DEVIATOR:
			return DOCTRINE_ROLE_ASSAULT;

		case UNIT_TRIKE:
		case UNIT_RAIDER_TRIKE:
		case UNIT_QUAD:
			return DOCTRINE_ROLE_RAID;

		/* These were put here because they could not keep up: Infantry moved at
		 * 2.2 against a Tank's 10.9 and a Devastator's 4.4, so synchronising the
		 * whole spread meant the fast half idling for most of the march.
		 *
		 * The class-balance module now derives light infantry speed from rocket
		 * infantry (Unit_CombatBalance_LightInfantrySpeed()), which at the
		 * default puts Infantry at 5.2 and Soldier at 7.9 -- past the Devastator
		 * and up against the Siege Tank's 8.8.  Rocket infantry is still the slow
		 * half at 4.4 and 6.6.  The assignment below has not been re-measured
		 * against those numbers; it is inherited, not derived. */
		case UNIT_INFANTRY:
		case UNIT_TROOPERS:
		case UNIT_SOLDIER:
		case UNIT_TROOPER:
		case UNIT_DEVASTATOR:
			return DOCTRINE_ROLE_GARRISON;

		default:
			return DOCTRINE_ROLE_NONE;
	}
}

/** Effective speed on open sand: the terrain rate for this movement type scaled
 *  by the unit's own factor, which is how Unit_GetTileSpeed() computes it. */
static uint16 Doctrine_Speed(uint16 type)
{
	const UnitInfo *ui = &g_table_unitInfo[type];
	uint16 ground = g_table_landscapeInfo[LST_NORMAL_SAND].movementSpeed[ui->movementType];
	uint16 speed = (uint16)((uint32)ground * ui->movingSpeedFactor / 256);

	return (speed == 0) ? 1 : speed;
}

bool Doctrine_IsTurretTarget(uint16 encoded)
{
	const Structure *s = Tools_Index_GetStructure(encoded);

	if (s == NULL) return false;

	return (s->o.type == STRUCTURE_TURRET || s->o.type == STRUCTURE_ROCKET_TURRET);
}

/** Whether an encoded index names a turret. */
static bool Doctrine_IsTurret(uint16 encoded)
{
	const Structure *s = Tools_Index_GetStructure(encoded);

	if (s == NULL) return false;

	return (s->o.type == STRUCTURE_TURRET || s->o.type == STRUCTURE_ROCKET_TURRET);
}

/**
 * Who a wave is made of.
 *
 * Not the raiders.  They have a job that pays better than standing in a line of
 * battle -- a Trike reaches three tiles and dies to anything, but it is the
 * fastest thing on the map and an enemy harvester is unarmed and standing where
 * its owner cannot do without it.  Pulling them into an assault costs the
 * strangling and buys almost no weight, so they stay out and keep hunting.
 *
 * Everything else that is not garrison goes, all of it, every time.
 */
static bool Doctrine_IsAttacker(uint8 role)
{
	return (role == DOCTRINE_ROLE_ARTILLERY || role == DOCTRINE_ROLE_ASSAULT);
}

bool Doctrine_IsOnWave(const Unit *u)
{
	if (u == NULL || u->o.index >= UNIT_INDEX_MAX) return false;

	/* Under the legacy doctrine a wave is a Team with a target, which is what
	 * Unit_Skirmish_ClearTheWay() has always asked. */
	if (Doctrine_GetForHouse(u->o.houseID) == DOCTRINE_LEGACY) {
		const Team *t;

		if (u->team == 0) return false;
		t = Team_Get_ByIndex(u->team - 1);
		return (t != NULL && t->target != 0);
	}

	return (s_unitOnWave[u->o.index] != 0);
}

/* -------------------------------------------------------------------------- */
/* Doctrine A -- the legacy wave gate                                          */
/* -------------------------------------------------------------------------- */

bool Doctrine_WaveReady(uint8 houseID)
{
	/* B does not gate teams, it has no teams; the opcode that asks this only
	 * runs for a house that has them. */
	if (Doctrine_GetForHouse(houseID) != DOCTRINE_LEGACY) return true;

	return Skirmish_LegacyWaveReady(houseID, s_doctrine[DOCTRINE_LEGACY].waveSize);
}

/* -------------------------------------------------------------------------- */
/* Doctrine B -- geometry helpers                                              */
/* -------------------------------------------------------------------------- */

/** The house this one is fighting, or HOUSE_INVALID when there is nobody. */
static uint8 Doctrine_EnemyOf(uint8 houseID)
{
	uint8 index;

	for (index = 0; index < SKIRMISH_PLAYER_MAX; index++) {
		uint8 other = Skirmish_GetBaseHouse(index);

		if (other == HOUSE_INVALID || other == houseID) continue;
		if (House_AreAllied(houseID, other)) continue;

		return other;
	}

	return HOUSE_INVALID;
}

/** Nearest enemy turret to a tile, as an encoded index.  Distance in tiles is
 *  returned through @p distance when it is not NULL. */
static uint16 Doctrine_NearestTurret(uint8 houseID, uint16 packed, uint16 *distance)
{
	PoolFindStruct find;
	uint16 best = 0;
	uint16 bestDistance = 0xFFFF;

	find.houseID = HOUSE_INVALID;
	find.index   = 0xFFFF;
	find.type    = 0xFFFF;

	while (true) {
		const Structure *s = Structure_Find(&find);
		uint16 d;

		if (s == NULL) break;
		if (s->o.type != STRUCTURE_TURRET && s->o.type != STRUCTURE_ROCKET_TURRET) continue;
		if (House_AreAllied(houseID, s->o.houseID)) continue;
		if (s->o.flags.s.isNotOnMap) continue;

		d = Tile_GetDistancePacked(packed, Tile_PackTile(s->o.position));
		if (d >= bestDistance) continue;

		bestDistance = d;
		best = Tools_Index_Encode(s->o.index, IT_STRUCTURE);
	}

	if (distance != NULL) *distance = (best == 0) ? 0xFFFF : bestDistance;

	return best;
}

/**
 * Pick the face of the enemy plateau to come in on: the one with the fewest
 * turrets covering it.
 *
 * This is the deep flank, and it is cheap because the base plan only fortifies
 * the two faces that look at the middle of the map -- measured, one house took
 * 1165 shots at buildings across a match with not one of them fired past a
 * turret, because on its approach there were none.  The other two faces are
 * open, and finding them is counting.
 */
static uint16 Doctrine_PickApproach(uint8 houseID, uint8 enemy)
{
	uint16 rectX, rectY, width, height;
	uint16 best = 0;
	int32 bestScore = 0x7FFFFFFF;
	uint16 ownOrigin = 0;
	uint8 index;
	uint8 face;

	if (!Skirmish_GetBaseRect(enemy, &rectX, &rectY, &width, &height)) return 0;

	for (index = 0; index < SKIRMISH_PLAYER_MAX; index++) {
		if (Skirmish_GetBaseHouse(index) != houseID) continue;
		ownOrigin = Skirmish_GetBaseOrigin(index);
	}

	for (face = 0; face < 4; face++) {
		int32 cx = rectX + width / 2;
		int32 cy = rectY + height / 2;
		uint16 packed;
		uint16 turrets = 0;
		PoolFindStruct find;
		int32 score;

		/* Six tiles clear of the plateau edge, so the probe sits on open ground
		 * rather than on the base itself. */
		switch (face) {
			case 0: cx = (int32)rectX - 6;                  break;  /* west  */
			case 1: cx = (int32)rectX + width + 6;          break;  /* east  */
			case 2: cy = (int32)rectY - 6;                  break;  /* north */
			default: cy = (int32)rectY + height + 6;        break;  /* south */
		}

		if (cx < 2) cx = 2;
		if (cy < 2) cy = 2;
		if (cx > 61) cx = 61;
		if (cy > 61) cy = 61;

		packed = Tile_PackXY((uint16)cx, (uint16)cy);

		find.houseID = enemy;
		find.index   = 0xFFFF;
		find.type    = 0xFFFF;

		while (true) {
			const Structure *s = Structure_Find(&find);

			if (s == NULL) break;
			if (s->o.type != STRUCTURE_TURRET && s->o.type != STRUCTURE_ROCKET_TURRET) continue;
			if (Tile_GetDistancePacked(packed, Tile_PackTile(s->o.position)) > 12) continue;
			turrets++;
		}

		/* Turrets dominate; the walk is the tie-break, so an equally open face
		 * closer to home wins.  Weighted so no amount of distance on a 62x62 map
		 * buys its way past a single turret. */
		score = (int32)turrets * 100;
		if (ownOrigin != 0) score += Tile_GetDistancePacked(ownOrigin, packed);

		if (score >= bestScore) continue;

		bestScore = score;
		best = packed;
	}

	return best;
}

/**
 * The line of departure: a tile on the chosen approach, far enough out that the
 * wave can stand there and form up without being shot at.
 *
 * This is what answers "hold under fire or fall back": neither, because the
 * waiting happens outside the envelope by construction.  A Rocket Turret reaches
 * 8, so the standoff is 14 -- six tiles of margin for a turret we have not seen
 * yet and for the spread of a formation.
 */
static uint16 Doctrine_FindLD(uint8 houseID, uint16 approach, uint16 objective)
{
	const DoctrineParams *p = Doctrine_ParamsOf(houseID);
	uint16 objectivePacked;
	uint16 packed = approach;
	uint8 step;

	if (objective == 0) return approach;
	objectivePacked = Tools_Index_GetPackedTile(objective);

	for (step = 0; step < 20; step++) {
		uint16 distance;
		tile32 tile;

		Doctrine_NearestTurret(houseID, packed, &distance);
		if (distance >= p->ldStandoff) return packed;

		/* One tile further from the objective, along the line we came in on. */
		tile = Tile_MoveByDirection(Tile_UnpackTile(packed),
		                            Tile_GetDirection(Tile_UnpackTile(objectivePacked), Tile_UnpackTile(packed)),
		                            1 << 8);
		if (Tile_IsOutOfMap(Tile_PackTile(tile))) break;

		packed = Tile_PackTile(tile);
	}

	return packed;
}

/**
 * What the wave is out to kill.
 *
 * Turrets are never an objective -- they are the obstacle on the way, handled by
 * the artillery in PHASE_SUPPRESS and by every unit's own sweep after that.
 * What is left is a ladder: take the money while the enemy can still fight back,
 * take the factories when the money is gone, and only go for the Construction
 * Yard once there is nothing left to punish it.
 */
static uint16 Doctrine_PickObjective(uint8 houseID, uint8 enemy, uint16 approach, uint16 waveCount)
{
	static const uint8 ladder[3][4] = {
		{ STRUCTURE_REFINERY,     STRUCTURE_INVALID,     STRUCTURE_INVALID,   STRUCTURE_INVALID },
		{ STRUCTURE_HEAVY_VEHICLE, STRUCTURE_LIGHT_VEHICLE, STRUCTURE_STARPORT, STRUCTURE_BARRACKS },
		{ STRUCTURE_CONSTRUCTION_YARD, STRUCTURE_INVALID, STRUCTURE_INVALID,  STRUCTURE_INVALID }
	};

	uint16 enemyUnits = Skirmish_CountCombatUnits(enemy);
	uint8 order[3];
	uint8 rung;

	/* Weak enough to finish: go for the head.  Otherwise the Yard is the worst
	 * possible objective, sitting behind everything the enemy owns. */
	if (enemyUnits * 2 < waveCount) {
		order[0] = 2; order[1] = 1; order[2] = 0;
	} else {
		order[0] = 0; order[1] = 1; order[2] = 2;
	}

	for (rung = 0; rung < 3; rung++) {
		const uint8 *types = ladder[order[rung]];
		PoolFindStruct find;
		uint16 best = 0;
		uint16 bestDistance = 0xFFFF;

		find.houseID = enemy;
		find.index   = 0xFFFF;
		find.type    = 0xFFFF;

		while (true) {
			const Structure *s = Structure_Find(&find);
			uint16 d;
			uint8 i;
			bool wanted = false;

			if (s == NULL) break;
			if (s->o.flags.s.isNotOnMap) continue;

			for (i = 0; i < 4; i++) {
				if (types[i] == STRUCTURE_INVALID) continue;
				if (s->o.type == types[i]) { wanted = true; break; }
			}
			if (!wanted) continue;

			/* Nearest to the face we picked, not to us: the approach was chosen
			 * for being open, and an objective on the far side would drag the
			 * wave back around the base.
			 *
			 * An objective no turret covers counts as forty tiles nearer than
			 * one that is.  This is the flank, and it is worth walking a long way
			 * for: taking a refinery nobody is guarding costs a drive, and taking
			 * the one behind the line costs the wave. */
			d = Tile_GetDistancePacked(approach, Tile_PackTile(s->o.position));
			if (Doctrine_CoveringTurret(houseID, Tile_PackTile(s->o.position)) == 0) {
				d = (d > 40) ? (uint16)(d - 40) : 0;
			}
			if (d >= bestDistance) continue;

			bestDistance = d;
			best = Tools_Index_Encode(s->o.index, IT_STRUCTURE);
		}

		if (best != 0) return best;
	}

	return 0;
}

/* -------------------------------------------------------------------------- */
/* Doctrine B -- danger, patrol, and the turret exclusion zone                 */
/* -------------------------------------------------------------------------- */

/** How far a turret of this type engages, in tiles.  These are the arguments to
 *  FindTargetUnit in BUILD.EMC, which is the only place they exist. */
static uint16 Doctrine_TurretReach(uint16 structureType)
{
	if (structureType == STRUCTURE_ROCKET_TURRET) return 8;
	if (structureType == STRUCTURE_TURRET) return 5;
	return 0;
}

static void Doctrine_BuildDanger(uint8 houseID)
{
	PoolFindStruct find;
	uint8 *cell = s_danger[houseID];
	uint16 i;

	memset(cell, 0, DANGER_CELLS * DANGER_CELLS);
	s_dangerUntil[houseID] = g_timerGame + 150;

	find.houseID = HOUSE_INVALID;
	find.index   = 0xFFFF;
	find.type    = 0xFFFF;

	while (true) {
		Unit *u = Unit_Find(&find);
		uint16 cx, cy;

		if (u == NULL) break;
		if (u->o.flags.s.isNotOnMap) continue;
		if (House_AreAllied(houseID, Unit_GetHouseID(u))) continue;
		if (!g_table_unitInfo[u->o.type].o.flags.priority) continue;      /* Not a bullet. */
		if (g_table_unitInfo[u->o.type].fireDistance == 0) continue;      /* Not a threat. */

		cx = Tile_GetPackedX(Tile_PackTile(u->o.position)) / 4;
		cy = Tile_GetPackedY(Tile_PackTile(u->o.position)) / 4;
		if (cx >= DANGER_CELLS || cy >= DANGER_CELLS) continue;

		if (cell[cy * DANGER_CELLS + cx] < 250) cell[cy * DANGER_CELLS + cx] += 4;
	}

	/* Turrets are permanent danger and worth more than a passing tank. */
	find.houseID = HOUSE_INVALID;
	find.index   = 0xFFFF;
	find.type    = 0xFFFF;

	while (true) {
		const Structure *s = Structure_Find(&find);
		uint16 cx, cy;

		if (s == NULL) break;
		if (Doctrine_TurretReach(s->o.type) == 0) continue;
		if (House_AreAllied(houseID, s->o.houseID)) continue;

		cx = Tile_GetPackedX(Tile_PackTile(s->o.position)) / 4;
		cy = Tile_GetPackedY(Tile_PackTile(s->o.position)) / 4;
		if (cx >= DANGER_CELLS || cy >= DANGER_CELLS) continue;

		if (cell[cy * DANGER_CELLS + cx] < 240) cell[cy * DANGER_CELLS + cx] += 10;
	}

	/* One pass of bleed into the neighbours, so the edge of a fight is not a
	 * safe place to park a harvester either. */
	for (i = 0; i < DANGER_CELLS * DANGER_CELLS; i++) {
		uint16 x = i % DANGER_CELLS;
		uint16 y = i / DANGER_CELLS;
		uint16 spread = cell[i] / 2;

		if (spread == 0) continue;

		if (x > 0                && cell[i - 1] < spread) cell[i - 1] = (uint8)spread;
		if (x < DANGER_CELLS - 1 && cell[i + 1] < spread) cell[i + 1] = (uint8)spread;
		if (y > 0                && cell[i - DANGER_CELLS] < spread) cell[i - DANGER_CELLS] = (uint8)spread;
		if (y < DANGER_CELLS - 1 && cell[i + DANGER_CELLS] < spread) cell[i + DANGER_CELLS] = (uint8)spread;
	}
}

/**
 * Tiles from a position to the nearest hostile thing that can shoot, capped
 * at 99.  Exact, unlike the danger grid, because both the metric and the rule
 * that acts on it have to agree about what "near" means.
 */
uint16 Doctrine_ThreatDistance(uint8 houseID, uint16 packed)
{
	PoolFindStruct find;
	uint16 best = 99;

	if (houseID >= HOUSE_MAX) return 99;

	find.houseID = HOUSE_INVALID;
	find.index   = 0xFFFF;
	find.type    = 0xFFFF;

	while (true) {
		Unit *u = Unit_Find(&find);
		uint16 d;

		if (u == NULL) break;
		if (u->o.flags.s.isNotOnMap) continue;
		if (House_AreAllied(houseID, Unit_GetHouseID(u))) continue;
		if (!g_table_unitInfo[u->o.type].o.flags.priority) continue;
		if (g_table_unitInfo[u->o.type].fireDistance == 0) continue;

		d = Tile_GetDistancePacked(packed, Tile_PackTile(u->o.position));
		if (d < best) best = d;
	}

	find.houseID = HOUSE_INVALID;
	find.index   = 0xFFFF;
	find.type    = 0xFFFF;

	while (true) {
		const Structure *s = Structure_Find(&find);
		uint16 reach, d;

		if (s == NULL) break;

		reach = Doctrine_TurretReach(s->o.type);
		if (reach == 0) continue;
		if (House_AreAllied(houseID, s->o.houseID)) continue;
		if (s->o.flags.s.isNotOnMap) continue;

		/* Measured from the edge of its reach, so "eight tiles from a turret"
		 * means the same thing as "eight tiles from a tank". */
		d = Tile_GetDistancePacked(packed, Tile_PackTile(s->o.position));
		d = (d > reach) ? (uint16)(d - reach) : 0;
		if (d < best) best = d;
	}

	return best;
}

/**
 * How dangerous a tile is for this House, 0 upwards.
 *
 * Money likes quiet.  A harvester that drives into a battle is not a harvester
 * any more, and the spice it was going for is still there afterwards -- so the
 * cost of walking further is almost always smaller than the cost of the trip
 * that does not come back.
 */
uint16 Doctrine_DangerAt(uint8 houseID, uint16 packed)
{
	uint16 cx, cy;

	if (houseID >= HOUSE_MAX) return 0;
	if (Doctrine_GetForHouse(houseID) == DOCTRINE_LEGACY) return 0;

	if (s_dangerUntil[houseID] <= g_timerGame) Doctrine_BuildDanger(houseID);

	cx = Tile_GetPackedX(packed) / 4;
	cy = Tile_GetPackedY(packed) / 4;
	if (cx >= DANGER_CELLS || cy >= DANGER_CELLS) return 0;

	return s_danger[houseID][cy * DANGER_CELLS + cx];
}

/**
 * The enemy turret whose reach covers a tile, if any, as an encoded index.
 *
 * Every order the doctrine issues has to ask this before it issues it: sending a
 * unit somewhere covered and then pushing it back out on the next tick is not a
 * rule, it is a loop, and it ran twenty-three thousand times in a single match.
 */
uint16 Doctrine_CoveringTurret(uint8 houseID, uint16 packed)
{
	PoolFindStruct find;
	uint16 best = 0;
	uint16 bestReach = 0;

	find.houseID = HOUSE_INVALID;
	find.index   = 0xFFFF;
	find.type    = 0xFFFF;

	while (true) {
		const Structure *s = Structure_Find(&find);
		uint16 reach;

		if (s == NULL) break;

		reach = Doctrine_TurretReach(s->o.type);
		if (reach == 0) continue;
		if (House_AreAllied(houseID, s->o.houseID)) continue;
		if (s->o.flags.s.isNotOnMap) continue;
		if (Tile_GetDistancePacked(packed, Tile_PackTile(s->o.position)) > reach) continue;

		if (reach <= bestReach) continue;

		bestReach = reach;
		best = Tools_Index_Encode(s->o.index, IT_STRUCTURE);
	}

	return best;
}

/**
 * Whether this unit is one of the few allowed inside a turret's reach.
 *
 * Exactly one case: committed to a wave, and that wave in the assault.  That is
 * the organised attack the doctrine is willing to spend units on -- anyone else
 * standing there is a unit dying alone for nothing.
 */
bool Doctrine_MayEnterTurretZone(const Unit *u)
{
	uint8 houseID;

	if (u == NULL || u->o.index >= UNIT_INDEX_MAX) return false;

	/* Unit_GetHouseID() wants a mutable unit and this is a question, not an
	 * order; a deviated unit is not on anybody's wave anyway. */
	houseID = u->o.houseID;
	if (houseID >= HOUSE_MAX) return false;

	return (s_unitOnWave[u->o.index] != 0 && s_house[houseID].phase == PHASE_ASSAULT);
}

/** Whether the ground an encoded target stands on is covered by an enemy turret. */
bool Doctrine_TargetIsCovered(uint8 houseID, uint16 encoded)
{
	if (encoded == 0 || !Tools_Index_IsValid(encoded)) return false;

	return (Doctrine_CoveringTurret(houseID, Tools_Index_GetPackedTile(encoded)) != 0);
}

/** Whether this unit is standing inside an enemy turret's reach right now. */
bool Doctrine_IsInTurretZone(const Unit *u)
{
	if (u == NULL || u->o.index >= UNIT_INDEX_MAX) return false;
	if (Doctrine_GetForHouse(u->o.houseID) == DOCTRINE_LEGACY) return false;

	return (s_inZone[u->o.index] != 0);
}

/** Whether this unit was just pushed out of an envelope and is still leaving. */
bool Doctrine_IsLeaving(const Unit *u)
{
	if (u == NULL || u->o.index >= UNIT_INDEX_MAX) return false;
	if (Doctrine_GetForHouse(u->o.houseID) == DOCTRINE_LEGACY) return false;

	return (s_turnedBack[u->o.index] > g_timerGame);
}

/**
 * Keep a unit out of a turret's reach unless that turret is what it came for.
 *
 * A tank fighting another tank drifts, and the ground it drifts onto is often
 * covered by something that shoots for free: the tank is busy, the turret is
 * not, and the exchange is one-sided.  The rule is the blunt one -- you are
 * inside the envelope only when the turret is your target -- and it is the same
 * rule the wave already follows, applied to everyone all the time.
 *
 * Backing off rather than engaging is deliberate.  A Tank reaches 4 and a Rocket
 * Turret 8, so "fight it where you stand" is a losing trade for everything but
 * the artillery; the unit that should engage a turret is the one whose job it
 * is, and Unit_Skirmish_ClearTheWay() has already given it that target -- at
 * which point this does not fire.
 *
 * @return True when the unit was pushed out.
 */
bool Doctrine_TurretExclusion(Unit *unit)
{
	const UnitInfo *ui;
	PoolFindStruct find;
	const Structure *worst = NULL;
	uint16 worstReach = 0;
	uint16 distance;
	uint16 margin;
	bool inside = false;
	uint8 houseID;

	if (unit == NULL || unit->o.index >= UNIT_INDEX_MAX || unit->o.flags.s.isNotOnMap) return false;

	houseID = Unit_GetHouseID(unit);
	if (Doctrine_GetForHouse(houseID) == DOCTRINE_LEGACY) return false;

	ui = &g_table_unitInfo[unit->o.type];
	if (!ui->flags.isNormalUnit || !ui->flags.isGroundUnit) return false;
	if (unit->o.type == UNIT_SABOTEUR) return false;                 /* Its job is to arrive. */

	/* Harvesters stay in.  They read as normal ground units and the harvester
	 * layer has its own, wider rule about keeping away from guns, so letting the
	 * fence take them too looks like two systems arguing over one unit -- but it
	 * was tried and it costs: exposure doubled from 8 per cent of samples to 16
	 * and both doctrines' spice moved with it.  The fence turns a harvester at
	 * the edge of a gun's reach faster than a flee that only re-decides when the
	 * harvest cycle next asks. */

	/* The one case that is allowed in: a unit committed to a wave, and that wave
	 * in the assault.  That is the organised attack the whole doctrine is built
	 * to produce, and inside it the turrets are the wave's problem.
	 *
	 * The first version of this permitted only a unit whose own target was the
	 * particular turret covering it, which reads as the stricter rule and is
	 * actually the opposite: an assault is a wave going at a factory, so every
	 * unit in it is shooting something that is not a turret, so the fence pushed
	 * the entire assault back out of the base it had just been ordered into.
	 * They walked in under orders, were turned round under the rule, walked in
	 * again, and the counter recorded the argument -- 27195 ticks inside per
	 * match, taking fire the whole way, for an attack that never landed. */
	if (Doctrine_MayEnterTurretZone(unit)) return false;

	/* The fence has to be as wide as the ground a unit covers between two looks.
	 *
	 * This runs under tickUnknown4, which fires every twenty game ticks, and a
	 * Raider Trike moves at an effective 37.5 against a Tank's 10.9 -- so a
	 * one-tile fence is something the fast half of the army steps straight over
	 * and is already inside by the time anyone asks.  Scaled by speed, the fast
	 * units get turned early enough to actually turn. */
	margin = (uint16)(1 + Doctrine_Speed(unit->o.type) / 8);
	if (margin > 6) margin = 6;

	find.houseID = HOUSE_INVALID;
	find.index   = 0xFFFF;
	find.type    = 0xFFFF;

	while (true) {
		const Structure *s = Structure_Find(&find);
		uint16 reach;

		if (s == NULL) break;

		reach = Doctrine_TurretReach(s->o.type);
		if (reach == 0) continue;
		if (House_AreAllied(houseID, s->o.houseID)) continue;
		if (s->o.flags.s.isNotOnMap) continue;


		/* Turned back a tile early, on purpose.
		 *
		 * Gating every order the doctrine issues took one seed from 318 crossings
		 * to 23 and made another worse, because what was left is not an order at
		 * all: a unit whose start and finish are both clear still walks the line
		 * between them, and the engine's pathfinder has never heard of a turret.
		 * There is no way to ask it for a detour, so the fence goes one tile
		 * outside the envelope and units are turned at it.
		 *
		 * The counter below still measures the real edge, so the number stays
		 * honest about what it claims: crossings, not near misses. */
		distance = Tile_GetDistanceRoundedUp(unit->o.position, s->o.position);
		if (distance > reach + margin) continue;

		if (distance <= reach) inside = true;

		if (reach > worstReach) {
			worstReach = reach;
			worst = s;
		}
	}

	/* Track the crossing whichever way it goes, so an entry is counted once
	 * rather than once per tick, and a unit that leaves can be counted in again
	 * when it comes back. */
	if (worst == NULL) {
		s_inZone[unit->o.index] = 0;
		s_exitTile[unit->o.index] = 0;
		return false;
	}

	/* Inside with nothing to shoot at is not the violation this is about.
	 *
	 * It is overwhelmingly one situation: the unit came in to kill a turret, the
	 * turret died, and another one still covers the ground it is standing on.
	 * Pushing it out to walk back in is worse than useless -- it is already
	 * there and the line is still up -- so it takes the covering turret and
	 * carries on, which is what the rule permits and what the doctrine wanted.
	 *
	 * The violation the rule exists for is the other one: inside while shooting
	 * at something that is not a turret.  That is a tank that drifted in chasing
	 * a tank, taking free fire from a gun it is not even fighting, and that is
	 * what gets pushed out and counted. */
	if (inside) {
		/* Only when the unit moved into it.  A turret finishing next to a unit
		 * that has not moved puts it inside without it having gone anywhere, and
		 * booking that as a crossing measures the enemy's construction rather
		 * than our own discipline.  It still leaves; it is just not a breach. */
		const bool moved = (s_lastTile[unit->o.index] != Tile_PackTile(unit->o.position));

		if (s_inZone[unit->o.index] == 0 && moved) s_turretEntries[houseID]++;

		s_inZone[unit->o.index] = 1;
		s_turretZone[houseID]++;
	}

	s_lastTile[unit->o.index] = Tile_PackTile(unit->o.position);

	s_turnedBack[unit->o.index] = g_timerGame + 90;

	/* Out to the nearest tile clear of every envelope, not merely away from this
	 * one.
	 *
	 * Pushing straight out from whichever turret was found first left units
	 * inside a second turret's reach, and where the line is dense -- which is the
	 * entire point of a line -- that is most of them.  Searched outward instead,
	 * so a unit leaves by the shortest way out rather than the obvious one. */
	{
		const uint16 here = Tile_PackTile(unit->o.position);
		const uint16 x = Tile_GetPackedX(here);
		const uint16 y = Tile_GetPackedY(here);
		uint16 out = 0;
		int16 ring;

		/* The way out is chosen once and kept.
		 *
		 * The search below takes the first clear tile of the innermost clear ring,
		 * which moves as the unit does -- so recomputing it on every call handed a
		 * different destination every few ticks, the unit re-planned instead of
		 * walking, and it stood in the envelope re-planning until something killed
		 * it.  The trace read the same each time: ACTION_MOVE, a fresh targetMove,
		 * and a position that had not changed.
		 *
		 * So the tile survives between calls and is only re-chosen when it stops
		 * being clear, or when the unit is out and the state is dropped. */
		if (s_exitTile[unit->o.index] != 0
			&& Doctrine_CoveringTurret(houseID, s_exitTile[unit->o.index]) == 0) {
			out = s_exitTile[unit->o.index];
		}

		for (ring = 1; ring <= 14 && out == 0; ring++) {
			int16 dx, dy;

			for (dy = (int16)-ring; dy <= ring && out == 0; dy++) {
				for (dx = (int16)-ring; dx <= ring; dx++) {
					uint16 packed;
					int32 nx, ny;

					if (dx > -ring && dx < ring && dy > -ring && dy < ring) continue;

					nx = (int32)x + dx;
					ny = (int32)y + dy;
					if (nx < 1 || ny < 1 || nx > 62 || ny > 62) continue;

					packed = Tile_PackXY((uint16)nx, (uint16)ny);
					if (Doctrine_CoveringTurret(houseID, packed) != 0) continue;
					if (Unit_GetTileEnterScore(unit, packed, 0) > 255) continue;

					out = packed;
					break;
				}
			}
		}

		/* Nowhere clear within fourteen tiles: keep walking away from the worst
		 * of them rather than turning to fight.  One unit against a turret loses,
		 * whatever the situation -- there is no case where that trade is worth
		 * making alone. */
		if (out == 0) {
			out = Tile_PackTile(Tile_MoveByDirection(unit->o.position,
			                                         Tile_GetDirection(worst->o.position, unit->o.position),
			                                         (uint16)((worstReach + 2) << 8)));
		}

		/* Inside the envelope, leaving is an order.  Outside it -- in the margin,
		 * which is the tile or six of slack the fence is built with -- it is a
		 * destination and nothing more.
		 *
		 * That difference is the whole fix.  The first version never forced the
		 * action on a unit that was attacking something, reasoning that walking
		 * out should not mean walking out unarmed; what it meant in practice is
		 * that nothing walked out at all.  ACTION_ATTACK re-approaches its target
		 * every time the script runs and overwrites the destination underneath,
		 * so the unit stood in the envelope and fought while a gun shot it for
		 * free.  Measured: 37085 ticks spent inside against 40 crossings, which
		 * is not forty visits but a handful of units that got in and never came
		 * out, and the counter was reporting discipline while measuring a siege.
		 *
		 * Keeping the fence advisory in the margin is deliberate and is the other
		 * half of the trade: a rocket turret plus a Raider's margin reaches
		 * fourteen tiles, and forcing a move at fourteen tiles is how an army
		 * stops fighting anywhere near the enemy line at all. */
		if (inside) {
			/* The target goes only if it is standing in the envelope too.  Then
			 * there is no clear ground to shoot it from and holding it is just a
			 * promise to walk back in; anything else is re-acquired the moment
			 * the unit is somewhere legal, which is exactly what should happen. */
			/* Assigned rather than set through Unit_SetTarget(): that one takes an
			 * encoded index and rejects an invalid one, so "no target" is not
			 * something it can be asked for. */
			if (Tools_Index_IsValid(unit->targetAttack)
				&& Doctrine_CoveringTurret(houseID, Tools_Index_GetPackedTile(unit->targetAttack)) != 0) {
				unit->targetAttack = 0;
			}

			if (unit->actionID != ACTION_MOVE) Unit_SetAction(unit, ACTION_MOVE);
		} else if (unit->actionID != ACTION_ATTACK || !Tools_Index_IsValid(unit->targetAttack)) {
			if (unit->actionID != ACTION_MOVE) Unit_SetAction(unit, ACTION_MOVE);
		}

		s_exitTile[unit->o.index] = out;
		Unit_SetDestination(unit, Tools_Index_Encode(out, IT_TILE));
	}

	return true;
}

/* -------------------------------------------------------------------------- */
/* Doctrine B -- roster                                                        */
/* -------------------------------------------------------------------------- */

/** Sum of a role's members, its centre of mass and its slowest speed. */
typedef struct RoleGroup {
	uint16 count;
	uint16 centre;                                          /*!< Packed tile. */
	uint16 slowest;
	uint32 hitpoints;
	uint16 order[UNIT_INDEX_MAX];
} RoleGroup;

static void Doctrine_Gather(uint8 houseID, uint8 role, bool onWave, RoleGroup *g)
{
	PoolFindStruct find;
	uint32 sumX = 0, sumY = 0;

	memset(g, 0, sizeof(RoleGroup));
	g->slowest = 0xFFFF;

	find.houseID = houseID;
	find.index   = 0xFFFF;
	find.type    = 0xFFFF;

	while (true) {
		Unit *u = Unit_Find(&find);
		uint16 speed;

		if (u == NULL) break;
		if (u->o.index >= UNIT_INDEX_MAX || u->o.flags.s.isNotOnMap) continue;
		if (s_unitRole[u->o.index] != role) continue;
		if ((s_unitOnWave[u->o.index] != 0) != onWave) continue;

		speed = Doctrine_Speed(u->o.type);
		if (speed < g->slowest) g->slowest = speed;

		sumX += Tile_GetPackedX(Tile_PackTile(u->o.position));
		sumY += Tile_GetPackedY(Tile_PackTile(u->o.position));
		g->hitpoints += u->o.hitpoints;
		g->order[g->count++] = u->o.index;
	}

	if (g->count == 0) {
		g->slowest = 1;
		return;
	}

	g->centre = Tile_PackXY((uint16)(sumX / g->count), (uint16)(sumY / g->count));
}

/** Give every military unit of this house a role it does not have yet. */
static void Doctrine_AssignRoles(uint8 houseID)
{
	PoolFindStruct find;

	find.houseID = houseID;
	find.index   = 0xFFFF;
	find.type    = 0xFFFF;

	while (true) {
		Unit *u = Unit_Find(&find);
		uint8 role;

		if (u == NULL) break;
		if (u->o.index >= UNIT_INDEX_MAX) continue;
		if (s_unitRole[u->o.index] != DOCTRINE_ROLE_NONE) continue;

		role = Doctrine_RoleOf(u->o.type);
		if (role == DOCTRINE_ROLE_NONE) continue;

		s_unitRole[u->o.index] = role;
	}
}

/* -------------------------------------------------------------------------- */
/* Doctrine B -- orders                                                        */
/* -------------------------------------------------------------------------- */

/** Whether this unit was just turned back and should be left to finish leaving. */
static bool Doctrine_JustTurnedBack(const Unit *u)
{
	return (u->o.index < UNIT_INDEX_MAX && s_turnedBack[u->o.index] > g_timerGame);
}

static void Doctrine_OrderMove(Unit *u, uint16 packed)
{
	if (u->actionID != ACTION_MOVE) Unit_SetAction(u, ACTION_MOVE);
	Unit_SetDestination(u, Tools_Index_Encode(packed, IT_TILE));
}

static void Doctrine_OrderHold(Unit *u)
{
	if (u->actionID == ACTION_ATTACK && Tools_Index_IsValid(u->targetAttack)) return;
	if (u->actionID != ACTION_GUARD) Unit_SetAction(u, ACTION_GUARD);
	Unit_SetDestination(u, Tools_Index_Encode(Tile_PackTile(u->o.position), IT_TILE));
}

static void Doctrine_OrderAttack(Unit *u, uint16 encoded, uint16 standoffPacked)
{
	if (u->actionID != ACTION_ATTACK) Unit_SetAction(u, ACTION_ATTACK);
	Unit_SetTarget(u, encoded);
	Unit_SetDestination(u, Tools_Index_Encode(standoffPacked, IT_TILE));
}

/**
 * March a group to a tile, keeping it together.
 *
 * The leaders are held once the column is longer than columnHold tiles, and let
 * go again below columnRelease -- hysteresis, or the formation stutters every
 * tick.  Ten and six are not guesses: a Rocket Turret engages at 8, so a column
 * shorter than that enters the envelope as one body, and a longer one is fed to
 * the turret a vehicle at a time.  Which is exactly what a river between the
 * bases looks like.
 */
static uint16 Doctrine_MarchTo(uint8 houseID, RoleGroup *g, uint16 destination, bool cohesion)
{
	const DoctrineParams *p = Doctrine_ParamsOf(houseID);
	uint16 nearest = 0xFFFF;
	uint16 furthest = 0;
	uint16 column;
	uint16 i;

	if (g->count == 0) return 0;

	for (i = 0; i < g->count; i++) {
		const Unit *u = Unit_Get_ByIndex(g->order[i]);
		uint16 d;

		if (u == NULL) continue;
		d = Tile_GetDistancePacked(Tile_PackTile(u->o.position), destination);
		if (d < nearest) nearest = d;
		if (d > furthest) furthest = d;
	}

	column = (furthest > nearest) ? (uint16)(furthest - nearest) : 0;

	UnitSelection_SortOrderByDistance(g->order, g->count, destination);
	UnitSelection_SpreadReset();

	for (i = 0; i < g->count; i++) {
		Unit *u = Unit_Get_ByIndex(g->order[i]);
		uint16 d;

		if (u == NULL || Doctrine_JustTurnedBack(u)) continue;

		d = Tile_GetDistancePacked(Tile_PackTile(u->o.position), destination);

		/* A leader waits for the tail; a straggler never waits. */
		if (cohesion && column > p->columnHold && d < nearest + p->columnRelease) {
			Doctrine_OrderHold(u);
			continue;
		}

		{
			uint16 spot = UnitSelection_SpreadTake(u, destination);

			/* A march never ends inside an envelope, and never answers one by
			 * attacking it: a single unit against a turret is a unit thrown away.
			 * Turrets are killed by a wave in the assault or not at all. */
			if (Doctrine_CoveringTurret(houseID, spot) != 0) {
				Doctrine_OrderHold(u);
				continue;
			}

			Doctrine_OrderMove(u, spot);
		}
	}

	return column;
}

/** Everything this house owns that could join a wave but has not. */
static uint16 Doctrine_CountReserve(uint8 houseID, uint32 *hitpoints)
{
	PoolFindStruct find;
	uint16 count = 0;

	if (hitpoints != NULL) *hitpoints = 0;

	find.houseID = houseID;
	find.index   = 0xFFFF;
	find.type    = 0xFFFF;

	while (true) {
		const Unit *u = Unit_Find(&find);

		if (u == NULL) break;
		if (u->o.index >= UNIT_INDEX_MAX || u->o.flags.s.isNotOnMap) continue;
		if (s_unitOnWave[u->o.index] != 0) continue;
		if (!Doctrine_IsAttacker(s_unitRole[u->o.index])) continue;
		if (u->o.type == UNIT_SABOTEUR) continue;                  /* Never part of a muster. */

		count++;
		if (hitpoints != NULL) *hitpoints += u->o.hitpoints;
	}

	return count;
}

static uint16 Doctrine_CountWave(uint8 houseID, uint32 *hitpoints)
{
	PoolFindStruct find;
	uint16 count = 0;

	if (hitpoints != NULL) *hitpoints = 0;

	find.houseID = houseID;
	find.index   = 0xFFFF;
	find.type    = 0xFFFF;

	while (true) {
		const Unit *u = Unit_Find(&find);

		if (u == NULL) break;
		if (u->o.index >= UNIT_INDEX_MAX || u->o.flags.s.isNotOnMap) continue;
		if (s_unitOnWave[u->o.index] == 0) continue;

		count++;
		if (hitpoints != NULL) *hitpoints += u->o.hitpoints;
	}

	return count;
}

static void Doctrine_DismissWave(uint8 houseID)
{
	PoolFindStruct find;

	find.houseID = houseID;
	find.index   = 0xFFFF;
	find.type    = 0xFFFF;

	while (true) {
		const Unit *u = Unit_Find(&find);

		if (u == NULL) break;
		if (u->o.index >= UNIT_INDEX_MAX) continue;
		s_unitOnWave[u->o.index] = 0;
	}
}

/* -------------------------------------------------------------------------- */
/* Doctrine B -- the phases                                                    */
/* -------------------------------------------------------------------------- */

/**
 * What the enemy has to be got through, in hitpoints: everything that shoots,
 * plus the line itself.
 */
/** Everything this House could put into a wave, in hitpoints: every unit whose
 *  role is not garrison, whether it is already committed or not. */
static uint32 Doctrine_AttackStrength(uint8 houseID)
{
	PoolFindStruct find;
	uint32 total = 0;

	find.houseID = houseID;
	find.index   = 0xFFFF;
	find.type    = 0xFFFF;

	while (true) {
		const Unit *u = Unit_Find(&find);

		if (u == NULL) break;
		if (u->o.index >= UNIT_INDEX_MAX || u->o.flags.s.isNotOnMap) continue;
		if (!Doctrine_IsAttacker(s_unitRole[u->o.index])) continue;
		if (u->o.type == UNIT_SABOTEUR) continue;

		total += u->o.hitpoints;
	}

	return total;
}

static uint32 Doctrine_DefenceStrength(uint8 houseID, uint8 enemy, uint16 objective, uint16 turretWeight)
{
	PoolFindStruct find;
	uint32 total = 0;
	uint16 aim = (objective != 0) ? Tools_Index_GetPackedTile(objective) : 0;

	find.houseID = enemy;
	find.index   = 0xFFFF;
	find.type    = 0xFFFF;

	/* Mobile units count by where they are.
	 *
	 * A wave does not have to beat the enemy's whole army -- it has to beat what
	 * is at the objective, plus whatever can get back there in time.  Counting
	 * every unit on the map at full weight made the test say "not yet" with
	 * twelve artillery and twenty-five assault standing ready, because the
	 * defender's own raiders, out hunting on the far side of the map, were being
	 * counted as though they were parked on the objective.
	 *
	 * Discounting the distant ones was tried and measured worse -- three wins in
	 * six became two, because a defender does concentrate and arrives home before
	 * a wave that has to cross the map.  They count in full, wherever they
	 * are. */
	while (true) {
		const Unit *u = Unit_Find(&find);

		if (u == NULL) break;
		if (u->o.flags.s.isNotOnMap) continue;
		if (!g_table_unitInfo[u->o.type].o.flags.priority) continue;
		if (g_table_unitInfo[u->o.type].fireDistance == 0) continue;

		total += u->o.hitpoints;
	}

	find.houseID = enemy;
	find.index   = 0xFFFF;
	find.type    = 0xFFFF;

	/* Turrets, unlike units, cannot be somewhere else -- so only the ones the
	 * wave is going to meet count.
	 *
	 * Counting the whole line was the mistake that made the test unreachable: a
	 * grown base has twenty-odd turrets spread over two faces, the wave meets
	 * perhaps five of them on the approach it chose, and weighing all twenty at
	 * double their hitpoints put the bar four times higher than the fight it
	 * describes.  With twelve artillery and twenty-five assault standing ready
	 * the test still said no, and the army went on accumulating for the rest of
	 * the match.
	 *
	 * Twelve tiles around the objective is the envelope the assault has to live
	 * inside; a turret outside that is somebody else's problem. */
	while (turretWeight != 0) {
		const Structure *s = Structure_Find(&find);

		if (s == NULL) break;
		if (Doctrine_TurretReach(s->o.type) == 0) continue;
		if (s->o.flags.s.isNotOnMap) continue;
		if (aim != 0 && Tile_GetDistancePacked(aim, Tile_PackTile(s->o.position)) > 20) continue;

		/* Worth double its hitpoints: it fires for free from outside the reach of
		 * nearly everything, so removing it costs more than a tank of the same
		 * size.  Three shapes of this test were measured over six seeds --
		 * turrets at 1.5x within twelve tiles, the same with distant units
		 * discounted, and this one -- and the strictest won most: three wins in
		 * six against two.  For a House that cannot outrange a turret, patience
		 * is not timidity, it is the only edge it has. */
		total += (uint32)s->o.hitpoints * turretWeight;
	}

	VARIABLE_NOT_USED(houseID);

	return total;
}

/**
 * Everything built after a wave left still belongs to it, until it goes in.
 *
 * The muster commits what exists at that moment and never looked again, so a
 * wave that spent thirty thousand ticks marching and suppressing was the army as
 * it stood when it formed, while the factories kept filling the yard behind it.
 * Measured mid-assault: seven units attacking, twenty-two standing in the
 * muster, seventeen garrison idle -- a quarter of the army fighting.
 *
 * Reinforcement stops at the assault, deliberately.  Before it there is a
 * gathering point to catch up to and the time to do it in; after it, a unit sent
 * alone across open ground into a base arrives alone, which is the trickle this
 * doctrine exists to end.  What is built during an assault is the next wave.
 */
static void Doctrine_Reinforce(uint8 houseID, DoctrineHouse *dh)
{
	const DoctrineParams *p = Doctrine_ParamsOf(houseID);
	PoolFindStruct find;
	uint16 reserve;
	uint16 taken = 0;

	if (dh->phase != PHASE_APPROACH && dh->phase != PHASE_SUPPRESS) return;

	reserve = Doctrine_CountReserve(houseID, NULL);
	if (reserve <= p->garrisonKeep) return;

	find.houseID = houseID;
	find.index   = 0xFFFF;
	find.type    = 0xFFFF;

	while (true) {
		Unit *u = Unit_Find(&find);

		if (u == NULL) break;
		if (u->o.index >= UNIT_INDEX_MAX || u->o.flags.s.isNotOnMap) continue;
		if (s_unitOnWave[u->o.index] != 0) continue;
		if (!Doctrine_IsAttacker(s_unitRole[u->o.index])) continue;
		if (u->o.type == UNIT_SABOTEUR) continue;
		if (taken + p->garrisonKeep >= reserve) break;

		s_unitOnWave[u->o.index] = 1;
		taken++;
	}

	if (taken != 0) dh->waveCount = Doctrine_CountWave(houseID, NULL);
}

static void Doctrine_EnterPhase(DoctrineHouse *dh, uint8 phase)
{
	if (phase == PHASE_ASSAULT && dh->firstAssault == 0) dh->firstAssault = Doctrine_Elapsed();

	dh->phase = phase;
	dh->phaseStart = g_timerGame;
}

/** Where the next wave forms up: outside the base, on the side the enemy is. */
static uint16 Doctrine_MusterPoint(uint8 houseID, uint8 enemy)
{
	uint16 rally = Skirmish_GetBaseRally(houseID);
	uint8 index;

	VARIABLE_NOT_USED(enemy);

	if (rally != 0) return rally;

	for (index = 0; index < SKIRMISH_PLAYER_MAX; index++) {
		if (Skirmish_GetBaseHouse(index) != houseID) continue;
		return Skirmish_GetBaseOrigin(index);
	}

	return 0;
}

/**
 * Muster: hold everything at the rally until there is a wave's worth of it.
 *
 * The reserve is what the base keeps: garrison-role units never leave, and
 * garrisonKeep attackers stay with them.  A house that sends literally everything
 * wins the race to the enemy factory and loses its own base to the wave coming
 * the other way.
 */
static void Doctrine_PhaseMuster(uint8 houseID, DoctrineHouse *dh, uint8 enemy)
{
	const DoctrineParams *p = Doctrine_ParamsOf(houseID);
	uint16 reserve;
	uint32 reserveHp = 0;
	uint16 keep;
	uint16 approach;
	uint16 objective;
	bool uncovered;
	PoolFindStruct find;
	uint16 taken = 0;

	dh->musterPacked = Doctrine_MusterPoint(houseID, enemy);
	if (dh->musterPacked == 0) return;

	reserve = Doctrine_CountReserve(houseID, &reserveHp);
	if (reserve < p->minWave) return;

	/* Weighed here, before anybody moves.
	 *
	 * Deciding this at the line of departure meant the wave formed, marched
	 * forty tiles, looked at what it was facing, and went home -- hundreds of
	 * times a match.  From outside that is not a wave and not a decision, it is
	 * a column shuttling back and forth, which is exactly the trickle this
	 * doctrine exists to replace.
	 *
	 * Declining also has to stick for a while.  Re-asking the same question
	 * thirty ticks later gets the same answer and re-forms the same wave, so a
	 * house that is not strong enough waits and keeps raiding instead. */
	if (dh->declineUntil > g_timerGame) return;

	approach = Doctrine_PickApproach(houseID, enemy);
	if (approach == 0) return;

	objective = Doctrine_PickObjective(houseID, enemy, approach, reserve);
	if (objective == 0) return;

	/* Against everything this House could send, not against what happens to be
	 * unassigned.
	 *
	 * Measured against the reserve alone, the test answered a question nobody
	 * asked -- the reserve shrinks every time a wave forms -- and the army piled
	 * up almost without limit waiting for a number that the reserve could not
	 * reach on its own.  One assault at the end of a match is not a strategy, it
	 * is a stall.  The aggregate is the honest quantity: if everything that is
	 * not garrison cannot beat the line, nothing can, and the raiders keep
	 * strangling instead. */
	/* A way in that no turret covers is not a fight with the line, so the line
	 * is not in the price.
	 *
	 * The objective picker already prefers an uncovered target by a wide margin,
	 * and when it finds one the wave is going somewhere the defence was not
	 * built to hold -- charging it what the turrets cost would refuse an attack
	 * on the grounds of guns that will never fire at it.  Covered, they count
	 * double as before. */
	uncovered = (Doctrine_CoveringTurret(houseID, Tools_Index_GetPackedTile(objective)) == 0);

	dh->attackStrength = Doctrine_AttackStrength(houseID);
	dh->defenceStrength = Doctrine_DefenceStrength(houseID, enemy, objective, uncovered ? 1 : 2);

	if (dh->attackStrength * 100 < dh->defenceStrength * p->assaultRatio) {
		dh->wavesDeclined++;
		dh->declineUntil = g_timerGame + 600;
		return;
	}

	/* The keep comes out of what is left above the minimum, not on top of it.
	 * Demanding minWave + garrisonKeep before anything moves put the bar at ten
	 * attackers, and a B house that was losing never reached it again: it stood
	 * in its base with seven of them and launched nothing for the rest of the
	 * match. */
	keep = (reserve > (uint16)(p->minWave + p->garrisonKeep)) ? p->garrisonKeep : (uint16)(reserve - p->minWave);

	dh->objective = objective;
	dh->ldPacked  = Doctrine_FindLD(houseID, approach, objective);

	/* Commit everybody but the garrison keep.  Sorted by distance to the muster
	 * point so what stays behind is what is already standing in the base. */
	find.houseID = houseID;
	find.index   = 0xFFFF;
	find.type    = 0xFFFF;

	while (true) {
		Unit *u = Unit_Find(&find);

		if (u == NULL) break;
		if (u->o.index >= UNIT_INDEX_MAX || u->o.flags.s.isNotOnMap) continue;
		if (!Doctrine_IsAttacker(s_unitRole[u->o.index])) continue;
		if (u->o.type == UNIT_SABOTEUR) continue;
		if (taken + keep >= reserve) break;

		s_unitOnWave[u->o.index] = 1;
		s_unitPicket[u->o.index] = 0;
		taken++;
	}

	dh->waveCount = Doctrine_CountWave(houseID, &dh->waveHpStart);
	dh->waveHpLast = dh->waveHpStart;
	dh->wavesLaunched++;
	dh->atLD = 0;

	Doctrine_EnterPhase(dh, PHASE_APPROACH);
}

/**
 * Approach: every role leaves so that all of them arrive together.
 *
 * A role holds while its own travel time to the line of departure is shorter
 * than the slowest role's, and starts when the gap closes.  Recomputed every
 * pass, so a group that gets held up on the way pulls the others back to it
 * automatically -- which is the answer to "does everybody wait while one team
 * sorts itself out": yes, but only here, only outside the envelope, and only
 * until the grace runs out.
 *
 * Contact cancels the synchronisation outright.  Standing still once the enemy
 * knows where you are only gives them time to concentrate, and the whole point
 * of arriving together is surprise that has already been spent.
 */
static void Doctrine_PhaseApproach(uint8 houseID, DoctrineHouse *dh)
{
	const DoctrineParams *p = Doctrine_ParamsOf(houseID);
	RoleGroup group[DOCTRINE_ROLE_MAX];
	uint32 eta[DOCTRINE_ROLE_MAX];
	uint32 etaMax = 0;
	uint32 hp = 0;
	uint16 count;
	uint16 atLD = 0;
	uint32 hpAtLD = 0;
	bool contact;
	bool expired;
	uint8 role;

	count = Doctrine_CountWave(houseID, &hp);
	if (count == 0) { Doctrine_EnterPhase(dh, PHASE_MUSTER); return; }

	contact = (hp < dh->waveHpLast);
	dh->waveHpLast = hp;
	expired = (g_timerGame > dh->phaseStart + p->graceTicks);

	if (hp * 100 < dh->waveHpStart * p->abortPercent) {
		dh->wavesAborted++;
		Doctrine_DismissWave(houseID);
		Doctrine_EnterPhase(dh, PHASE_MUSTER);
		return;
	}

	for (role = 0; role < DOCTRINE_ROLE_MAX; role++) {
		Doctrine_Gather(houseID, role, true, &group[role]);
		eta[role] = 0;

		if (group[role].count == 0) continue;

		/* Tiles scaled up so integer division keeps its resolution; the units are
		 * arbitrary because only the comparison between roles matters. */
		eta[role] = (uint32)Tile_GetDistancePacked(group[role].centre, dh->ldPacked) * 256 / group[role].slowest;
		if (eta[role] > etaMax) etaMax = eta[role];
	}

	for (role = 0; role < DOCTRINE_ROLE_MAX; role++) {
		uint16 i;

		if (group[role].count == 0) continue;

		/* Early: this role would arrive before the slowest one.  Hold. */
		if (!contact && !expired && eta[role] + p->etaSlack < etaMax) {
			for (i = 0; i < group[role].count; i++) {
				Unit *u = Unit_Get_ByIndex(group[role].order[i]);

				if (u != NULL) Doctrine_OrderHold(u);
			}
			continue;
		}

		dh->columnLength = Doctrine_MarchTo(houseID, &group[role], dh->ldPacked, !contact);
	}

	/* How much of the wave is standing on the line. */
	{
		PoolFindStruct find;

		find.houseID = houseID;
		find.index   = 0xFFFF;
		find.type    = 0xFFFF;

		while (true) {
			const Unit *u = Unit_Find(&find);

			if (u == NULL) break;
			if (u->o.index >= UNIT_INDEX_MAX || u->o.flags.s.isNotOnMap) continue;
			if (s_unitOnWave[u->o.index] == 0) continue;
			if (Tile_GetDistancePacked(Tile_PackTile(u->o.position), dh->ldPacked) > 6) continue;

			atLD++;
			hpAtLD += u->o.hitpoints;
		}
	}

	dh->atLD = atLD;

	if (contact || expired || hpAtLD * 100 >= hp * p->releasePercent) {
		/* Assembled at the line -- now, is it worth going in?
		 *
		 * A wave that cannot beat what is in front of it does not improve its
		 * chances by arriving: it feeds the turrets a vehicle at a time and the
		 * enemy loses nothing.  Not going in is not passivity, it is the other
		 * half of the strategy -- the raiders are out hunting harvesters, and an
		 * enemy whose economy is being taken apart has to come out to us, where
		 * there are no turrets. */
		Doctrine_EnterPhase(dh, PHASE_SUPPRESS);
	}
}

/**
 * Suppress: the artillery takes the turret line apart from outside its reach,
 * and the assault stands between it and the base.
 *
 * A Launcher reaches 9 and a Rocket Turret 8, so this is safe against the line
 * itself -- but not against what drives out of the base to stop it.  An
 * unescorted artillery group is the softest thing on the map: no armour, no
 * close-in weapon, and standing still.  So the assault sits escortDistance
 * behind, between the guns and the enemy, and the moment anything reaches the
 * artillery the whole wave goes in.  Waiting is not the alternative: the guns
 * die while the assault watches.
 */
static void Doctrine_PhaseSuppress(uint8 houseID, DoctrineHouse *dh)
{
	const DoctrineParams *p = Doctrine_ParamsOf(houseID);
	RoleGroup artillery, assault, raid;
	uint16 turret;
	uint16 turretDistance;
	uint16 turretPacked;
	uint32 hp = 0;
	uint16 count;
	bool threatened = false;
	uint16 i;

	count = Doctrine_CountWave(houseID, &hp);
	if (count == 0) { Doctrine_EnterPhase(dh, PHASE_MUSTER); return; }

	if (hp * 100 < dh->waveHpStart * p->abortPercent) {
		dh->wavesAborted++;
		Doctrine_DismissWave(houseID);
		Doctrine_EnterPhase(dh, PHASE_MUSTER);
		return;
	}

	Doctrine_Gather(houseID, DOCTRINE_ROLE_ARTILLERY, true, &artillery);
	Doctrine_Gather(houseID, DOCTRINE_ROLE_ASSAULT, true, &assault);
	Doctrine_Gather(houseID, DOCTRINE_ROLE_RAID, true, &raid);

	turret = Doctrine_NearestTurret(houseID, dh->ldPacked, &turretDistance);

	/* Nothing left to suppress, no gun to do it with, or the clock ran out. */
	if (turret == 0 || artillery.count == 0
		|| turretDistance > p->ldStandoff + 8
		|| g_timerGame > dh->phaseStart + p->graceTicks * 4) {
		Doctrine_EnterPhase(dh, PHASE_ASSAULT);
		return;
	}

	dh->suppressTarget = turret;
	turretPacked = Tools_Index_GetPackedTile(turret);

	/* Anything of the enemy's that has come out to the guns. */
	{
		PoolFindStruct find;

		find.houseID = HOUSE_INVALID;
		find.index   = 0xFFFF;
		find.type    = 0xFFFF;

		while (true) {
			Unit *u = Unit_Find(&find);

			if (u == NULL) break;
			if (House_AreAllied(houseID, Unit_GetHouseID(u))) continue;
			if (!g_table_unitInfo[u->o.type].o.flags.priority) continue;
			if (Tile_GetDistancePacked(Tile_PackTile(u->o.position), artillery.centre) > 10) continue;

			threatened = true;
			break;
		}
	}

	if (artillery.hitpoints < dh->waveHpLast && artillery.count > 0) threatened = true;
	dh->waveHpLast = hp;

	/* The escort trigger.  Defenders reaching the artillery is the signal for the
	 * assault to stop escorting and start attacking. */
	if (threatened) {
		Doctrine_EnterPhase(dh, PHASE_ASSAULT);
		return;
	}

	/* Guns forward to their own reach, which is further than the turret's. */
	UnitSelection_SpreadReset();
	for (i = 0; i < artillery.count; i++) {
		Unit *u = Unit_Get_ByIndex(artillery.order[i]);
		uint16 reach;
		tile32 stand;

		if (u == NULL) continue;

		/* A Saboteur has no standoff -- its weapon is arriving. */
		if (u->o.type == UNIT_SABOTEUR) {
			if (u->actionID != ACTION_SABOTAGE) Unit_SetAction(u, ACTION_SABOTAGE);
			Unit_SetTarget(u, turret);
			Unit_SetDestination(u, turret);
			continue;
		}

		reach = g_table_unitInfo[u->o.type].fireDistance;
		stand = Tile_MoveByDirection(Tile_UnpackTile(turretPacked),
		                             Tile_GetDirection(Tile_UnpackTile(turretPacked), u->o.position),
		                             reach << 8);

		Doctrine_OrderAttack(u, turret, UnitSelection_SpreadTake(u, Tile_PackTile(stand)));
		dh->suppressShots++;
	}

	/* Assault between the guns and the base, close enough to reach anything that
	 * comes for them within a couple of ticks. */
	{
		tile32 behind = Tile_MoveByDirection(Tile_UnpackTile(artillery.centre),
		                                     Tile_GetDirection(Tile_UnpackTile(turretPacked), Tile_UnpackTile(artillery.centre)),
		                                     p->escortDistance << 8);

		Doctrine_MarchTo(houseID, &assault, Tile_PackTile(behind), false);
	}

	Doctrine_MarchTo(houseID, &raid, dh->ldPacked, false);
}

/**
 * Assault: in, and nobody waits for anybody.
 *
 * The objective is only a destination.  What each unit actually shoots is
 * decided on its own tick by Unit_Skirmish_ClearTheWay(), which engages whatever
 * is in front of it first and returns to the objective when the way is clear.
 */
static void Doctrine_PhaseAssault(uint8 houseID, DoctrineHouse *dh, uint8 enemy)
{
	const DoctrineParams *p = Doctrine_ParamsOf(houseID);
	RoleGroup group[DOCTRINE_ROLE_MAX];
	uint32 hp = 0;
	uint16 count;
	uint16 objectivePacked;
	uint8 role;

	count = Doctrine_CountWave(houseID, &hp);
	if (count == 0) { Doctrine_EnterPhase(dh, PHASE_MUSTER); return; }

	if (hp * 100 < dh->waveHpStart * p->abortPercent) {
		dh->wavesAborted++;
		Doctrine_DismissWave(houseID);
		Doctrine_EnterPhase(dh, PHASE_MUSTER);
		return;
	}

	/* Objective gone.  Still strong enough to keep going: take the next one and
	 * stay in.  Otherwise the wave has done its work and goes home to re-form,
	 * rather than dissolving into the stream this doctrine exists to end. */
	if (!Tools_Index_IsValid(dh->objective) || Tools_Index_GetStructure(dh->objective) == NULL) {
		uint16 next = 0;

		if (hp * 100 >= dh->waveHpStart * p->releasePercent) {
			next = Doctrine_PickObjective(houseID, enemy, dh->ldPacked, count);
		}

		if (next == 0) {
			Doctrine_DismissWave(houseID);
			Doctrine_EnterPhase(dh, PHASE_MUSTER);
			return;
		}

		dh->objective = next;
	}

	/* A wave is a wave, not a siege.
	 *
	 * Reinforcement deliberately stops at the assault, so an assault that grinds
	 * on for tens of thousands of ticks is one where the factories fill the yard
	 * behind it and none of it joins: measured at its worst, seven units in the
	 * assault against twenty-two standing in the muster.  Past the limit the wave
	 * is spent whether or not it took its objective, and what is standing at home
	 * becomes the next one -- which is the shape this doctrine is named for. */
	if (g_timerGame > dh->phaseStart + p->assaultTicks) {
		Doctrine_DismissWave(houseID);
		Doctrine_EnterPhase(dh, PHASE_MUSTER);
		return;
	}

	objectivePacked = Tools_Index_GetPackedTile(dh->objective);

	for (role = 0; role < DOCTRINE_ROLE_MAX; role++) {
		uint16 i;

		Doctrine_Gather(houseID, role, true, &group[role]);
		if (group[role].count == 0) continue;

		UnitSelection_SpreadReset();

		for (i = 0; i < group[role].count; i++) {
			Unit *u = Unit_Get_ByIndex(group[role].order[i]);
			uint16 reach;
			tile32 stand;

			if (u == NULL || Doctrine_JustTurnedBack(u)) continue;

			/* Already engaged with something in the way -- leave it alone. */
			if (u->actionID == ACTION_ATTACK && Tools_Index_IsValid(u->targetAttack)
				&& Tools_Index_GetType(u->targetAttack) == IT_UNIT) continue;

			reach = g_table_unitInfo[u->o.type].fireDistance;
			stand = Tile_MoveByDirection(Tile_UnpackTile(objectivePacked),
			                             Tile_GetDirection(Tile_UnpackTile(objectivePacked), u->o.position),
			                             reach << 8);

			/* The post is inside somebody's envelope, which most of them are:
			 * the objective is a building and the buildings are what the line is
			 * there to cover.  Then the turret is the job, and the objective
			 * waits -- this is the doctrine anyway, and it is also the only way
			 * to send a unit in there without breaking the rule that says being
			 * inside is allowed exactly when the turret is the target.
			 *
			 * Doing it here rather than in the exclusion is the whole point.
			 * Ordering a unit somewhere covered and pushing it out again next
			 * tick is not a rule, it is a loop, and it ran 318 times a match. */
			{
				uint16 cover = Doctrine_CoveringTurret(houseID, Tile_PackTile(stand));

				if (cover != 0) {
					Doctrine_OrderAttack(u, cover, Tile_PackTile(stand));
					continue;
				}
			}

			Doctrine_OrderAttack(u, dh->objective, UnitSelection_SpreadTake(u, Tile_PackTile(stand)));
		}
	}

	dh->columnLength = 0;
	dh->atLD = 0;
}

/**
 * Gather everything that is not on a wave behind the defence line.
 *
 * This overrides ACTION_HUNT, which the engine gives every fresh AI unit and
 * which is the whole of doctrine A's behaviour: a unit rolls out of the factory
 * and drives at the enemy on its own.  That is why a B house was seen sending
 * quads one at a time into a turret line -- they were not being sent, they were
 * going by themselves, and arriving alone.
 *
 * Overriding it is what makes a wave possible at all.  It was removed once
 * before, because with the wave threshold set where it was no wave ever formed
 * and the override left the reserve inert; with waves forming it is the point.
 * The rally is behind the house's own turrets rather than out in the open, so
 * what accumulates there accumulates next to the guns covering it.
 */
/**
 * Raiders hunt harvesters; they are no use anywhere else.
 *
 * A Trike reaches 3 tiles and dies to anything that shoots back, so putting it
 * in a line of battle is throwing it away -- but it is the fastest thing on the
 * map, and an enemy harvester is unarmed, alone, and standing on the one part of
 * the map its owner cannot do without.  The same reasoning that keeps our own
 * harvesters away from a fight sends these to where theirs have to be.
 *
 * Only raiders not on a wave: when a wave takes them they are its screen.
 */
/**
 * A spice field to lie in wait on: nearest to the enemy, clear of turrets.
 *
 * Sampled rather than swept -- a full landscape walk on every doctrine tick is
 * not worth it for a question whose answer only has to be roughly right, and
 * spice comes in fields rather than single tiles.
 */
static uint16 Doctrine_FindHuntingGround(uint8 houseID, uint16 anchor)
{
	uint16 best = 0;
	uint16 bestDistance = 0xFFFF;
	uint16 x, y;

	for (y = 2; y < 61; y += 3) {
		for (x = 2; x < 61; x += 3) {
			const uint16 packed = Tile_PackXY(x, y);
			uint16 type, d;

			type = Map_GetLandscapeType(packed);
			if (type != LST_SPICE && type != LST_THICK_SPICE) continue;
			if (Doctrine_CoveringTurret(houseID, packed) != 0) continue;

			d = Tile_GetDistancePacked(anchor, packed);
			if (d >= bestDistance) continue;

			bestDistance = d;
			best = packed;
		}
	}

	return best;
}

static void Doctrine_Patrol(uint8 houseID, DoctrineHouse *dh)
{
	PoolFindStruct find;
	uint16 prey = 0;
	uint16 preyDistance = 0xFFFF;
	uint16 anchor = (dh->ldPacked != 0) ? dh->ldPacked : dh->musterPacked;

	if (anchor == 0) return;

	/* The nearest enemy harvester on the map, loaded or not. */
	find.houseID = HOUSE_INVALID;
	find.index   = 0xFFFF;
	find.type    = UNIT_HARVESTER;

	while (true) {
		Unit *u = Unit_Find(&find);
		uint16 d;

		if (u == NULL) break;
		if (u->o.flags.s.isNotOnMap) continue;
		if (House_AreAllied(houseID, Unit_GetHouseID(u))) continue;

		/* Not under a turret.  A Trike reaches three tiles and a Rocket Turret
		 * eight: a harvester parked inside the envelope is bait, and taking it
		 * costs more than the harvester is worth. */
		if (Doctrine_CoveringTurret(houseID, Tile_PackTile(u->o.position)) != 0) continue;

		d = Tile_GetDistancePacked(anchor, Tile_PackTile(u->o.position));
		if (d >= preyDistance) continue;

		preyDistance = d;
		prey = Tools_Index_Encode(u->o.index, IT_UNIT);
	}

	/* Nothing takeable: wait where they have to come.
	 *
	 * Every harvester the enemy owns is either on spice or on its way to it, so
	 * a spice field outside anybody's reach is an ambush that does not have to
	 * find anything.  Without this a raider whose prey was all parked under
	 * turrets simply stopped -- measured at 18 raiders and none hunting for the
	 * last forty thousand ticks of a match. */
	if (prey == 0) {
		uint16 field = Doctrine_FindHuntingGround(houseID, anchor);

		if (field == 0) return;

		find.houseID = houseID;
		find.index   = 0xFFFF;
		find.type    = 0xFFFF;

		UnitSelection_SpreadReset();

		while (true) {
			Unit *u = Unit_Find(&find);

			if (u == NULL) break;
			if (u->o.index >= UNIT_INDEX_MAX || u->o.flags.s.isNotOnMap) continue;
			if (s_unitRole[u->o.index] != DOCTRINE_ROLE_RAID) continue;
			if (Tools_Index_IsValid(u->targetAttack)) continue;
			if (Doctrine_JustTurnedBack(u)) continue;

			/* Already on station: hold it and watch. */
			if (Tile_GetDistancePacked(Tile_PackTile(u->o.position), field) <= 5) {
				if (u->actionID != ACTION_GUARD) Doctrine_OrderHold(u);
				continue;
			}

			if (u->targetMove != 0 && u->actionID == ACTION_MOVE) continue;

			Doctrine_OrderMove(u, UnitSelection_SpreadTake(u, field));
		}

		return;
	}

	find.houseID = houseID;
	find.index   = 0xFFFF;
	find.type    = 0xFFFF;

	while (true) {
		Unit *u = Unit_Find(&find);

		if (u == NULL) break;
		if (u->o.index >= UNIT_INDEX_MAX || u->o.flags.s.isNotOnMap) continue;
		if (s_unitRole[u->o.index] != DOCTRINE_ROLE_RAID) continue;
		if (s_unitOnWave[u->o.index] != 0) continue;
		if (Doctrine_JustTurnedBack(u)) continue;

		/* Already hunting something: let it finish. */
		if (Tools_Index_IsValid(u->targetAttack)) continue;

		if (u->actionID != ACTION_ATTACK) Unit_SetAction(u, ACTION_ATTACK);
		Unit_SetTarget(u, prey);
		Unit_SetDestination(u, prey);
	}
}

/**
 * Post part of the reserve onto flank spice instead of the yard.
 *
 * A crowd standing at home is capital doing nothing, and it is also a target:
 * it is on the line the enemy's attacks come down, so it is ground away by
 * fights it did not choose and gains nothing for the losses.  The same units on
 * a spice field away from that line hold something worth holding -- they push
 * the enemy's raiders off it, which is the same strangling our own raiders do,
 * from the other end.
 *
 * They remain wave material: a muster takes them straight back, so this costs
 * the assault nothing and only decides where they wait.
 *
 * The field is chosen away from the line of departure, not near it, which is
 * what "flank" means here: standing next to the corridor the wave uses would
 * put them back in the path of everything coming the other way.
 */
static void Doctrine_Picket(uint8 houseID, DoctrineHouse *dh)
{
	const DoctrineParams *p = Doctrine_ParamsOf(houseID);
	PoolFindStruct find;
	uint16 reserve;
	uint16 want;
	uint16 posted = 0;
	uint16 field;

	if (p->picketPercent == 0) return;

	reserve = Doctrine_CountReserve(houseID, NULL);
	if (reserve < p->picketFrom) return;

	want = (uint16)(reserve * p->picketPercent / 100);
	if (want == 0) return;

	/* Away from the corridor: measured from home rather than from the line of
	 * departure, so the field picked is one on our own flank. */
	field = Doctrine_FindHuntingGround(houseID, dh->musterPacked);
	if (field == 0) return;
	if (dh->ldPacked != 0 && Tile_GetDistancePacked(field, dh->ldPacked) < 12) return;

	find.houseID = houseID;
	find.index   = 0xFFFF;
	find.type    = 0xFFFF;

	UnitSelection_SpreadReset();

	while (true) {
		Unit *u = Unit_Find(&find);

		if (u == NULL) break;
		if (u->o.index >= UNIT_INDEX_MAX || u->o.flags.s.isNotOnMap) continue;
		if (s_unitOnWave[u->o.index] != 0) continue;
		if (s_unitRole[u->o.index] != DOCTRINE_ROLE_ASSAULT) continue;
		if (posted >= want) break;

		s_unitPicket[u->o.index] = 1;
		posted++;

		if (Tools_Index_IsValid(u->targetAttack)) continue;
		if (Doctrine_JustTurnedBack(u)) continue;

		if (Tile_GetDistancePacked(Tile_PackTile(u->o.position), field) <= 5) {
			if (u->actionID != ACTION_GUARD) Doctrine_OrderHold(u);
			continue;
		}

		if (u->targetMove != 0 && u->actionID == ACTION_MOVE) continue;

		Doctrine_OrderMove(u, UnitSelection_SpreadTake(u, field));
	}
}

static void Doctrine_Rally(uint8 houseID, DoctrineHouse *dh)
{
	PoolFindStruct find;

	if (dh->musterPacked == 0) return;

	find.houseID = houseID;
	find.index   = 0xFFFF;
	find.type    = 0xFFFF;

	/* Spread along the line rather than piled on its first tile: one claimed
	 * tile each, radiating from the forward gun, so the reserve fills the gaps
	 * in the picket instead of making a single target of itself. */
	UnitSelection_SpreadReset();

	while (true) {
		Unit *u = Unit_Find(&find);

		if (u == NULL) break;
		if (u->o.index >= UNIT_INDEX_MAX || u->o.flags.s.isNotOnMap) continue;
		if (s_unitOnWave[u->o.index] != 0) continue;
		if (s_unitRole[u->o.index] == DOCTRINE_ROLE_NONE) continue;
		if (u->o.type == UNIT_SABOTEUR) continue;                  /* Has its own errand. */
		if (s_unitRole[u->o.index] == DOCTRINE_ROLE_RAID) continue; /* Out hunting harvesters. */
		if (s_unitPicket[u->o.index] != 0) continue;                /* Holding a flank field. */

		/* Shooting at something is the job; do not interrupt it. */
		if (Tools_Index_IsValid(u->targetAttack)) continue;

		/* In the picket already: hold.  ACTION_GUARD still answers for its own
		 * ground, and Unit_Skirmish_ClearTheWay() gives it anything in range. */
		if (Tile_GetDistancePacked(Tile_PackTile(u->o.position), dh->musterPacked) <= 4) {
			if (u->actionID != ACTION_GUARD) Doctrine_OrderHold(u);
			continue;
		}

		if (u->targetMove != 0 && u->actionID == ACTION_MOVE) continue;

		Doctrine_OrderMove(u, UnitSelection_SpreadTake(u, dh->musterPacked));
	}
}

/**
 * Doctrine B touches nothing it has not committed.
 *
 * There was a garrison routine here that fetched idle reserve units back to the
 * rally point, and it was the reason a B house sat inside its own base while its
 * defence line was taken apart.  The engine gives every fresh AI unit
 * ACTION_HUNT -- that is what sends it out to fight on its own, and it is the
 * whole of doctrine A's behaviour.  Any order issued to a unit B has not
 * committed replaces that with something quieter, so B was strictly less active
 * than the doctrine it is supposed to improve on.
 *
 * The rule now is that B is A plus waves: a unit is either on a wave, and then
 * B owns it completely, or it is not, and then B does not speak to it at all.
 * That bounds how much worse B can be, which matters more here than any tidy
 * idea about what a reserve ought to be doing.
 */

/**
 * Send every Saboteur at the turret in the way, the moment it exists.
 *
 * They are not built and cannot be planned for: the Palace spawns one on its own
 * countdown and hands it ACTION_SABOTAGE, whereupon UNIT.EMC picks a target with
 * Unit_FindBestTargetEncoded(mode 4) -- highest priority over the whole map,
 * divided by distance.  From inside its own base that is a lottery over
 * everything the enemy owns, and it is usually spent on whatever happens to sit
 * on the near edge.
 *
 * Spent on a Rocket Turret instead it is worth 250 credits of defence and the
 * wave behind it gets through.  So they are never held for a muster -- a free
 * unit that arrives on a timer should leave on the same timer -- and they are
 * aimed at the turret nearest the line of departure, which is by construction
 * the one the wave is about to meet.
 */
static void Doctrine_Saboteurs(uint8 houseID, DoctrineHouse *dh)
{
	PoolFindStruct find;
	uint16 aim = (dh->ldPacked != 0) ? dh->ldPacked : dh->musterPacked;
	uint16 turret;

	if (aim == 0) return;

	turret = Doctrine_NearestTurret(houseID, aim, NULL);
	if (turret == 0) return;

	find.houseID = houseID;
	find.index   = 0xFFFF;
	find.type    = UNIT_SABOTEUR;

	while (true) {
		Unit *u = Unit_Find(&find);

		if (u == NULL) break;
		if (u->o.index >= UNIT_INDEX_MAX || u->o.flags.s.isNotOnMap) continue;

		/* Already walking into a turret: leave it alone.  Re-aiming one in
		 * transit is how it ends up circling between two of them. */
		if (u->targetMove != 0 && Doctrine_IsTurret(u->targetMove)) continue;

		if (u->actionID != ACTION_SABOTAGE) Unit_SetAction(u, ACTION_SABOTAGE);
		Unit_SetTarget(u, turret);
		Unit_SetDestination(u, turret);
	}
}

/**
 * The two things a post-mortem cannot tell you, sampled while they are true.
 *
 * Harvester exposure and assault cohesion are both states, not events: by the
 * time they show up in a loss or a failed attack the run is over and the number
 * says only that something went wrong.  Sampled every doctrine tick they are
 * usable as a regression test -- exposure climbs before a harvester dies, and
 * cohesion falls before an assault is thrown away piecemeal.
 */
static void Doctrine_SampleMetrics(uint8 houseID, const DoctrineHouse *dh)
{
	PoolFindStruct find;
	uint16 near8 = 0, worst = 99;
	uint16 onWave = 0, attackers = 0;

	Doctrine_HarvesterExposure(houseID, &near8, &worst);
	s_harvesterSamples[houseID]++;
	if (near8 > 0) s_harvesterExposed[houseID]++;

	/* Only while an assault is actually under way.  Outside one there is no wave
	 * to be part of, and averaging in the muster would answer a question nobody
	 * asked. */
	if (dh->phase != PHASE_ASSAULT) return;

	find.houseID = houseID;
	find.index   = 0xFFFF;
	find.type    = 0xFFFF;

	while (true) {
		const Unit *u = Unit_Find(&find);
		uint8 role;

		if (u == NULL) break;
		if (u->o.index >= UNIT_INDEX_MAX || u->o.flags.s.isNotOnMap) continue;

		role = s_unitRole[u->o.index];
		if (!Doctrine_IsAttacker(role)) continue;

		attackers++;
		if (s_unitOnWave[u->o.index] != 0) onWave++;
	}

	s_cohesionOn[houseID]  += onWave;
	s_cohesionAll[houseID] += attackers;
}

void Doctrine_Tick(House *h)
{
	uint8 houseID;
	uint8 enemy;
	DoctrineHouse *dh;

	if (h == NULL) return;

	houseID = (uint8)h->index;
	if (houseID >= HOUSE_MAX) return;
	if (Doctrine_GetForHouse(houseID) == DOCTRINE_LEGACY) return;

	dh = &s_house[houseID];
	if (dh->nextTick > g_timerGame) return;
	dh->nextTick = g_timerGame + DOCTRINE_TICK;

	enemy = Doctrine_EnemyOf(houseID);
	if (enemy == HOUSE_INVALID) return;
	dh->enemy = enemy;

	Doctrine_AssignRoles(houseID);
	Doctrine_Reinforce(houseID, dh);

	switch (dh->phase) {
		case PHASE_MUSTER:   Doctrine_PhaseMuster(houseID, dh, enemy);   break;
		case PHASE_APPROACH: Doctrine_PhaseApproach(houseID, dh);        break;
		case PHASE_SUPPRESS: Doctrine_PhaseSuppress(houseID, dh);        break;
		default:             Doctrine_PhaseAssault(houseID, dh, enemy);  break;
	}

	Doctrine_Picket(houseID, dh);
	Doctrine_Rally(houseID, dh);
	Doctrine_Saboteurs(houseID, dh);
	Doctrine_Patrol(houseID, dh);

	Doctrine_SampleMetrics(houseID, dh);
}

/* -------------------------------------------------------------------------- */
/* Doctrine B -- production                                                    */
/* -------------------------------------------------------------------------- */

/**
 * Type weights inside a role.  The role shares decide how much of the army each
 * role gets; these decide what it is made of.
 */
static const uint8 s_typeWeight[UNIT_MAX] = {
	0,   /* Carryall     -- the economy's business */
	10,  /* Ornithopter */
	20,  /* Infantry */
	30,  /* Troopers     -- reach 5 and 5 damage, the useful half of the garrison */
	10,  /* Soldier */
	20,  /* Trooper */
	0,   /* Saboteur */
	60,  /* Launcher     -- reach 9, the only unit that outranges a Rocket Turret */
	20,  /* Deviator */
	50,  /* Tank */
	40,  /* Siege Tank */
	30,  /* Devastator */
	40,  /* Sonic Tank   -- reach 8, a draw with a Rocket Turret */
	20,  /* Trike */
	30,  /* Raider Trike */
	40,  /* Quad */
	0,   /* Harvester */
	0    /* MCV */
};

/**
 * Build towards the doctrine's role proportions.
 *
 * Two levels, because one flat table cannot express "we need artillery" while
 * the tech for artillery does not exist yet.  Only roles with something
 * buildable in this factory's mask take part in the comparison, so an
 * unreachable role does not sit at zero dragging the whole rule towards it --
 * which is what "часть технологий вначале недоступна" costs if you ignore it.
 * The moment the IX Research Centre finishes, the artillery role appears in the
 * comparison already far below its share and gets built first.
 */
uint16 Doctrine_PickUnit(const House *h, uint32 buildable)
{
	const DoctrineParams *p;
	uint16 roleCount[DOCTRINE_ROLE_MAX];
	bool roleBuildable[DOCTRINE_ROLE_MAX];
	uint32 shareSum = 0;
	uint32 total = 0;
	uint8 bestRole = DOCTRINE_ROLE_NONE;
	int32 bestRoleScore = 0;
	uint16 best = 0xFFFF;
	int32 bestScore = 0;
	uint32 weightSum = 0;
	uint32 roleTotal = 0;
	uint8 role;
	uint16 i;

	if (h == NULL) return 0xFFFF;
	p = Doctrine_ParamsOf((uint8)h->index);

	for (role = 0; role < DOCTRINE_ROLE_MAX; role++) {
		roleCount[role] = 0;
		roleBuildable[role] = false;
	}

	for (i = 0; i < UNIT_MAX; i++) {
		uint8 r = Doctrine_RoleOf(i);

		if (r == DOCTRINE_ROLE_NONE || s_typeWeight[i] == 0) continue;

		/* Counted whether or not this factory can build it: the share is of the
		 * whole army, not of one production line.
		 *
		 * Alive, not built-ever.  A composition rule is about the army standing on
		 * the map: counting everything ever built bans a role that keeps dying,
		 * which is how the raid role reached zero -- fast units die, the built
		 * count keeps climbing, and the cap locks them out for the rest of the
		 * match exactly when they need replacing. */
		roleCount[r] = (uint16)(roleCount[r] + Skirmish_CountUnitsOfType((uint8)h->index, i));

		if ((buildable & (1u << i)) == 0) continue;
		roleBuildable[r] = true;
		s_roleSeen[h->index][r] = 1;
	}

	for (role = 0; role < DOCTRINE_ROLE_MAX; role++) {
		if (!roleBuildable[role] || p->roleShare[role] == 0) continue;
		shareSum += p->roleShare[role];
		total    += roleCount[role];
	}

	if (shareSum == 0) return 0xFFFF;

	for (role = 0; role < DOCTRINE_ROLE_MAX; role++) {
		int32 want, have, score;

		if (!roleBuildable[role] || p->roleShare[role] == 0) continue;

		want = (int32)(p->roleShare[role] * 1000 / shareSum);
		have = (total == 0) ? 0 : (int32)(roleCount[role] * 1000 / total);
		score = want - have;

		if (bestRole != DOCTRINE_ROLE_NONE && score <= bestRoleScore) continue;

		bestRole = role;
		bestRoleScore = score;
	}

	if (bestRole == DOCTRINE_ROLE_NONE) return 0xFFFF;

	for (i = 0; i < UNIT_MAX; i++) {
		if ((buildable & (1u << i)) == 0 || s_typeWeight[i] == 0) continue;
		if (Doctrine_RoleOf(i) != bestRole) continue;

		weightSum += s_typeWeight[i];
		roleTotal += Skirmish_CountUnitsOfType((uint8)h->index, i);
	}

	if (weightSum == 0) return 0xFFFF;

	for (i = 0; i < UNIT_MAX; i++) {
		int32 want, have, score;

		if ((buildable & (1u << i)) == 0 || s_typeWeight[i] == 0) continue;
		if (Doctrine_RoleOf(i) != bestRole) continue;

		want = (int32)(s_typeWeight[i] * 1000 / weightSum);
		have = (roleTotal == 0) ? 0 : (int32)(Skirmish_CountUnitsOfType((uint8)h->index, i) * 1000 / roleTotal);
		score = want - have;

		if (best != 0xFFFF && score <= bestScore) continue;

		best = i;
		bestScore = score;
	}

	if (best != 0xFFFF) s_pickedRole[h->index][bestRole]++;

	return best;
}

/**
 * Veto a unit whose role already has more than its share.
 *
 * Choosing well inside one factory is not enough, because most factories can
 * only build one role: the Barracks sees nothing but garrison and the Light
 * Factory nothing but raid, so each of them picks "the role furthest below its
 * share" out of a set of one and builds it for ever.  Measured, that gave 26
 * raid and 37 garrison units against 2 artillery and 4 assault -- the shares
 * were computed correctly and enforced nowhere.
 *
 * The share is taken over the roles this house has ever been able to build, so
 * a technology it does not have yet does not freeze production: before the IX
 * Research Centre exists artillery is unreachable and its twenty percent belongs
 * to the other three, and the moment it appears it is far below share and gets
 * built first.
 */
bool Doctrine_AllowUnit(const House *h, uint16 unitType)
{
	const DoctrineParams *p;
	uint8 role;
	uint32 shareSum = 0;
	uint32 total = 0;
	uint32 mine = 0;
	uint16 i;
	uint8 r;

	if (h == NULL) return true;

	role = Doctrine_RoleOf(unitType);
	if (role == DOCTRINE_ROLE_NONE) return true;

	p = Doctrine_ParamsOf((uint8)h->index);

	for (i = 0; i < UNIT_MAX; i++) {
		r = Doctrine_RoleOf(i);

		if (r == DOCTRINE_ROLE_NONE || !s_roleSeen[h->index][r]) continue;

		/* Built over the match, not alive now.  The two questions are different
		 * and each rule needs its own: Doctrine_PickUnit() asks what the army is
		 * short of and must look at what is standing, while this one asks where
		 * the money has gone and must look at what was paid for.
		 *
		 * Measured with live counts here, the veto never fired once in a whole
		 * match.  Infantry is the one thing that never stays alive: it dies, the
		 * live count drops, the test passes, the Barracks builds more -- 46
		 * garrison units against 2 assault, and not one refusal.  It is also
		 * cheap, and production is paid a credit at a time, so five soldiers take
		 * the credit stream a tank needed. */
		total += Skirmish_GetUnitsBuilt((uint8)h->index, i);
		if (r == role) mine += Skirmish_GetUnitsBuilt((uint8)h->index, i);
	}

	for (r = 0; r < DOCTRINE_ROLE_MAX; r++) {
		if (s_roleSeen[h->index][r]) shareSum += p->roleShare[r];
	}

	if (shareSum == 0) return true;

	/* Refusing to build is almost never right.
	 *
	 * The first version of this vetoed anything over its share, and it was the
	 * single largest handicap doctrine B carried: the roles are never all
	 * buildable at the same moment, so whichever ones a factory can reach are
	 * the ones above share, every one of them is refused, and the army stops
	 * growing.  Measured, B was pinned between 8 and 16 units for a whole match
	 * while A reached 43.  Loosening it to 1.5x was not enough either; with the
	 * veto off entirely the two doctrines became close enough that the corner of
	 * the map decided the match rather than the strategy.
	 *
	 * So composition is steered where it costs nothing -- Doctrine_PickUnit()
	 * picks the role furthest below its share whenever a factory asks -- and the
	 * veto is a quarter over share, measured in credits spent rather than units
	 * standing. */
	if (total < DOCTRINE_MIX_FLOOR) return true;

	if (mine * shareSum * 4 <= total * p->roleShare[role] * 5) return true;

	s_vetoedRole[h->index][role]++;

	return false;
}

/* -------------------------------------------------------------------------- */
/* Reporting                                                                   */
/* -------------------------------------------------------------------------- */

/**
 * Units built by role, picks by role, refusals by role.
 *
 * Roles are a property of the unit type, so this reads the same under either
 * doctrine and the two production lines can be compared directly.
 */
bool Doctrine_GetProduction(uint8 houseID, char *buf, uint16 length)
{
	uint16 built[DOCTRINE_ROLE_MAX];
	uint16 near8 = 0, worst = 99;
	uint8 r;
	uint16 i;

	if (houseID >= HOUSE_MAX || buf == NULL || length == 0) return false;

	for (r = 0; r < DOCTRINE_ROLE_MAX; r++) built[r] = 0;

	Doctrine_HarvesterExposure(houseID, &near8, &worst);

	for (i = 0; i < UNIT_MAX; i++) {
		r = Doctrine_RoleOf(i);
		if (r == DOCTRINE_ROLE_NONE) continue;
		built[r] = (uint16)(built[r] + Skirmish_GetUnitsBuilt(houseID, i));
	}

	snprintf(buf, length, "%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u",
	         built[0], built[1], built[2], built[3],
	         (unsigned)s_pickedRole[houseID][0], (unsigned)s_pickedRole[houseID][1],
	         (unsigned)s_pickedRole[houseID][2], (unsigned)s_pickedRole[houseID][3],
	         (unsigned)s_vetoedRole[houseID][0], (unsigned)s_vetoedRole[houseID][1],
	         (unsigned)s_vetoedRole[houseID][2], (unsigned)s_vetoedRole[houseID][3],
	         (unsigned)s_turretZone[houseID], (unsigned)s_turretEntries[houseID],
	         (unsigned)s_harvesterLost[houseID], (unsigned)s_harvesterKilled[houseID],
	         (unsigned)s_harvesterLostEarly[houseID], near8, worst,
	         (unsigned)s_killedByTurretLoose[houseID], (unsigned)s_killedByTurretAssault[houseID],
	         (unsigned)s_turretsKilled[houseID]);

	return true;
}

/** Every counter the metrics suite reads, for one House, as they stand now. */
void Doctrine_GetMetrics(uint8 houseID, DoctrineMetrics *out)
{
	const DoctrineHouse *dh;

	if (out == NULL) return;

	memset(out, 0, sizeof(DoctrineMetrics));
	if (houseID >= HOUSE_MAX) return;

	dh = &s_house[houseID];

	out->turretEntries       = s_turretEntries[houseID];
	out->turretDwell         = s_turretZone[houseID];
	out->turretDeathsLoose   = s_killedByTurretLoose[houseID];
	out->turretDeathsAssault = s_killedByTurretAssault[houseID];
	out->turretsKilled       = s_turretsKilled[houseID];
	out->harvesterLost       = s_harvesterLost[houseID];
	out->harvesterLostEarly  = s_harvesterLostEarly[houseID];
	out->harvesterKilled     = s_harvesterKilled[houseID];
	out->harvesterExposed    = s_harvesterExposed[houseID];
	out->harvesterSamples    = s_harvesterSamples[houseID];
	out->cohesionOn          = s_cohesionOn[houseID];
	out->cohesionAll         = s_cohesionAll[houseID];
	out->wavesLaunched       = dh->wavesLaunched;
	out->wavesAborted        = dh->wavesAborted;
	out->wavesDeclined       = dh->wavesDeclined;
	out->firstAssault        = dh->firstAssault;
}

/** A bullet landed on @p victim; @p originEncoded is whatever fired it. */
void Doctrine_RecordHit(struct Unit *victim, uint16 originEncoded)
{
	const Unit *bullet;
	const Structure *turret;

	if (victim == NULL || victim->o.index >= UNIT_INDEX_MAX) return;

	/* The explosion carries the bullet, and the bullet carries the thing that
	 * fired it -- see Script_Structure_Fire(), which stamps the turret onto its
	 * missile.  Two hops, and neither of them is reachable from Unit_Damage(). */
	bullet = Tools_Index_GetUnit(originEncoded);
	if (bullet == NULL) return;

	turret = Tools_Index_GetStructure(bullet->originEncoded);
	if (turret == NULL) return;
	if (turret->o.type != STRUCTURE_TURRET && turret->o.type != STRUCTURE_ROCKET_TURRET) return;

	s_hitByTurret[victim->o.index] = g_timerGame;
}

/** A unit of this House died.  Book it against a turret if one had just hit it. */
void Doctrine_RecordDeath(uint8 houseID, uint16 unitIndex)
{
	if (houseID >= HOUSE_MAX || unitIndex >= UNIT_INDEX_MAX) return;
	if (s_hitByTurret[unitIndex] == 0 || s_hitByTurret[unitIndex] + 200 < g_timerGame) return;

	/* On a wave, in the assault, is the only place a turret is allowed to kill
	 * one of ours -- that is the trade the doctrine is willing to make. */
	if (s_unitOnWave[unitIndex] != 0 && s_house[houseID].phase == PHASE_ASSAULT) {
		s_killedByTurretAssault[houseID]++;
	} else {
		s_killedByTurretLoose[houseID]++;
	}
}

void Doctrine_RecordTurretKilled(uint8 killer)
{
	if (killer < HOUSE_MAX) s_turretsKilled[killer]++;
}

void Doctrine_RecordHarvesterLoss(uint8 owner, uint8 killer)
{
	if (owner < HOUSE_MAX) {
		s_harvesterLost[owner]++;
		if (Doctrine_Elapsed() < 100000) s_harvesterLostEarly[owner]++;
	}
	if (killer < HOUSE_MAX) s_harvesterKilled[killer]++;
}

/**
 * How close this House's harvesters are to the nearest thing that can shoot
 * them: how many are inside eight tiles of one, and the worst single distance.
 *
 * Weighting danger when a field is chosen was not enough and the count says why
 * -- a harvester picks a quiet field, settles on it, and the war arrives.  From
 * that moment nothing re-asks the question: Unit_Harvester_Update() returns
 * early for a harvester already standing on spice, which is exactly the one in
 * trouble.  A number that is only sampled at the start of a trip cannot see
 * that; this one is sampled continuously.
 */
void Doctrine_HarvesterExposure(uint8 houseID, uint16 *near8, uint16 *worst)
{
	PoolFindStruct find;
	uint16 count = 0;
	uint16 closest = 99;

	if (near8 != NULL) *near8 = 0;
	if (worst != NULL)  *worst = 99;
	if (houseID >= HOUSE_MAX) return;

	find.houseID = houseID;
	find.index   = 0xFFFF;
	find.type    = UNIT_HARVESTER;

	while (true) {
		const Unit *u = Unit_Find(&find);
		uint16 d;

		if (u == NULL) break;
		if (u->o.flags.s.isNotOnMap) continue;

		d = Doctrine_ThreatDistance(houseID, Tile_PackTile(u->o.position));
		if (d <= 8) count++;
		if (d < closest) closest = d;
	}

	if (near8 != NULL) *near8 = count;
	if (worst != NULL)  *worst = closest;
}

bool Doctrine_GetTelemetry(uint8 houseID, char *buf, uint16 length)
{
	const DoctrineHouse *dh;
	PoolFindStruct find;
	uint16 inAssault = 0;
	uint16 inMuster = 0;
	uint16 idleGarrison = 0;
	uint16 spread = 0;
	uint16 near = 0xFFFF, far = 0;
	uint16 raiders = 0;
	uint16 hunting = 0;
	uint16 picket = 0;

	if (houseID >= HOUSE_MAX || buf == NULL || length == 0) return false;

	dh = &s_house[houseID];

	/* Where the army actually is, in three numbers.
	 *
	 * "The assault does not go in together" is not visible from the wave count:
	 * a wave of twenty that arrives as four and sixteen has the same count as one
	 * that arrives as twenty.  These say how the army is divided at this instant
	 * -- committed, held back, or standing about -- and the spread says how far
	 * apart the committed ones are from each other. */
	find.houseID = houseID;
	find.index   = 0xFFFF;
	find.type    = 0xFFFF;

	while (true) {
		const Unit *u = Unit_Find(&find);
		uint8 role;

		if (u == NULL) break;
		if (u->o.index >= UNIT_INDEX_MAX || u->o.flags.s.isNotOnMap) continue;

		role = s_unitRole[u->o.index];
		if (role == DOCTRINE_ROLE_NONE) continue;

		if (s_unitOnWave[u->o.index] != 0) {
			uint16 d;

			inAssault++;

			if (dh->objective != 0) {
				d = Tile_GetDistancePacked(Tile_PackTile(u->o.position), Tools_Index_GetPackedTile(dh->objective));
				if (d < near) near = d;
				if (d > far)  far = d;
			}
			continue;
		}

		if (Doctrine_IsAttacker(role)) {
			if (s_unitPicket[u->o.index] != 0) picket++; else inMuster++;
			continue;
		}

		/* Raiders do not join a wave, so "are they doing anything" needs its own
		 * number: how many exist, and how many are on a harvester right now.
		 * Without it "they hunt" is a claim about code, not about the game. */
		if (role == DOCTRINE_ROLE_RAID) {
			const Unit *prey = Tools_Index_GetUnit(u->targetAttack);

			raiders++;
			if (prey != NULL && prey->o.type == UNIT_HARVESTER) hunting++;
			continue;
		}

		if (!Tools_Index_IsValid(u->targetAttack) && u->targetMove == 0) idleGarrison++;
	}

	if (near != 0xFFFF && far > near) spread = (uint16)(far - near);

	snprintf(buf, length, "%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u",
	         dh->phase, dh->waveCount, dh->atLD, dh->columnLength,
	         (unsigned)dh->wavesLaunched, (unsigned)dh->wavesAborted,
	         (unsigned)dh->wavesDeclined,
	         inAssault, inMuster, idleGarrison, spread,
	         (unsigned)dh->firstAssault, raiders, hunting,
	         (unsigned)dh->attackStrength, (unsigned)dh->defenceStrength, picket);

	return true;
}

bool Doctrine_GetSummary(uint8 houseID, char *buf, uint16 length)
{
	static const char *phaseName[4] = { "mus", "app", "sup", "ASSAULT" };
	const DoctrineHouse *dh;
	uint16 role[DOCTRINE_ROLE_MAX];
	PoolFindStruct find;
	uint8 i;

	if (houseID >= HOUSE_MAX || buf == NULL || length == 0) return false;

	if (Doctrine_GetForHouse(houseID) == DOCTRINE_LEGACY) {
		snprintf(buf, length, "A legacy");
		return true;
	}

	dh = &s_house[houseID];

	for (i = 0; i < DOCTRINE_ROLE_MAX; i++) role[i] = 0;

	find.houseID = houseID;
	find.index   = 0xFFFF;
	find.type    = 0xFFFF;

	while (true) {
		const Unit *u = Unit_Find(&find);

		if (u == NULL) break;
		if (u->o.index >= UNIT_INDEX_MAX) continue;
		if (s_unitRole[u->o.index] >= DOCTRINE_ROLE_MAX) continue;
		role[s_unitRole[u->o.index]]++;
	}

	snprintf(buf, length, "B %s w%u/LD%u T%u a%us%ur%ug%u %u:%u L%uA%uD%u",
	         phaseName[dh->phase & 3], dh->waveCount, dh->atLD,
	         /* Tiles from the rally to the nearest own turret.  A reserve that is
	          * not standing with the guns is not covering them, and the number is
	          * the only way to see that without counting pixels. */
	         min(Skirmish_GetTurretDistance(houseID, dh->musterPacked), 99),
	         role[DOCTRINE_ROLE_ARTILLERY], role[DOCTRINE_ROLE_ASSAULT],
	         role[DOCTRINE_ROLE_RAID], role[DOCTRINE_ROLE_GARRISON],
	         (unsigned)dh->attackStrength, (unsigned)dh->defenceStrength,
	         (unsigned)dh->wavesLaunched, (unsigned)dh->wavesAborted,
	         (unsigned)dh->wavesDeclined);

	return true;
}
