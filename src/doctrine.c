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
	uint16 etaSlack;                                        /*!< Arrival-time difference treated as "together". */
	uint8  roleShare[DOCTRINE_ROLE_MAX];                    /*!< Army composition the factories build towards. */
} DoctrineParams;

static const DoctrineParams s_doctrine[DOCTRINE_MAX] = {
	{
		"A", "legacy: engine teams, one wave gate, fixed army mix",
		12,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		{ 0, 0, 0, 0 }
	},
	{
		"B", "echelon: roles, line of departure, artillery suppression",
		0,
		/* minWave */        6,
		/* garrisonKeep */   4,
		/* ldStandoff */     14,
		/* columnHold */     10,
		/* columnRelease */  6,
		/* releasePercent */ 60,
		/* abortPercent */   40,
		/* graceTicks */     1500,
		/* escortDistance */ 3,
		/* etaSlack */       600,
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
	uint32 suppressShots;
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

void Doctrine_Reset(void)
{
	memset(s_doctrineOf, DOCTRINE_LEGACY, sizeof(s_doctrineOf));
	memset(s_unitRole, DOCTRINE_ROLE_NONE, sizeof(s_unitRole));
	memset(s_unitOnWave, 0, sizeof(s_unitOnWave));
	memset(s_roleSeen, 0, sizeof(s_roleSeen));
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

		case UNIT_TANK:
		case UNIT_SIEGE_TANK:
		case UNIT_DEVIATOR:
			return DOCTRINE_ROLE_ASSAULT;

		case UNIT_TRIKE:
		case UNIT_RAIDER_TRIKE:
		case UNIT_QUAD:
			return DOCTRINE_ROLE_RAID;

		/* Too slow to march with the rest -- Infantry moves at 2.2 against a
		 * Tank's 10.9, and a Devastator at 4.4.  Synchronising the whole spread
		 * means the fast half idles for most of the march, so these hold the base
		 * instead, where speed does not matter and their damage still does. */
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

static bool Doctrine_IsAttacker(uint8 role)
{
	return (role == DOCTRINE_ROLE_ARTILLERY || role == DOCTRINE_ROLE_ASSAULT || role == DOCTRINE_ROLE_RAID);
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
static uint16 Doctrine_PickObjective(uint8 enemy, uint16 approach, uint16 waveCount)
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
			 * wave back around the base. */
			d = Tile_GetDistancePacked(approach, Tile_PackTile(s->o.position));
			if (d >= bestDistance) continue;

			bestDistance = d;
			best = Tools_Index_Encode(s->o.index, IT_STRUCTURE);
		}

		if (best != 0) return best;
	}

	return 0;
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

		if (u == NULL) continue;

		d = Tile_GetDistancePacked(Tile_PackTile(u->o.position), destination);

		/* A leader waits for the tail; a straggler never waits. */
		if (cohesion && column > p->columnHold && d < nearest + p->columnRelease) {
			Doctrine_OrderHold(u);
			continue;
		}

		Doctrine_OrderMove(u, UnitSelection_SpreadTake(u, destination));
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

static void Doctrine_EnterPhase(DoctrineHouse *dh, uint8 phase)
{
	dh->phase = phase;
	dh->phaseStart = g_timerGame;
}

/** Where the next wave forms up: outside the base, on the side the enemy is. */
static uint16 Doctrine_MusterPoint(uint8 houseID, uint8 enemy)
{
	uint16 own = 0;
	uint16 theirs = 0;
	uint8 index;
	tile32 tile;

	for (index = 0; index < SKIRMISH_PLAYER_MAX; index++) {
		uint8 h = Skirmish_GetBaseHouse(index);

		if (h == houseID) own = Skirmish_GetBaseOrigin(index);
		if (h == enemy)   theirs = Skirmish_GetBaseOrigin(index);
	}

	if (own == 0) return 0;
	if (theirs == 0) return own;

	/* Four tiles, not ten: the rally has to sit inside the plateau.  Ten put it
	 * out on open sand on the enemy's side, where the garrison stood in a heap
	 * in front of its own defence line and was taken apart while the harvesters
	 * it was there to protect were hunted down behind it. */
	tile = Tile_MoveByDirection(Tile_UnpackTile(own),
	                            Tile_GetDirection(Tile_UnpackTile(own), Tile_UnpackTile(theirs)),
	                            4 << 8);

	return Tile_PackTile(tile);
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
	uint16 keep;
	uint16 approach;
	uint16 objective;
	PoolFindStruct find;
	uint16 taken = 0;

	dh->musterPacked = Doctrine_MusterPoint(houseID, enemy);
	if (dh->musterPacked == 0) return;

	reserve = Doctrine_CountReserve(houseID, NULL);
	if (reserve < p->minWave) return;

	/* The keep comes out of what is left above the minimum, not on top of it.
	 * Demanding minWave + garrisonKeep before anything moves put the bar at ten
	 * attackers, and a B house that was losing never reached it again: it stood
	 * in its base with seven of them and launched nothing for the rest of the
	 * match. */
	keep = (reserve > (uint16)(p->minWave + p->garrisonKeep)) ? p->garrisonKeep : (uint16)(reserve - p->minWave);

	approach = Doctrine_PickApproach(houseID, enemy);
	if (approach == 0) return;

	objective = Doctrine_PickObjective(enemy, approach, (uint16)(reserve - keep));
	if (objective == 0) return;

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
		if (taken + keep >= reserve) break;

		s_unitOnWave[u->o.index] = 1;
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
			next = Doctrine_PickObjective(enemy, dh->ldPacked, count);
		}

		if (next == 0) {
			Doctrine_DismissWave(houseID);
			Doctrine_EnterPhase(dh, PHASE_MUSTER);
			return;
		}

		dh->objective = next;
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

			if (u == NULL) continue;

			/* Already engaged with something in the way -- leave it alone. */
			if (u->actionID == ACTION_ATTACK && Tools_Index_IsValid(u->targetAttack)
				&& Tools_Index_GetType(u->targetAttack) == IT_UNIT) continue;

			reach = g_table_unitInfo[u->o.type].fireDistance;
			stand = Tile_MoveByDirection(Tile_UnpackTile(objectivePacked),
			                             Tile_GetDirection(Tile_UnpackTile(objectivePacked), u->o.position),
			                             reach << 8);

			Doctrine_OrderAttack(u, dh->objective, UnitSelection_SpreadTake(u, Tile_PackTile(stand)));
		}
	}

	dh->columnLength = 0;
	dh->atLD = 0;
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

	Doctrine_AssignRoles(houseID);

	switch (dh->phase) {
		case PHASE_MUSTER:   Doctrine_PhaseMuster(houseID, dh, enemy);   break;
		case PHASE_APPROACH: Doctrine_PhaseApproach(houseID, dh);        break;
		case PHASE_SUPPRESS: Doctrine_PhaseSuppress(houseID, dh);        break;
		default:             Doctrine_PhaseAssault(houseID, dh, enemy);  break;
	}

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

		total += Skirmish_CountUnitsOfType((uint8)h->index, i);
		if (r == role) mine += Skirmish_CountUnitsOfType((uint8)h->index, i);
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
	 * veto only catches the runaway case: three times over share, which one
	 * production line grinding out the same unit for ten thousand ticks does
	 * reach and nothing healthy does. */
	if (total < DOCTRINE_MIX_FLOOR) return true;

	return (mine * shareSum <= total * p->roleShare[role] * 3);
}

/* -------------------------------------------------------------------------- */
/* Reporting                                                                   */
/* -------------------------------------------------------------------------- */

bool Doctrine_GetTelemetry(uint8 houseID, char *buf, uint16 length)
{
	const DoctrineHouse *dh;

	if (houseID >= HOUSE_MAX || buf == NULL || length == 0) return false;

	dh = &s_house[houseID];

	snprintf(buf, length, "%u,%u,%u,%u,%u,%u",
	         dh->phase, dh->waveCount, dh->atLD, dh->columnLength,
	         (unsigned)dh->wavesLaunched, (unsigned)dh->wavesAborted);

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

	snprintf(buf, length, "B %s w%u/LD%u a%us%ur%ug%u L%uA%u",
	         phaseName[dh->phase & 3], dh->waveCount, dh->atLD,
	         role[DOCTRINE_ROLE_ARTILLERY], role[DOCTRINE_ROLE_ASSAULT],
	         role[DOCTRINE_ROLE_RAID], role[DOCTRINE_ROLE_GARRISON],
	         (unsigned)dh->wavesLaunched, (unsigned)dh->wavesAborted);

	return true;
}
