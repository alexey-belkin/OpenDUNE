/** @file src/unit.c %Unit routines. */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "types.h"
#include "os/common.h"
#include "os/math.h"
#include "os/strings.h"

#include "unit.h"

#include "animation.h"
#include "audio/sound.h"
#include "config.h"
#include "explosion.h"
#include "gui/gui.h"
#include "gui/widget.h"
#include "house.h"
#include "input/mouse.h"
#include "inifile.h"
#include "map.h"
#include "opendune.h"
#include "pool/pool.h"
#include "pool/house.h"
#include "pool/structure.h"
#include "pool/unit.h"
#include "pool/team.h"
#include "skirmish.h"
#include "sprites.h"
#include "string.h"
#include "structure.h"
#include "table/strings.h"
#include "team.h"
#include "tile.h"
#include "timer.h"
#include "tools.h"


static uint32 s_tickUnitMovement  = 0; /*!< Indicates next time the Movement function is executed. */
static uint32 s_tickUnitRotation  = 0; /*!< Indicates next time the Rotation function is executed. */
static uint32 s_tickUnitBlinking  = 0; /*!< Indicates next time the Blinking function is executed. */
static uint32 s_tickUnitUnknown4  = 0; /*!< Indicates next time the Unknown4 function is executed. */
static uint32 s_tickUnitScript    = 0; /*!< Indicates next time the Script function is executed. */
static uint32 s_tickUnitUnknown5  = 0; /*!< Indicates next time the Unknown5 function is executed. */
static uint32 s_tickUnitDeviation = 0; /*!< Indicates next time the Deviation function is executed. */

Unit *g_unitActive = NULL;
Unit *g_unitHouseMissile = NULL;
Unit *g_unitSelected = NULL;

uint16 g_unitSelectionCount = 0;
static uint16 s_unitSelection[UNIT_SELECTION_MAX];
static uint16 s_unitOrder[UNIT_SELECTION_MAX];
/* A target command is modal UI, not a new selection.  Keep an immutable
 * snapshot while its target is being chosen so any legacy UI action that
 * touches g_unitSelected cannot discard the user's group. */
static uint16 s_unitTargetSelection[UNIT_SELECTION_MAX];
static uint16 s_unitTargetSelectionCount = 0;
static bool s_unitTargetSelectionActive = false;
static uint16 s_unitOrderCount = 0;
static ActionType s_unitOrderAction = ACTION_INVALID;
static bool s_unitOrderAirTransit = false;

typedef enum CombatClass {
	COMBAT_CLASS_P = 0,
	COMBAT_CLASS_RP,
	COMBAT_CLASS_LT,
	COMBAT_CLASS_TT,
	COMBAT_CLASS_MAX,
	COMBAT_CLASS_NONE = 0xFF
} CombatClass;

typedef struct CombatBalanceConfig {
	bool enabled;
	bool sharedInfantryProduction;
	uint16 damage[COMBAT_CLASS_MAX][COMBAT_CLASS_MAX];
	uint16 pRangeBonus;
	uint16 atreidesInfantry;
	uint16 harkonnenRocketInfantry;
	uint16 ordosTrike;
} CombatBalanceConfig;

static CombatBalanceConfig s_combatBalance = {
	true,
	true,
	{
		{ 100, 250,  75,  60 },
		{  30, 100, 125, 130 },
		{ 125, 135, 100,  70 },
		{  85,  90, 130, 100 }
	},
	1,
	120,
	110,
	110
};

static uint16 s_combatBalancePBaseRange[2];

static uint16 Unit_CombatBalance_ReadPercent(const char *key, uint16 defaultValue)
{
	int value = IniFile_GetInteger(key, defaultValue);

	if (value < 0) return 0;
	if (value > 1000) return 1000;
	return (uint16)value;
}

static uint16 Unit_CombatBalance_ScaleDamage(uint16 damage, uint16 percent)
{
	uint32 scaled;

	if (damage == 0 || percent == 0) return 0;
	scaled = ((uint32)damage * percent + 50) / 100;
	return (uint16)min(scaled, 0xFFFF);
}

static CombatClass Unit_CombatBalance_GetClass(UnitType type)
{
	switch (type) {
		case UNIT_SOLDIER:
		case UNIT_INFANTRY:     return COMBAT_CLASS_P;
		case UNIT_TROOPER:
		case UNIT_TROOPERS:     return COMBAT_CLASS_RP;
		case UNIT_TRIKE:
		case UNIT_RAIDER_TRIKE:
		case UNIT_QUAD:         return COMBAT_CLASS_LT;
		case UNIT_TANK:
		case UNIT_SIEGE_TANK:
		case UNIT_DEVASTATOR:   return COMBAT_CLASS_TT;
		default:                return COMBAT_CLASS_NONE;
	}
}

/** Load the optional class-balance module and expose both infantry classes. */
void Unit_CombatBalance_Init(void)
{
	static const char *matrixKeys[COMBAT_CLASS_MAX][COMBAT_CLASS_MAX] = {
		{ "class_damage_p_vs_p",  "class_damage_p_vs_rp",  "class_damage_p_vs_lt",  "class_damage_p_vs_tt" },
		{ "class_damage_rp_vs_p", "class_damage_rp_vs_rp", "class_damage_rp_vs_lt", "class_damage_rp_vs_tt" },
		{ "class_damage_lt_vs_p", "class_damage_lt_vs_rp", "class_damage_lt_vs_lt", "class_damage_lt_vs_tt" },
		{ "class_damage_tt_vs_p", "class_damage_tt_vs_rp", "class_damage_tt_vs_lt", "class_damage_tt_vs_tt" }
	};
	uint16 attacker;
	uint16 target;

	s_combatBalance.enabled = IniFile_GetInteger("class_balance_enabled", 1) != 0;
	s_combatBalance.sharedInfantryProduction = IniFile_GetInteger("class_balance_shared_infantry", 1) != 0;
	for (attacker = 0; attacker < COMBAT_CLASS_MAX; attacker++) {
		for (target = 0; target < COMBAT_CLASS_MAX; target++) {
			s_combatBalance.damage[attacker][target] = Unit_CombatBalance_ReadPercent(matrixKeys[attacker][target], s_combatBalance.damage[attacker][target]);
		}
	}
	s_combatBalance.atreidesInfantry = Unit_CombatBalance_ReadPercent("class_bonus_atreides_p", 120);
	s_combatBalance.harkonnenRocketInfantry = Unit_CombatBalance_ReadPercent("class_bonus_harkonnen_rp", 110);
	s_combatBalance.ordosTrike = Unit_CombatBalance_ReadPercent("class_bonus_ordos_trike", 110);
	s_combatBalance.pRangeBonus = (uint16)min(max(IniFile_GetInteger("class_range_p_bonus", 1), 0), 32);

	if (!s_combatBalance.enabled) return;

	/* fireDistance is measured in map cells. Keep the configured result below
	 * the signed fixed-point limit used by the original firing scripts. */
	s_combatBalancePBaseRange[0] = g_table_unitInfo[UNIT_SOLDIER].fireDistance;
	s_combatBalancePBaseRange[1] = g_table_unitInfo[UNIT_INFANTRY].fireDistance;
	g_table_unitInfo[UNIT_SOLDIER].fireDistance = (uint16)min(s_combatBalancePBaseRange[0] + s_combatBalance.pRangeBonus, 127);
	g_table_unitInfo[UNIT_INFANTRY].fireDistance = (uint16)min(s_combatBalancePBaseRange[1] + s_combatBalance.pRangeBonus, 127);

	if (!s_combatBalance.sharedInfantryProduction) return;

	/* Barracks becomes the common infantry factory. WOR remains available as
	 * a specialised legacy factory so existing campaigns and saves still work. */
	g_table_structureInfo[STRUCTURE_BARRACKS].o.availableHouse = FLAG_HOUSE_ALL;
	g_table_structureInfo[STRUCTURE_BARRACKS].buildableUnits[0] = UNIT_SOLDIER;
	g_table_structureInfo[STRUCTURE_BARRACKS].buildableUnits[1] = UNIT_INFANTRY;
	g_table_structureInfo[STRUCTURE_BARRACKS].buildableUnits[2] = UNIT_TROOPER;
	g_table_structureInfo[STRUCTURE_BARRACKS].buildableUnits[3] = UNIT_TROOPERS;
	g_table_unitInfo[UNIT_SOLDIER].o.availableHouse = FLAG_HOUSE_ALL;
	g_table_unitInfo[UNIT_INFANTRY].o.availableHouse = FLAG_HOUSE_ALL;
	g_table_unitInfo[UNIT_TROOPER].o.availableHouse = FLAG_HOUSE_ALL;
	g_table_unitInfo[UNIT_TROOPERS].o.availableHouse = FLAG_HOUSE_ALL;
}

/** Apply a House identity bonus to the base shot, including shots at structures. */
uint16 Unit_CombatBalance_ApplyHouseDamage(const Unit *attacker, uint16 damage)
{
	uint16 percent = 100;

	if (!s_combatBalance.enabled || attacker == NULL) return damage;
	if (attacker->o.houseID == HOUSE_ATREIDES && Unit_CombatBalance_GetClass(attacker->o.type) == COMBAT_CLASS_P) {
		percent = s_combatBalance.atreidesInfantry;
	} else if (attacker->o.houseID == HOUSE_HARKONNEN && Unit_CombatBalance_GetClass(attacker->o.type) == COMBAT_CLASS_RP) {
		percent = s_combatBalance.harkonnenRocketInfantry;
	} else if (attacker->o.houseID == HOUSE_ORDOS && (attacker->o.type == UNIT_TRIKE || attacker->o.type == UNIT_RAIDER_TRIKE)) {
		percent = s_combatBalance.ordosTrike;
	}

	return Unit_CombatBalance_ScaleDamage(damage, percent);
}

/** Apply the attacker/target class matrix to damage already carried by a shot. */
uint16 Unit_CombatBalance_ApplyClassDamage(const Unit *attacker, const Unit *target, uint16 damage)
{
	CombatClass attackerClass;
	CombatClass targetClass;

	if (!s_combatBalance.enabled || attacker == NULL || target == NULL) return damage;
	attackerClass = Unit_CombatBalance_GetClass(attacker->o.type);
	targetClass = Unit_CombatBalance_GetClass(target->o.type);
	if (attackerClass == COMBAT_CLASS_NONE || targetClass == COMBAT_CLASS_NONE) return damage;

	return Unit_CombatBalance_ScaleDamage(damage, s_combatBalance.damage[attackerClass][targetClass]);
}

/** Verify the configured matrix, neutral classes, House bonuses and shared Barracks. */
int Unit_CombatBalance_RunRegressionTest(void)
{
	static const UnitType representatives[COMBAT_CLASS_MAX] = { UNIT_SOLDIER, UNIT_TROOPER, UNIT_TRIKE, UNIT_TANK };
	Unit attacker;
	Unit target;
	uint16 a;
	uint16 t;

	if (!s_combatBalance.enabled) return -1;
	if (g_table_unitInfo[UNIT_SOLDIER].fireDistance != min(s_combatBalancePBaseRange[0] + s_combatBalance.pRangeBonus, 127)) return 0;
	if (g_table_unitInfo[UNIT_INFANTRY].fireDistance != min(s_combatBalancePBaseRange[1] + s_combatBalance.pRangeBonus, 127)) return 0;
	memset(&attacker, 0, sizeof(attacker));
	memset(&target, 0, sizeof(target));
	for (a = 0; a < COMBAT_CLASS_MAX; a++) {
		attacker.o.type = representatives[a];
		for (t = 0; t < COMBAT_CLASS_MAX; t++) {
			target.o.type = representatives[t];
			if (Unit_CombatBalance_ApplyClassDamage(&attacker, &target, 100) != s_combatBalance.damage[a][t]) return 0;
		}
	}

	attacker.o.type = UNIT_LAUNCHER;
	target.o.type = UNIT_TROOPER;
	if (Unit_CombatBalance_ApplyClassDamage(&attacker, &target, 100) != 100) return 0;
	attacker.o.type = UNIT_TROOPER;
	target.o.type = UNIT_SONIC_TANK;
	if (Unit_CombatBalance_ApplyClassDamage(&attacker, &target, 100) != 100) return 0;

	attacker.o.type = UNIT_INFANTRY;
	attacker.o.houseID = HOUSE_ATREIDES;
	if (Unit_CombatBalance_ApplyHouseDamage(&attacker, 100) != s_combatBalance.atreidesInfantry) return 0;
	attacker.o.type = UNIT_TROOPERS;
	attacker.o.houseID = HOUSE_HARKONNEN;
	if (Unit_CombatBalance_ApplyHouseDamage(&attacker, 100) != s_combatBalance.harkonnenRocketInfantry) return 0;
	attacker.o.type = UNIT_RAIDER_TRIKE;
	attacker.o.houseID = HOUSE_ORDOS;
	if (Unit_CombatBalance_ApplyHouseDamage(&attacker, 100) != s_combatBalance.ordosTrike) return 0;
	attacker.o.type = UNIT_QUAD;
	if (Unit_CombatBalance_ApplyHouseDamage(&attacker, 100) != 100) return 0;

	if (s_combatBalance.sharedInfantryProduction) {
		const StructureInfo *si = &g_table_structureInfo[STRUCTURE_BARRACKS];

		if (si->o.availableHouse != FLAG_HOUSE_ALL) return 0;
		if (si->buildableUnits[0] != UNIT_SOLDIER || si->buildableUnits[1] != UNIT_INFANTRY ||
				si->buildableUnits[2] != UNIT_TROOPER || si->buildableUnits[3] != UNIT_TROOPERS) return 0;
	}

	/* Integration path: a real explosion resolves its encoded source, applies
	 * the House bonus carried by the shot, then applies the victim class. */
	{
		Unit *sourceUnit;
		Unit *targetUnit;
		uint16 shotDamage;
		uint16 expectedDamage;
		uint16 targetHitpoints;
		tile32 offMap;

		g_playerHouseID = HOUSE_ATREIDES;
		g_playerHouse = House_Get_ByIndex(g_playerHouseID);
		g_playerHouse->unitCountMax = UNIT_SELECTION_MAX;
		House_Get_ByIndex(HOUSE_HARKONNEN)->unitCountMax = UNIT_SELECTION_MAX;
		offMap.x = 0xFFFF;
		offMap.y = 0xFFFF;
		sourceUnit = Unit_Create(UNIT_INDEX_INVALID, UNIT_SOLDIER, HOUSE_ATREIDES, offMap, 0);
		targetUnit = Unit_Create(UNIT_INDEX_INVALID, UNIT_TROOPER, HOUSE_HARKONNEN, offMap, 0);
		if (sourceUnit == NULL || targetUnit == NULL) {
			printf("combat-balance integration setup failed: source=%p target=%p\n", (void *)sourceUnit, (void *)targetUnit);
			return 0;
		}
		sourceUnit->o.position = Tile_UnpackTile(Tile_PackXY(4, 4));
		sourceUnit->o.flags.s.isNotOnMap = false;
		targetUnit->o.position = Tile_UnpackTile(Tile_PackXY(20, 20));
		targetUnit->o.flags.s.isNotOnMap = false;

		shotDamage = Unit_CombatBalance_ApplyHouseDamage(sourceUnit, 10);
		expectedDamage = Unit_CombatBalance_ApplyClassDamage(sourceUnit, targetUnit, shotDamage);
		targetHitpoints = targetUnit->o.hitpoints;
		Map_MakeExplosion(EXPLOSION_IMPACT_SMALL, targetUnit->o.position, shotDamage, Tools_Index_Encode(sourceUnit->o.index, IT_UNIT));
		if (targetUnit->o.hitpoints != targetHitpoints - min(targetHitpoints, expectedDamage)) {
			printf("combat-balance integration mismatch: shot=%u expected=%u hp=%u->%u\n", shotDamage, expectedDamage, targetHitpoints, targetUnit->o.hitpoints);
			return 0;
		}
	}

	return 1;
}

/* Runtime-only tactical state.  The actual route remains owned by the unit
 * script, so save-game layouts and the normal movement system stay intact. */
static bool s_attackPositionManual[UNIT_INDEX_MAX];
static uint16 s_attackPositionTile[UNIT_INDEX_MAX];
static uint16 s_attackPositionTarget[UNIT_INDEX_MAX];
static uint32 s_attackPositionETA[UNIT_INDEX_MAX];
static uint32 s_attackPositionNextCheck[UNIT_INDEX_MAX];
/* A defensive sortie has one owner and one lifecycle.  Keeping this state in
 * one place prevents the legacy action script, tactical target picker and
 * command UI from each trying to restore a different "home" action. */
typedef enum AutonomousPostState {
	AUTONOMOUS_POST_NONE,
	AUTONOMOUS_POST_ENGAGING,
	AUTONOMOUS_POST_RETURNING
} AutonomousPostState;

typedef struct AutonomousPost {
	uint16 anchor;
	ActionType action;
	AutonomousPostState state;
} AutonomousPost;

static AutonomousPost s_autonomousPost[UNIT_INDEX_MAX];
/* Set only by the player-order entry points.  It separates an explicit Attack
 * from a legacy Guard script switching itself to Attack. */
static bool s_manualOrderStarting[UNIT_INDEX_MAX];
static bool s_manualHunt[UNIT_INDEX_MAX];
static uint32 s_autonomyNextCheck[UNIT_INDEX_MAX];

/* Per-harvester bookkeeping.  It is deliberately kept out of struct Unit: none
 * of it needs to survive a save game, and all of it is reset by
 * Unit_Harvester_ResetState() when an index is handed to a new unit. */
typedef struct HarvesterTracker {
	uint32 nextCheck;                /*!< Throttle for the periodic maintenance pass. */
	uint16 lastPosition;             /*!< Packed tile seen at the previous check. */
	uint32 lastProgress;             /*!< Game time the unit last changed tile. */
	uint16 refineryTarget;           /*!< Encoded refinery the return trip is aimed at. */
	uint32 refineryStalledSince;     /*!< Game time the approach stopped making progress. */
	uint32 refineryRetarget;         /*!< Earliest game time the return trip may be re-aimed again. */
	uint32 airliftDeadline;          /*!< Game time the wait for a carryall gives up. */
	uint32 liftCheck;                /*!< Throttle for the "is a carryall free now?" retry. */
	uint16 spiceCache;               /*!< Memo for Unit_Harvester_FindPreferredSpice(). */
	uint16 spiceCacheCenter;         /*!< harvestCenter the memo was computed for. */
	uint32 spiceCacheUntil;          /*!< Game time the memo expires. */
	bool   forcedReturn;             /*!< Player pressed Return: unload even when not full. */
} HarvesterTracker;

static HarvesterTracker s_harvester[UNIT_INDEX_MAX];
static uint16 s_houseThreatTarget[HOUSE_MAX];
static uint32 s_houseThreatUntil[HOUSE_MAX];

static void Unit_AirTransit_Update(Unit *unit);

static void Unit_Harvester_ResetState(uint16 index)
{
	if (index >= UNIT_INDEX_MAX) return;
	memset(&s_harvester[index], 0, sizeof(s_harvester[index]));
}

static void Unit_Autonomy_ClearPost(Unit *unit)
{
	if (unit == NULL || unit->o.index >= UNIT_INDEX_MAX) return;
	s_autonomousPost[unit->o.index].anchor = 0;
	s_autonomousPost[unit->o.index].action = ACTION_INVALID;
	s_autonomousPost[unit->o.index].state = AUTONOMOUS_POST_NONE;
}

void Unit_BeginManualOrder(Unit *unit)
{
	if (unit == NULL || unit->o.index >= UNIT_INDEX_MAX) return;
	s_manualOrderStarting[unit->o.index] = true;
	Unit_Autonomy_ClearPost(unit);
}

static const int8 s_firingPositionDirectionX[16] = {4, 4, 3, 2, 0, -2, -3, -4, -4, -4, -3, -2, 0, 2, 3, 4};
static const int8 s_firingPositionDirectionY[16] = {0, -2, -3, -4, -4, -4, -3, -2, 0, 2, 3, 4, 4, 4, 3, 2};

static bool Unit_AttackPosition_IsEligible(Unit *unit)
{
	const UnitInfo *ui;

	if (unit == NULL || !unit->o.flags.s.used || !unit->o.flags.s.allocated || unit->o.flags.s.isNotOnMap) return false;
	if (!s_attackPositionManual[unit->o.index] || unit->actionID != ACTION_ATTACK) return false;
	if (Unit_GetHouseID(unit) != g_playerHouseID || !Tools_Index_IsValid(unit->targetAttack)) return false;

	ui = &g_table_unitInfo[unit->o.type];
	if (!ui->flags.isNormalUnit || !ui->flags.isGroundUnit || ui->fireDistance == 0) return false;

	return ui->movementType == MOVEMENT_FOOT || ui->movementType == MOVEMENT_TRACKED || ui->movementType == MOVEMENT_WHEELED;
}

static void Unit_AttackPosition_Clear(Unit *unit)
{
	if (unit == NULL || unit->o.index >= UNIT_INDEX_MAX) return;
	s_attackPositionTile[unit->o.index] = 0;
	s_attackPositionTarget[unit->o.index] = 0;
	s_attackPositionETA[unit->o.index] = 0;
	s_attackPositionNextCheck[unit->o.index] = 0;
}

/** Enable or clear tactical firing positions for a player-issued order. */
void Unit_AttackPosition_SetManual(Unit *unit, bool enabled)
{
	if (unit == NULL || unit->o.index >= UNIT_INDEX_MAX) return;

	s_attackPositionManual[unit->o.index] = enabled;
	Unit_Autonomy_ClearPost(unit);
	Unit_AttackPosition_Clear(unit);
	if (enabled) s_attackPositionNextCheck[unit->o.index] = g_timerGame + (unit->o.index % 7) * 3;
}

static void Unit_AttackPosition_SetAutomatic(Unit *unit, ActionType returnAction)
{
	if (unit == NULL || unit->o.index >= UNIT_INDEX_MAX) return;

	/* A sortie can be opened while the unit is already moving or attacking; the
	 * post has to remember a guard mode to come back to, not the action it
	 * happened to carry at that moment. */
	if (returnAction != ACTION_GUARD && returnAction != ACTION_AREA_GUARD && returnAction != ACTION_HUNT) {
		returnAction = Unit_GetDefaultAction(unit);
	}

	s_attackPositionManual[unit->o.index] = true;
	s_autonomousPost[unit->o.index].anchor = unit->guardPosition;
	s_autonomousPost[unit->o.index].action = returnAction;
	s_autonomousPost[unit->o.index].state = AUTONOMOUS_POST_ENGAGING;
	Unit_AttackPosition_Clear(unit);
	s_attackPositionNextCheck[unit->o.index] = g_timerGame + (unit->o.index % 7) * 3;
}

/* A sandworm is never a movement destination.  It travels through sand and
 * swallows whatever stands on it, so the only safe way to engage one is from
 * rock, which Unit_AttackPosition_Update() picks explicitly.  The original
 * scripts know nothing about that: Script_Unit_SetTarget() copies the target
 * into targetMove for every unit without a turret, and the Area Guard routine
 * drives straight at it (UNIT.EMC word 519).  Refusing the destination at the
 * two setters is what actually stops the chase - the scripts run four times as
 * often as the tactical pass, so clearing targetMove afterwards only loses the
 * tug of war. */
bool Unit_IsSandwormTarget(uint16 encoded)
{
	const Unit *u = Tools_Index_GetUnit(encoded);

	return u != NULL && u->o.type == UNIT_SANDWORM;
}

/* True unless the step would carry the unit onto sand and closer to the worm it
 * has taken as its target. */
static bool Unit_Sandworm_StepAllowed(const Unit *unit, uint16 packed)
{
	const Unit *worm = Tools_Index_GetUnit(unit->targetAttack);

	if (worm == NULL || worm->o.type != UNIT_SANDWORM) return true;
	if (unit->o.type == UNIT_SANDWORM) return true;
	if (!g_table_landscapeInfo[Map_GetLandscapeType(packed)].isSand) return true;

	return Tile_GetDistance(Tile_UnpackTile(packed), worm->o.position) >= Tile_GetDistance(unit->o.position, worm->o.position);
}

/* A sandworm travels through sand and cannot touch rock, so a unit that engages
 * one has exactly one safe place to do it from.  Firing back from a dune only
 * offers the worm its next meal, and driving after one is worse still.  The
 * restriction is deliberately independent of the House: which units bother to
 * shoot at a worm is a matter of target priority, but where they may stand
 * while doing it is not. */
static bool Unit_AttackPosition_IsFiringTileAllowed(uint16 target, uint16 packed)
{
	const Unit *targetUnit = Tools_Index_GetUnit(target);

	if (targetUnit == NULL || targetUnit->o.type != UNIT_SANDWORM) return true;

	return !g_table_landscapeInfo[Map_GetLandscapeType(packed)].isSand;
}

/* Estimate the fastest reachable firing tile with exactly the same terrain,
 * movement speed and route calculation used by firing-position reservations. */
static bool Unit_AttackPosition_EstimateTravel(Unit *unit, uint16 target, uint32 *travelTicks)
{
	const UnitInfo *ui;
	Structure *targetStructure;
	uint16 packedSource;
	uint16 packedTarget;
	uint16 radius;
	uint16 minRadius;
	uint16 dir;
	uint32 best = 0xFFFFFFFF;
	int16 left = 0;
	int16 right = 0;
	int16 top = 0;
	int16 bottom = 0;

	if (unit == NULL || !Tools_Index_IsValid(target)) return false;
	ui = &g_table_unitInfo[unit->o.type];
	if (ui->fireDistance == 0) return false;
	if (Object_GetDistanceToEncoded(&unit->o, target) <= (ui->fireDistance << 8)) {
		if (travelTicks != NULL) *travelTicks = 0;
		return true;
	}

	packedSource = Tile_PackTile(unit->o.position);
	packedTarget = Tools_Index_GetPackedTile(target);
	if (!Map_IsValidPosition(packedSource) || !Map_IsValidPosition(packedTarget)) return false;

	targetStructure = Tools_Index_GetStructure(target);
	if (targetStructure != NULL) {
		const XYSize *size = &g_table_structure_layoutSize[g_table_structureInfo[targetStructure->o.type].layout];

		left = Tile_GetPackedX(Tile_PackTile(targetStructure->o.position));
		top = Tile_GetPackedY(Tile_PackTile(targetStructure->o.position));
		right = left + size->width - 1;
		bottom = top + size->height - 1;
	}

	minRadius = ui->fireDistance > 2 ? ui->fireDistance - 2 : 1;
	for (radius = ui->fireDistance; radius >= minRadius; radius--) {
		for (dir = 0; dir < lengthof(s_firingPositionDirectionX); dir++) {
			int16 x;
			int16 y;
			uint16 packed;
			uint32 ticks;
			Object candidate;

			if (targetStructure == NULL) {
				x = Tile_GetPackedX(packedTarget) + (radius * s_firingPositionDirectionX[dir] + 2) / 4;
				y = Tile_GetPackedY(packedTarget) + (radius * s_firingPositionDirectionY[dir] + 2) / 4;
			} else {
				uint16 offsetX = (radius * abs(s_firingPositionDirectionX[dir]) + 2) / 4;
				uint16 offsetY = (radius * abs(s_firingPositionDirectionY[dir]) + 2) / 4;

				x = s_firingPositionDirectionX[dir] > 0 ? right + offsetX : (s_firingPositionDirectionX[dir] < 0 ? left - offsetX : (left + right) / 2);
				y = s_firingPositionDirectionY[dir] > 0 ? bottom + offsetY : (s_firingPositionDirectionY[dir] < 0 ? top - offsetY : (top + bottom) / 2);
			}

			if (x < 0 || x >= 64 || y < 0 || y >= 64) continue;
			packed = Tile_PackXY(x, y);
			if (!Map_IsValidPosition(packed) || Object_GetByPackedTile(packed) != NULL) continue;
			if (!Unit_AttackPosition_IsFiringTileAllowed(target, packed)) continue;
			if (Unit_GetTileEnterScore(unit, packed, 0) > 255) continue;
			if (!Script_Unit_HasRoute(unit, packedSource, packed, &ticks)) continue;

			candidate = unit->o;
			candidate.position = Tile_UnpackTile(packed);
			if (Object_GetDistanceToEncoded(&candidate, target) > (ui->fireDistance << 8)) continue;
			if (ticks < best) best = ticks;
		}
		if (radius == minRadius) break;
	}

	if (best == 0xFFFFFFFF) return false;
	if (travelTicks != NULL) *travelTicks = best;
	return true;
}

/* The tile the tactical layer currently has this unit heading for, 0 when it
 * has none.  Exposed for the debug overlay: from the outside a unit on its way
 * to a firing position looks exactly like one wandering off. */
uint16 Unit_AttackPosition_GetTile(const Unit *unit)
{
	if (unit == NULL || unit->o.index >= UNIT_INDEX_MAX) return 0;
	if (s_attackPositionTarget[unit->o.index] != unit->targetAttack) return 0;

	return s_attackPositionTile[unit->o.index];
}

/* True while the firing-position layer is placing this unit: a player Attack
 * order or an autonomous sortie.  Its destination is then owned by that layer. */
bool Unit_AttackPosition_IsManaged(const Unit *unit)
{
	if (unit == NULL || unit->o.index >= UNIT_INDEX_MAX) return false;

	return s_attackPositionManual[unit->o.index] && Tools_Index_IsValid(unit->targetAttack);
}

/* How a unit under tactical control may approach its target.
 *
 * Attacking is supposed to mean "go to the firing position picked for me", but
 * the original attack script paths straight at targetAttack (UNIT.EMC word 213)
 * for every unit, and it does so without touching targetMove, so nothing at the
 * order level can redirect it.  That is what drove units to point-blank range
 * whatever the tactical layer had decided - and into sandworms.
 *
 * Returns false when the unit must not path at all: it is ours to place, and no
 * position has been assigned yet, or none exists.  Holding for a tactical pass
 * is the correct answer there; approaching the target itself never is. */
bool Unit_AttackPosition_GetApproach(Unit *unit, uint16 encoded, uint16 *destination)
{
	if (unit == NULL || destination == NULL || unit->o.index >= UNIT_INDEX_MAX) return true;
	if (!s_attackPositionManual[unit->o.index]) return true;
	if (!Tools_Index_IsValid(unit->targetAttack) || encoded != unit->targetAttack) return true;

	/* Whatever else is true, a unit that can already shoot does not move.  This
	 * is what stops the pendulum: the firing ring lies at the outer edge of
	 * weapon range, so a unit that has drifted inside it would otherwise be
	 * pulled back out again, step in again, and never stand still long enough
	 * to fire. */
	if (Object_GetDistanceToEncoded(&unit->o, unit->targetAttack) <= (g_table_unitInfo[unit->o.type].fireDistance << 8)) return false;

	if (s_attackPositionTile[unit->o.index] == 0) return false;
	if (s_attackPositionTarget[unit->o.index] != unit->targetAttack) return false;

	*destination = Tools_Index_Encode(s_attackPositionTile[unit->o.index], IT_TILE);
	return true;
}

/* Returns the owner of a matching reservation, or -1 when the tile is free. */
static int16 Unit_AttackPosition_GetReservationOwner(const Unit *unit, uint16 packed, uint16 target)
{
	uint16 i;

	for (i = 0; i < UNIT_INDEX_MAX; i++) {
		Unit *other;

		if (i == unit->o.index) continue;
		if (!s_attackPositionManual[i] || s_attackPositionTile[i] != packed || s_attackPositionTarget[i] != target) continue;
		other = Unit_Get_ByIndex(i);
		if (!other->o.flags.s.used || other->actionID != ACTION_ATTACK) continue;
		return i;
	}

	return -1;
}

/* A waiting spot just outside weapon range, used when every firing position is
 * taken.  Standing at the edge of the fight is what the original script bought
 * by driving at the target - the unit is there to step into a slot the moment
 * one opens - except that it stops short of the target's own guns instead of
 * ending up under them.  A sandworm keeps the same rule as everywhere: rock
 * only, so the wait is never spent standing in its path. */
static uint16 Unit_AttackPosition_FindStaging(Unit *unit, uint32 *travelTicks)
{
	const UnitInfo *ui = &g_table_unitInfo[unit->o.type];
	uint16 packedSource = Tile_PackTile(unit->o.position);
	uint16 packedTarget = Tools_Index_GetPackedTile(unit->targetAttack);
	uint16 best = 0;
	uint32 bestTicks = 0xFFFFFFFF;
	uint16 current;
	uint16 radius;
	uint16 dir;

	if (!Map_IsValidPosition(packedSource) || !Map_IsValidPosition(packedTarget)) return 0;

	/* Waiting is only ever worth a step forward.  A unit already at the edge of
	 * the fight stays where it is: sending it to another tile of the same ring,
	 * or worse to a farther one, is the pendulum in its purest form. */
	current = Tile_GetDistancePacked(packedSource, packedTarget);
	if (current <= ui->fireDistance + 3) return 0;

	for (radius = ui->fireDistance + 1; radius <= ui->fireDistance + 3; radius++) {
		for (dir = 0; dir < lengthof(s_firingPositionDirectionX); dir++) {
			int16 x = Tile_GetPackedX(packedTarget) + (radius * s_firingPositionDirectionX[dir] + 2) / 4;
			int16 y = Tile_GetPackedY(packedTarget) + (radius * s_firingPositionDirectionY[dir] + 2) / 4;
			uint16 packed;
			uint32 ticks;

			if (x < 0 || x >= 64 || y < 0 || y >= 64) continue;
			packed = Tile_PackXY(x, y);
			/* Already waiting on a good tile: staying put beats shuffling. */
			if (packed == packedSource) return 0;
			if (!Map_IsValidPosition(packed) || Object_GetByPackedTile(packed) != NULL) continue;
			if (!Unit_AttackPosition_IsFiringTileAllowed(unit->targetAttack, packed)) continue;
			if (Tile_GetDistancePacked(packed, packedTarget) >= current) continue;
			if (Unit_GetTileEnterScore(unit, packed, 0) > 255) continue;
			if (Unit_AttackPosition_GetReservationOwner(unit, packed, unit->targetAttack) >= 0) continue;
			if (!Script_Unit_HasRoute(unit, packedSource, packed, &ticks)) continue;
			if (ticks >= bestTicks) continue;

			bestTicks = ticks;
			best = packed;
		}
	}

	if (best != 0 && travelTicks != NULL) *travelTicks = bestTicks;
	return best;
}

/* Pick a reachable, unclaimed tile near the outer edge of weapon range. */
static void Unit_AttackPosition_Update(Unit *unit)
{
	const UnitInfo *ui;
	Structure *targetStructure;
	uint16 packedSource;
	uint16 packedTarget;
	uint16 oldPacked;
	uint16 bestPacked = 0;
	uint16 bestDistance = 0;
	int16 bestOwner = -1;
	uint32 bestETA = 0xFFFFFFFF;
	uint32 oldETA = 0;
	uint32 refreshedOldETA = 0;
	uint16 targetDistance;
	uint16 radius;
	uint16 minRadius;
	uint16 dir;
	int16 left = 0;
	int16 right = 0;
	int16 top = 0;
	int16 bottom = 0;

	if (!Unit_AttackPosition_IsEligible(unit)) {
		Unit_AttackPosition_Clear(unit);
		return;
	}

	if (s_attackPositionNextCheck[unit->o.index] > g_timerGame) return;
	s_attackPositionNextCheck[unit->o.index] = g_timerGame + 30;

	ui = &g_table_unitInfo[unit->o.type];
	packedSource = Tile_PackTile(unit->o.position);
	packedTarget = Tools_Index_GetPackedTile(unit->targetAttack);
	if (!Map_IsValidPosition(packedSource) || !Map_IsValidPosition(packedTarget)) {
		Unit_AttackPosition_Clear(unit);
		return;
	}

	/* Structures occupy several map tiles.  Their attack range is measured to
	 * the nearest edge, so tactical positions must be generated from the full
	 * footprint instead of the structure's upper-left anchor tile. */
	targetStructure = Tools_Index_GetStructure(unit->targetAttack);
	if (targetStructure != NULL) {
		const XYSize *size = &g_table_structure_layoutSize[g_table_structureInfo[targetStructure->o.type].layout];

		left = Tile_GetPackedX(Tile_PackTile(targetStructure->o.position));
		top = Tile_GetPackedY(Tile_PackTile(targetStructure->o.position));
		right = left + size->width - 1;
		bottom = top + size->height - 1;
	}

	/* A unit already in range keeps firing instead of chasing a new slot. */
	targetDistance = Object_GetDistanceToEncoded(&unit->o, unit->targetAttack);
	if (targetDistance <= (ui->fireDistance << 8)) {
		if (unit->targetMove != 0 &&
			(unit->targetMove == unit->targetAttack ||
			 (s_attackPositionTile[unit->o.index] != 0 && unit->targetMove == Tools_Index_Encode(s_attackPositionTile[unit->o.index], IT_TILE)))) {
			unit->targetMove = 0;
			unit->route[0] = 0xFF;
		}
		Unit_AttackPosition_Clear(unit);
		return;
	}

	oldPacked = s_attackPositionTarget[unit->o.index] == unit->targetAttack ? s_attackPositionTile[unit->o.index] : 0;
	minRadius = ui->fireDistance > 2 ? ui->fireDistance - 2 : 1;

	for (radius = ui->fireDistance; radius >= minRadius; radius--) {
		for (dir = 0; dir < lengthof(s_firingPositionDirectionX); dir++) {
			int16 x;
			int16 y;
			uint16 packed;
			uint32 travelTicks;
			uint32 eta;
			uint16 firingDistance;
			int16 owner;
			Object candidate;

			if (targetStructure == NULL) {
				x = Tile_GetPackedX(packedTarget) + (radius * s_firingPositionDirectionX[dir] + 2) / 4;
				y = Tile_GetPackedY(packedTarget) + (radius * s_firingPositionDirectionY[dir] + 2) / 4;
			} else {
				uint16 offsetX = (radius * abs(s_firingPositionDirectionX[dir]) + 2) / 4;
				uint16 offsetY = (radius * abs(s_firingPositionDirectionY[dir]) + 2) / 4;

				x = s_firingPositionDirectionX[dir] > 0 ? right + offsetX : (s_firingPositionDirectionX[dir] < 0 ? left - offsetX : (left + right) / 2);
				y = s_firingPositionDirectionY[dir] > 0 ? bottom + offsetY : (s_firingPositionDirectionY[dir] < 0 ? top - offsetY : (top + bottom) / 2);
			}

			if (x < 0 || x >= 64 || y < 0 || y >= 64) continue;
			packed = Tile_PackXY(x, y);
			if (!Map_IsValidPosition(packed) || packed == packedSource) continue;
			if (Object_GetByPackedTile(packed) != NULL) continue;
			if (!Unit_AttackPosition_IsFiringTileAllowed(unit->targetAttack, packed)) continue;
			if (Unit_GetTileEnterScore(unit, packed, 0) > 255) continue;
			if (!Script_Unit_HasRoute(unit, packedSource, packed, &travelTicks)) continue;

			candidate = unit->o;
			candidate.position = Tile_UnpackTile(packed);
			firingDistance = Object_GetDistanceToEncoded(&candidate, unit->targetAttack);
			if (firingDistance > (ui->fireDistance << 8)) continue;
			eta = g_timerGame + travelTicks;

			owner = Unit_AttackPosition_GetReservationOwner(unit, packed, unit->targetAttack);
			/* A closer arrival may preempt this slot, but only with a wide
			 * enough margin to prevent two units from trading it every check. */
			if (owner >= 0 && s_attackPositionETA[owner] <= eta + 60) continue;

			if (packed == oldPacked) refreshedOldETA = eta;

			/* Arrival time is the primary ordering. At an exact tie, preserve
			 * the outer ring preference of the original implementation. */
			if (eta > bestETA) continue;
			if (eta == bestETA && bestPacked != 0 && firingDistance <= bestDistance) continue;
			bestETA = eta;
			bestPacked = packed;
			bestDistance = firingDistance;
			bestOwner = owner;
		}

		if (radius == minRadius) break; /* avoid unsigned wrap */
	}

	/* Script_Unit_Pathfinder() answers from the tile the unit is standing on and
	 * happily returns a partial route, which Script_Unit_HasRoute() then has to
	 * reject.  So the very same slot is unreachable on one pass and reachable on
	 * the next, purely because the unit moved a tile.  Dropping the reservation
	 * whenever a pass cannot confirm it destroys the hysteresis below and turns
	 * the approach into a pendulum: forward two tiles, back two tiles, never
	 * standing still long enough to fire.  A slot that this pass failed to
	 * confirm is kept unless somebody else has taken the tile. */
	if (oldPacked != 0 && oldPacked != packedSource && refreshedOldETA == 0 && Object_GetByPackedTile(oldPacked) == NULL) return;

	if (bestPacked == 0) {
		/* Same reasoning for a pass that found nothing at all. */
		if (s_attackPositionTile[unit->o.index] != 0 && s_attackPositionTarget[unit->o.index] == unit->targetAttack) return;
		if (refreshedOldETA != 0) s_attackPositionETA[unit->o.index] = refreshedOldETA;
		else {
			const Unit *targetUnit = Tools_Index_GetUnit(unit->targetAttack);

			Unit_AttackPosition_Clear(unit);
			if (targetUnit != NULL && targetUnit->o.type == UNIT_SANDWORM) {
				/* No rock in reach: hold instead of rolling onto the sand after
				 * the worm.  currentDestination stays untouched so the step in
				 * progress can finish; dropping targetMove is what stops the
				 * chase, including the target the guard scripts write there
				 * themselves for turretless units. */
				unit->targetMove = 0;
				unit->route[0] = 0xFF;
				Unit_Autonomy_ReturnToPost(unit);
			} else {
				/* No firing slot of our own: take a waiting spot at the edge of
				 * the fight and claim it, so a compact target does not leave half
				 * the force standing where it was.  The claim is what keeps the
				 * queue spread out instead of stacked on one tile, and it is the
				 * single answer to "no slot" - driving at a slot another unit has
				 * already claimed only trades the tile back and forth on arrival.
				 * There is deliberately no fallback onto the target itself either:
				 * approaching a target is only ever a means of reaching a firing
				 * position. */
				uint32 stagingTicks = 0;
				uint16 staging = Unit_AttackPosition_FindStaging(unit, &stagingTicks);

				if (staging != 0) {
					s_attackPositionTile[unit->o.index] = staging;
					s_attackPositionTarget[unit->o.index] = unit->targetAttack;
					s_attackPositionETA[unit->o.index] = g_timerGame + stagingTicks;
					Unit_SetDestination(unit, Tools_Index_Encode(staging, IT_TILE));
				}
			}
		}
		return;
	}

	/* Keep an existing slot unless the replacement arrives at least 60 ticks
	 * sooner.  This is the hysteresis counterpart to reservation preemption. */
	if (oldPacked != 0 && refreshedOldETA != 0 && bestPacked != oldPacked) {
		oldETA = refreshedOldETA;
		if (bestETA + 60 >= oldETA) {
			s_attackPositionETA[unit->o.index] = oldETA;
			return;
		}
	}

	if (bestOwner >= 0) Unit_AttackPosition_Clear(Unit_Get_ByIndex(bestOwner));

	s_attackPositionTile[unit->o.index] = bestPacked;
	s_attackPositionTarget[unit->o.index] = unit->targetAttack;
	s_attackPositionETA[unit->o.index] = bestETA;
	Unit_SetDestination(unit, Tools_Index_Encode(bestPacked, IT_TILE));
}

static bool Unit_Autonomy_IsCombatUnit(const Unit *unit)
{
	const UnitInfo *ui;
	uint8 houseID;

	if (unit == NULL || !unit->o.flags.s.used || !unit->o.flags.s.allocated || unit->o.flags.s.isNotOnMap) return false;
	houseID = unit->deviated != 0 ? (g_dune2_enhanced ? unit->deviatedHouse : HOUSE_ORDOS) : unit->o.houseID;
	if (houseID != g_playerHouseID) return false;

	ui = &g_table_unitInfo[unit->o.type];
	if (!ui->flags.isNormalUnit || !ui->flags.isGroundUnit || ui->fireDistance == 0) return false;
	return ui->movementType == MOVEMENT_FOOT || ui->movementType == MOVEMENT_TRACKED || ui->movementType == MOVEMENT_WHEELED;
}

static uint16 Unit_Autonomy_GetSearchRadius(const Unit *unit)
{
	const AutonomousPost *post = &s_autonomousPost[unit->o.index];
	ActionType action = (ActionType)unit->actionID;
	uint16 fireDistance;

	if (s_manualHunt[unit->o.index]) return 63;

	/* A unit in a sortie carries Attack or Move as its action, but the zone it
	 * answers to is still the guard mode it will return to.  Without this the
	 * radius would read as zero for exactly the two states in which the unit is
	 * away from its post and most needs to know how far its area reaches. */
	if (post->state != AUTONOMOUS_POST_NONE && post->action != ACTION_INVALID) action = post->action;

	/* The radius is measured from the post, and a unit stops as soon as its
	 * target is within its own weapon range, so what it really controls is how
	 * far the unit may leave the post: radius minus fireDistance.  A flat
	 * radius therefore meant something different for every unit, and for Rocket
	 * Troopers (range 5, radius 5) it meant zero - they only ever acquired what
	 * they could already shoot and never took a step.  Both modes are now
	 * defined by that margin instead: Guard leaves the post by at most five
	 * tiles whatever the unit carries, Area Guard keeps its wider fixed area. */
	fireDistance = g_table_unitInfo[unit->o.type].fireDistance;

	switch (action) {
		case ACTION_GUARD:      return max(5, fireDistance + 5);
		case ACTION_AREA_GUARD: return max(14, fireDistance + 5);
		case ACTION_HUNT:       return 63;
		default:                return 0;
	}
}

/* Player-issued Hunt is deliberately kept outside the original unit script.
 * That script can recurse for an already-running Quad; the autonomous layer
 * supplies map-wide target selection without touching its stack. */
void Unit_SetManualHunt(Unit *unit, bool enabled)
{
	if (unit == NULL || unit->o.index >= UNIT_INDEX_MAX) return;
	s_manualHunt[unit->o.index] = enabled;
}

static bool Unit_Autonomy_TargetInArea(const Unit *unit, uint16 target)
{
	const AutonomousPost *post = &s_autonomousPost[unit->o.index];
	uint16 radius = Unit_Autonomy_GetSearchRadius(unit);
	uint16 anchor = unit->guardPosition;

	if (radius == 0 || !Tools_Index_IsValid(target)) return false;
	if (s_manualHunt[unit->o.index] || unit->actionID == ACTION_HUNT) return true;

	/* The zone is measured from the guard post, never from where the unit
	 * happens to stand.  During a sortie that is the anchor the post was opened
	 * with, so a unit that has already moved out keeps answering for the same
	 * piece of ground - both when it looks for a new target and when it decides
	 * that the one it has has left. */
	if (post->state != AUTONOMOUS_POST_NONE && Map_IsValidPosition(post->anchor)) anchor = post->anchor;
	if (!Map_IsValidPosition(anchor)) anchor = Tile_PackTile(unit->o.position);

	return Tile_GetDistance(Tile_UnpackTile(anchor), Tools_Index_GetTile(target)) <= (radius << 8);
}

static uint32 Unit_Autonomy_BasePriority(Unit *unit, uint16 target, uint16 *maxHitpoints, uint16 *hitpoints)
{
	Unit *targetUnit;
	Structure *targetStructure;

	if (maxHitpoints != NULL) *maxHitpoints = 1;
	if (hitpoints != NULL) *hitpoints = 1;

	targetUnit = Tools_Index_GetUnit(target);
	if (targetUnit != NULL) {
		const UnitInfo *ti = &g_table_unitInfo[targetUnit->o.type];

		if (Unit_GetTargetUnitPriority(unit, targetUnit) == 0) return 0;
		if (maxHitpoints != NULL) *maxHitpoints = ti->o.hitpoints;
		if (hitpoints != NULL) *hitpoints = targetUnit->o.hitpoints;

		/* A sandworm ranks below everything else.  Its damage of 300 makes the
		 * formula below value it above any tank, which is how a defence walked
		 * away from the base it was standing on.  Nothing that shoots back is
		 * ever worth less than the worm, so it is engaged only when the area is
		 * otherwise empty - and then only from rock. */
		if (targetUnit->o.type == UNIT_SANDWORM) return 1;

		return ti->o.priorityTarget + ti->o.priorityBuild + ti->damage * 12;
	}

	targetStructure = Tools_Index_GetStructure(target);
	if (targetStructure != NULL) {
		const StructureInfo *si = &g_table_structureInfo[targetStructure->o.type];

		if (Unit_GetTargetStructurePriority(unit, targetStructure) == 0) return 0;
		if (maxHitpoints != NULL) *maxHitpoints = si->o.hitpoints;
		if (hitpoints != NULL) *hitpoints = targetStructure->o.hitpoints;
		return si->o.priorityTarget + si->o.priorityBuild;
	}

	return 0;
}

static void Unit_Autonomy_AddCandidate(uint16 *candidates, uint32 *scores, uint16 *count, uint16 target, uint32 score)
{
	uint16 i;
	uint16 insert = *count;

	for (i = 0; i < *count; i++) {
		if (score > scores[i]) {
			insert = i;
			break;
		}
	}
	if (insert >= 8) return;
	if (*count < 8) (*count)++;
	for (i = *count - 1; i > insert; i--) {
		candidates[i] = candidates[i - 1];
		scores[i] = scores[i - 1];
	}
	candidates[insert] = target;
	scores[insert] = score;
}

/* Approximate the damage already committed to a target over the next short
 * exchange. It favours focus fire until the target is covered, then makes
 * extra attackers look for another vulnerable threat. */
static uint32 Unit_Autonomy_IncomingDamage(Unit *unit, uint16 target)
{
	PoolFindStruct find;
	uint32 damage = 0;

	find.houseID = HOUSE_INVALID;
	find.index = 0xFFFF;
	find.type = 0xFFFF;
	while (true) {
		const UnitInfo *ui;
		Unit *other = Unit_Find(&find);

		if (other == NULL) break;
		if (other == unit || other->targetAttack != target) continue;
		if (!House_AreAllied(Unit_GetHouseID(other), Unit_GetHouseID(unit))) continue;
		ui = &g_table_unitInfo[other->o.type];
		if (!ui->flags.isNormalUnit || ui->damage == 0) continue;

		damage += ui->damage * 90 / max(ui->fireDelay, 15);
	}

	return damage;
}

/* Worth of one target to this unit, on the scale the whole selection uses.
 * Kept separate so a unit already in a fight can weigh what it is shooting at
 * against what it could be shooting at instead, with the same yardstick. */
/* A rough approach time for ranking targets, used when no firing tile can be
 * confirmed.  The point is that it is still a number: an enemy the unit cannot
 * reserve a slot against - because its neighbours are already standing on all
 * of them - is a target it should queue up behind, not one it cannot see. */
static uint32 Unit_Autonomy_EstimateApproachTicks(const Unit *unit, uint16 target)
{
	const UnitInfo *ui = &g_table_unitInfo[unit->o.type];
	uint16 type = Map_GetLandscapeType(Tile_PackTile(unit->o.position));
	uint16 speed;
	uint16 distance;

	if (type == LST_STRUCTURE) type = LST_CONCRETE_SLAB;
	speed = g_table_landscapeInfo[type].movementSpeed[ui->movementType];
	speed = ui->movingSpeedFactor * speed / 256;
	if (speed == 0) speed = 1;

	distance = Tile_GetDistanceRoundedUp(unit->o.position, Tools_Index_GetTile(target));
	distance = distance > ui->fireDistance ? distance - ui->fireDistance : 0;

	return (uint32)distance * 256 * 3 / speed;
}

static uint32 Unit_Autonomy_ScoreTarget(Unit *unit, uint16 target)
{
	uint16 maxHitpoints;
	uint16 hitpoints;
	uint32 priority;
	uint32 eta;
	uint32 score;
	uint32 coverage;
	uint32 focus;

	priority = Unit_Autonomy_BasePriority(unit, target, &maxHitpoints, &hitpoints);
	if (priority == 0) return 0;

	/* Whether a firing position happens to be free right now decides where the
	 * unit goes, never whether the enemy exists.  Tying the two together made
	 * every defender behind the front line blind: the ring around the target was
	 * taken by its own neighbours, so the attacker scored zero and was skipped.
	 * A sandworm stays the one exception - it may only be engaged from rock, so
	 * no rock position genuinely means no target. */
	if (!Unit_AttackPosition_EstimateTravel(unit, target, &eta)) {
		if (Unit_IsSandwormTarget(target)) return 0;
		eta = Unit_Autonomy_EstimateApproachTicks(unit, target);
	}

	/* ETA is the pathfinder-derived time to an actual firing tile, rather than
	 * a straight-line distance. */
	score = priority * 1024 / (eta / 15 + 1);
	score = score * (256 + ((maxHitpoints - min(hitpoints, maxHitpoints)) * 179 / max(maxHitpoints, 1))) / 256;

	coverage = Unit_Autonomy_IncomingDamage(unit, target) * 256 / max(hitpoints, 1);
	if (coverage < 256) {
		focus = 256 + min(154, coverage * 3 / 5);
	} else {
		focus = 410 - min(180, (coverage - 256) * 3 / 5);
	}
	score = score * focus / 256;
	if (s_houseThreatUntil[g_playerHouseID] > g_timerGame && target == s_houseThreatTarget[g_playerHouseID]) score *= 2;

	return score;
}

static uint16 Unit_Autonomy_FindTarget(Unit *unit, uint32 *bestScoreOut)
{
	uint16 candidates[8];
	uint32 roughScores[8];
	uint16 count = 0;
	uint16 i;
	uint16 best = 0;
	uint32 bestScore = 0;
	PoolFindStruct find;

	find.houseID = HOUSE_INVALID;
	find.index = 0xFFFF;
	find.type = 0xFFFF;
	while (true) {
		Unit *target = Unit_Find(&find);
		uint16 encoded;
		uint16 maxHitpoints;
		uint16 hitpoints;
		uint32 priority;
		uint16 distance;

		if (target == NULL) break;
		encoded = Tools_Index_Encode(target->o.index, IT_UNIT);
		if (!Unit_Autonomy_TargetInArea(unit, encoded)) continue;
		priority = Unit_Autonomy_BasePriority(unit, encoded, &maxHitpoints, &hitpoints);
		if (priority == 0) continue;
		distance = Tile_GetDistanceRoundedUp(unit->o.position, target->o.position);
		Unit_Autonomy_AddCandidate(candidates, roughScores, &count, encoded, priority * 256 / max(distance, 1));
	}

	find.houseID = HOUSE_INVALID;
	find.index = 0xFFFF;
	find.type = 0xFFFF;
	while (true) {
		Structure *target = Structure_Find(&find);
		uint16 encoded;
		uint16 maxHitpoints;
		uint16 hitpoints;
		uint32 priority;
		uint16 distance;

		if (target == NULL) break;
		if (target->o.type == STRUCTURE_SLAB_1x1 || target->o.type == STRUCTURE_SLAB_2x2 || target->o.type == STRUCTURE_WALL) continue;
		encoded = Tools_Index_Encode(target->o.index, IT_STRUCTURE);
		if (!Unit_Autonomy_TargetInArea(unit, encoded)) continue;
		priority = Unit_Autonomy_BasePriority(unit, encoded, &maxHitpoints, &hitpoints);
		if (priority == 0) continue;
		distance = Tile_GetDistanceRoundedUp(unit->o.position, target->o.position);
		Unit_Autonomy_AddCandidate(candidates, roughScores, &count, encoded, priority * 256 / max(distance, 1));
	}

	for (i = 0; i < count; i++) {
		uint32 score = Unit_Autonomy_ScoreTarget(unit, candidates[i]);

		if (score > bestScore) {
			bestScore = score;
			best = candidates[i];
		}
	}

	if (bestScoreOut != NULL) *bestScoreOut = bestScore;
	return best;
}

/* Break off a return and take up a fight again, without losing the post the
 * unit was on its way back to.  Going through Unit_Autonomy_BeginAttack() here
 * would record Move as the action to return to afterwards. */
static void Unit_Autonomy_ResumeFromReturn(Unit *unit, uint16 target)
{
	AutonomousPost *post = &s_autonomousPost[unit->o.index];
	ActionType returnAction = post->action;
	uint16 anchor = post->anchor;

	if (!Tools_Index_IsValid(target)) return;

	Unit_AttackPosition_SetManual(unit, true);
	post->anchor = anchor;
	post->action = returnAction;
	post->state = AUTONOMOUS_POST_ENGAGING;

	unit->targetMove = 0;
	unit->route[0] = 0xFF;
	Unit_SetAction(unit, ACTION_ATTACK);
	Unit_SetTarget(unit, target);
}

/* Consume an autonomous combat sortie and restore the unit's original post.
 * This is also called from the unit script completion path, which runs before
 * the periodic tactical update and therefore cannot lose the return order. */
bool Unit_Autonomy_ReturnToPost(Unit *unit)
{
	AutonomousPost *post;
	ActionType returnAction;
	uint16 anchor;

	if (unit == NULL) return false;
	post = &s_autonomousPost[unit->o.index];
	returnAction = post->action;
	anchor = post->anchor;
	if (post->state != AUTONOMOUS_POST_ENGAGING || returnAction == ACTION_INVALID) return false;

	/* A live target inside the guarded area is not something to walk away from.
	 * Without this the move script's completion hook sends the unit home the
	 * instant a return has been cancelled in favour of a fight, and the two
	 * decisions trade the unit back and forth on the spot.  A sandworm is the
	 * exception: breaking off is the whole point there. */
	if (Tools_Index_IsValid(unit->targetAttack) && !Unit_IsSandwormTarget(unit->targetAttack) &&
		Unit_Autonomy_TargetInArea(unit, unit->targetAttack)) return false;
	if (!Map_IsValidPosition(anchor)) anchor = unit->guardPosition;
	Unit_AttackPosition_SetManual(unit, false);
	unit->targetAttack = 0;
	unit->targetMove = 0;
	unit->route[0] = 0xFF;
	if (s_manualHunt[unit->o.index]) {
		Unit_SetAction(unit, ACTION_AREA_GUARD);
		return true;
	}

	if (returnAction == ACTION_HUNT || !Map_IsValidPosition(anchor) || Tile_GetDistance(unit->o.position, Tile_UnpackTile(anchor)) <= 128) {
		Unit_SetAction(unit, returnAction);
		return true;
	}

	Unit_SetAction(unit, ACTION_MOVE);
	Unit_SetDestination(unit, Tools_Index_Encode(anchor, IT_TILE));
	/* Do not use nextActionID here: it is consumed before the movement script
	 * creates currentDestination, which used to cancel this return immediately.
	 * The completion hook restores the original guard mode after arrival. */
	post->anchor = anchor;
	post->action = returnAction;
	post->state = AUTONOMOUS_POST_RETURNING;
	return true;
}

/* Advance: a march that keeps its initiative.  Mechanically it is the same
 * state a unit is in on its way home from a sortie - travelling to an anchor,
 * free to break off for anything worth fighting inside the zone around that
 * anchor, and returning to the march afterwards.  The anchor is the tile the
 * player pointed at rather than the one the unit came from, and the mode it
 * settles into on arrival is the tight Guard: an advance ends as a held
 * position, not as a wide patrol. */
static void Unit_Autonomy_BeginAdvance(Unit *unit, uint16 anchor)
{
	AutonomousPost *post;

	if (unit == NULL || unit->o.index >= UNIT_INDEX_MAX || !Map_IsValidPosition(anchor)) return;

	post = &s_autonomousPost[unit->o.index];
	post->anchor = anchor;
	post->action = ACTION_GUARD;
	post->state = AUTONOMOUS_POST_RETURNING;
}

static void Unit_Autonomy_BeginAttack(Unit *unit, uint16 target)
{
	if (!Tools_Index_IsValid(target)) return;
	/* Retargeting during the same defensive sortie keeps the original post;
	 * it must not turn a combat position into the unit's new home. */
	if (s_autonomousPost[unit->o.index].state != AUTONOMOUS_POST_ENGAGING) {
		Unit_AttackPosition_SetAutomatic(unit, unit->actionID);
	}
	Unit_SetAction(unit, ACTION_ATTACK);
	Unit_SetTarget(unit, target);
}

/* True when the unit is aimed at a sandworm it may not approach: out of range,
 * and no rock tile in weapon reach to take it under fire from.  Standing on
 * sand is not in itself a reason to move - a worm already within range is shot
 * at from where the unit stands, exactly as the guard scripts would. */
static bool Unit_Autonomy_SandwormOutOfReach(Unit *unit)
{
	const Unit *target = Tools_Index_GetUnit(unit->targetAttack);

	if (target == NULL || target->o.type != UNIT_SANDWORM) return false;
	if (s_manualHunt[unit->o.index] || unit->actionID == ACTION_HUNT) return false;

	return !Unit_AttackPosition_EstimateTravel(unit, unit->targetAttack, NULL);
}

static void Unit_Autonomy_Update(Unit *unit)
{
	uint16 target;

	if (!Unit_Autonomy_IsCombatUnit(unit)) return;

	/* Guard and Area Guard never walk into the sand after a worm.  This runs
	 * ahead of the sortie bookkeeping below because the chase can equally be
	 * started by the original script, which writes the worm straight into
	 * targetMove for every unit without a turret. */
	/* A worm close enough to shoot at is engaged from where the unit stands, but
	 * it still may not be walked into: drop a destination left pointing at it. */
	if (Unit_IsSandwormTarget(unit->targetMove)) {
		unit->targetMove = 0;
		unit->route[0] = 0xFF;
	}

	if (Unit_Autonomy_SandwormOutOfReach(unit)) {
		Unit_AttackPosition_Clear(unit);
		unit->targetMove = 0;
		unit->route[0] = 0xFF;
		Unit_Autonomy_ReturnToPost(unit);
		return;
	}

	/* A sortie whose target is gone, or has left the guarded area, is over.  The
	 * area check is what keeps a defender from being led away: the original
	 * scripts follow a target for as long as it lives, however far it runs. */
	if (s_autonomousPost[unit->o.index].state == AUTONOMOUS_POST_ENGAGING &&
		(!Tools_Index_IsValid(unit->targetAttack) || !Unit_Autonomy_TargetInArea(unit, unit->targetAttack))) {
		Unit_Autonomy_ReturnToPost(unit);
		return;
	}

	if (Unit_Autonomy_GetSearchRadius(unit) == 0) return;

	if (s_autonomyNextCheck[unit->o.index] == 0) {
		s_autonomyNextCheck[unit->o.index] = g_timerGame + (unit->o.index % 8) * 20;
		return;
	}
	if (s_autonomyNextCheck[unit->o.index] > g_timerGame) return;
	s_autonomyNextCheck[unit->o.index] = g_timerGame + 120;

	/* Fighting: keep looking around.  A unit on its way to a firing position is
	 * not committed to the target that sent it there - a worthwhile enough one
	 * that turns up meanwhile takes over.  The quarter-margin is what stops two
	 * comparable targets from trading the unit back and forth every check. */
	if (s_autonomousPost[unit->o.index].state == AUTONOMOUS_POST_ENGAGING) {
		uint32 bestScore = 0;

		target = Unit_Autonomy_FindTarget(unit, &bestScore);
		if (target != 0 && target != unit->targetAttack) {
			uint32 current = Unit_Autonomy_ScoreTarget(unit, unit->targetAttack);

			if (bestScore > current + current / 4) Unit_Autonomy_BeginAttack(unit, target);
		}
		return;
	}

	/* Returning: the way home is not a commitment either.  Anything worth
	 * fighting that appears inside the guarded area calls the unit straight back
	 * into the fight, with the same post kept for afterwards. */
	if (s_autonomousPost[unit->o.index].state == AUTONOMOUS_POST_RETURNING) {
		target = Unit_Autonomy_FindTarget(unit, NULL);
		if (target != 0) Unit_Autonomy_ResumeFromReturn(unit, target);
		return;
	}

	if (Tools_Index_IsValid(unit->targetAttack)) {
		Unit_Autonomy_BeginAttack(unit, unit->targetAttack);
		return;
	}

	target = Unit_Autonomy_FindTarget(unit, NULL);
	if (target != 0) Unit_Autonomy_BeginAttack(unit, target);
}

void Unit_Autonomy_ReportThreat(uint8 houseID, uint16 attacker, uint16 packed)
{
	if (houseID != g_playerHouseID || !Tools_Index_IsValid(attacker) || !Map_IsValidPosition(packed)) return;

	s_houseThreatTarget[houseID] = attacker;
	s_houseThreatUntil[houseID] = g_timerGame + 180;
}

/* ---------------------------------------------------------------------------
 * Harvester logic
 *
 * The original UNIT.EMC script owns the harvester.  Its cycle is:
 *
 *   ACTION_HARVEST   drive to targetMove, then call Script_Unit_Harvest() until
 *                    amount reaches 100.
 *   full             call Script_Unit_CallUnitByType(CARRYALL).  When a
 *                    transport is returned the script *parks and waits* for the
 *                    pickup; otherwise it reads Script_Unit_GetInfo(0x06)
 *                    (originEncoded, refreshed through Unit_FindClosestRefinery)
 *                    and drives home as ACTION_MOVE with targetMove set to the
 *                    refinery structure.
 *   at the refinery  Unit_EnterStructure() hides the unit; the structure script
 *                    unloads it and releases it again through
 *                    Script_Structure_Unknown0C5A().
 *
 * Everything below only supplements that script, so it has to respect who owns
 * which field.  Violating this is what produced the "full harvester parks and
 * never unloads" failures:
 *
 *   targetMove             The active order.  A structure index means "returning
 *                          to the refinery", a tile means "going to spice".
 *   o.script.variables[4]  A *mutual* reservation, only ever manipulated through
 *                          Object_Script_Variable4_{Link,Set,Clear}.
 *                          harvester <-> carryall = "pickup booked, stand still";
 *                          harvester <-> refinery = "entrance reserved";
 *                          refinery  <-> carryall = "transport inbound".
 *                          A non-zero value on a harvester means it deliberately
 *                          does nothing, so it may only be set when the other
 *                          side really can finish the job.
 *   originEncoded          Long-term home refinery.  A preference recomputed on
 *                          demand by the script, never an instruction.
 *   harvestCenter          Player-selected working area (ours).
 *   harvestHoldPosition    Explicit player Move: stay put (ours).
 *   airTransitDestination  One-shot airlift request (ours).  It is consumed on
 *                          arrival, on filling up and on any new order; a sticky
 *                          value made every later trip hijack a carryall.
 * ------------------------------------------------------------------------- */

static void Unit_Harvester_AddCandidate(uint16 *candidates, uint32 *scores, uint16 *count, uint16 target, uint32 score)
{
	uint16 i;
	uint16 insert = *count;

	for (i = 0; i < *count; i++) {
		if (score < scores[i]) {
			insert = i;
			break;
		}
	}
	if (insert >= 24) return;
	if (*count < 24) (*count)++;
	for (i = *count - 1; i > insert; i--) {
		candidates[i] = candidates[i - 1];
		scores[i] = scores[i - 1];
	}
	candidates[insert] = target;
	scores[insert] = score;
}

static bool Unit_Harvester_FindSpice(Unit *unit, uint16 center, uint16 radius, uint16 *result)
{
	uint16 candidates[24];
	uint32 roughScores[24];
	uint16 count = 0;
	uint16 x;
	uint16 y;
	uint16 i;
	uint16 best = 0;
	uint32 bestScore = 0xFFFFFFFF;

	for (y = 0; y < 64; y++) {
		for (x = 0; x < 64; x++) {
			uint16 packed = Tile_PackXY(x, y);
			uint16 type;
			uint16 distance;
			uint32 score;

			if (!Map_IsValidPosition(packed) || !Map_IsPositionUnveiled(packed)) continue;
			if (radius != 0 && Tile_GetDistancePacked(center, packed) > radius) continue;
			type = Map_GetLandscapeType(packed);
			if (type != LST_SPICE && type != LST_THICK_SPICE) continue;
			if (Object_GetByPackedTile(packed) != NULL || Unit_GetTileEnterScore(unit, packed, 0) > 255) continue;
			distance = Tile_GetDistancePacked(Tile_PackTile(unit->o.position), packed);
			score = distance * 16 + (type == LST_THICK_SPICE ? 0 : 8);
			Unit_Harvester_AddCandidate(candidates, roughScores, &count, packed, score);
		}
	}

	for (i = 0; i < count; i++) {
		uint32 ticks;
		uint16 type;
		uint32 score;

		if (!Script_Unit_HasRoute(unit, Tile_PackTile(unit->o.position), candidates[i], &ticks)) continue;
		type = Map_GetLandscapeType(candidates[i]);
		score = ticks * 16 + (type == LST_THICK_SPICE ? 0 : 80);
		if (score < bestScore) {
			bestScore = score;
			best = candidates[i];
		}
	}

	if (best == 0) return false;
	*result = best;
	return true;
}

/* Prefer the user-selected harvesting area, but never leave a player harvester
 * idle when another explored spice field is reachable.
 *
 * The search sweeps the whole map and runs the pathfinder over its best
 * candidates, and it is also reached from script context (the carryall pickup
 * and the refinery release).  The result is therefore memoised per unit for a
 * short while; the memo is dropped as soon as the working area changes. */
uint16 Unit_Harvester_FindPreferredSpice(Unit *unit)
{
	HarvesterTracker *tracker;
	uint16 packed;
	uint16 target;

	if (unit == NULL || unit->o.type != UNIT_HARVESTER || unit->o.index >= UNIT_INDEX_MAX) return 0;
	tracker = &s_harvester[unit->o.index];
	if (tracker->spiceCacheUntil > g_timerGame && tracker->spiceCacheCenter == unit->harvestCenter) return tracker->spiceCache;

	packed = Tile_PackTile(unit->o.position);
	if (!Map_IsValidPosition(unit->harvestCenter) || !Unit_Harvester_FindSpice(unit, unit->harvestCenter, 12, &target)) {
		if (!Unit_Harvester_FindSpice(unit, packed, 0, &target)) target = 0;
	}

	tracker->spiceCache = target;
	tracker->spiceCacheCenter = unit->harvestCenter;
	tracker->spiceCacheUntil = g_timerGame + 30;

	return target;
}

/* A refinery can become occupied after a harvester has already routed to its
 * entrance.  It must satisfy precisely the same acceptance conditions as the
 * Carryall pickup script; a clear linkedID alone is not sufficient.
 *
 * A door reservation held by the asking harvester itself still counts as
 * available.  Ignoring that made a queued harvester classify its own booking as
 * "occupied" and start hunting for transport it did not need. */
/* Which harvester is already driving to which refinery.
 *
 * The engine has no such thing, and that is the whole bug: three harvesters that
 * fill up together each ask for the nearest refinery that will accept them, all
 * three get the same answer -- nobody is inside it yet -- and all three drive to
 * the same door while the others stand empty.  Only after arriving does one win
 * and the rest re-route, having crossed the map for nothing.
 *
 * This is ours and not the script's.  The engine's own reservation is
 * o.script.variables[4], and writing that on a harvester means "stand still and
 * wait for a carryall", which would park the fleet -- see the ownership note
 * above.  A claim here only makes a refinery invisible to *other* harvesters
 * while one is on its way to it.
 *
 * The claim carries an expiry so nothing can be blocked forever by a harvester
 * that died, changed its mind, or got picked up on the way. */
#define HARVESTER_CLAIM_TICKS 900

static uint16 s_refineryClaim[STRUCTURE_INDEX_MAX_SOFT];      /*!< Unit index + 1, or 0. */
static uint32 s_refineryClaimUntil[STRUCTURE_INDEX_MAX_SOFT];

void Unit_Harvester_ReleaseClaim(const Unit *unit)
{
	uint16 i;

	if (unit == NULL) return;

	for (i = 0; i < STRUCTURE_INDEX_MAX_SOFT; i++) {
		if (s_refineryClaim[i] == unit->o.index + 1) s_refineryClaim[i] = 0;
	}
}

static bool Unit_Harvester_ClaimedByOther(const Structure *refinery, const Unit *unit)
{
	uint16 index;
	const Unit *holder;

	if (refinery->o.index >= STRUCTURE_INDEX_MAX_SOFT) return false;

	index = s_refineryClaim[refinery->o.index];
	if (index == 0) return false;
	if (unit != NULL && index == unit->o.index + 1) return false;
	if (s_refineryClaimUntil[refinery->o.index] <= g_timerGame) return false;

	/* A claim outlives nothing.  If the holder is gone, loaded no longer, or
	 * already inside a refinery, the door is free again. */
	holder = Unit_Get_ByIndex(index - 1);
	if (holder == NULL || !holder->o.flags.s.used || holder->o.type != UNIT_HARVESTER
		|| holder->o.flags.s.isNotOnMap || holder->amount == 0) {
		s_refineryClaim[refinery->o.index] = 0;
		return false;
	}

	return true;
}

/* Whether this harvester is the one holding this refinery. */
static bool Unit_Harvester_HoldsClaim(const Structure *refinery, const Unit *unit)
{
	if (refinery == NULL || unit == NULL || refinery->o.index >= STRUCTURE_INDEX_MAX_SOFT) return false;

	return (s_refineryClaim[refinery->o.index] == unit->o.index + 1
		&& s_refineryClaimUntil[refinery->o.index] > g_timerGame);
}

/* Book this refinery for this harvester, so the next one to ask looks elsewhere. */
static void Unit_Harvester_Claim(const Structure *refinery, const Unit *unit)
{
	if (refinery == NULL || unit == NULL || refinery->o.index >= STRUCTURE_INDEX_MAX_SOFT) return;

	Unit_Harvester_ReleaseClaim(unit);

	s_refineryClaim[refinery->o.index]      = unit->o.index + 1;
	s_refineryClaimUntil[refinery->o.index] = g_timerGame + HARVESTER_CLAIM_TICKS;
}

static bool Unit_Harvester_RefineryAccepts(const Structure *refinery, const Unit *unit)
{
	if (refinery == NULL
		|| refinery->o.type != STRUCTURE_REFINERY
		|| refinery->o.hitpoints == 0
		|| refinery->state != STRUCTURE_STATE_IDLE
		|| refinery->o.linkedID != 0xFF) return false;

	if (Unit_Harvester_ClaimedByOther(refinery, unit)) return false;

	if (refinery->o.script.variables[4] == 0) return true;
	return unit != NULL && refinery->o.script.variables[4] == Tools_Index_Encode(unit->o.index, IT_UNIT);
}

/**
 * Whether a harvester is queueing: sitting on the map with a full load, aimed at
 * a refinery that will not take it.
 *
 * This is the "WAIT REF" of the harvester trace, and it is the only harvester
 * state that means the house is short of refinery capacity.  A harvester driving
 * home with a load is not queueing, however long the drive -- and telling the
 * two apart is the whole point: counting the commute as a queue let a base with
 * one harvester and one refinery report a queue at itself.
 */
bool Unit_Harvester_IsQueued(const Unit *unit)
{
	const Structure *refinery;

	if (unit == NULL || unit->o.type != UNIT_HARVESTER) return false;
	if (unit->o.flags.s.isNotOnMap) return false;    /* Already inside one. */
	if (unit->amount < 100) return false;            /* Not full: still has work to do. */

	/* Same two places the trace looks: where it is going, or failing that the
	 * refinery it belongs to. */
	refinery = Tools_Index_GetStructure(unit->targetMove);
	if (refinery == NULL && Tools_Index_GetType(unit->originEncoded) == IT_STRUCTURE) {
		refinery = Tools_Index_GetStructure(unit->originEncoded);
	}

	if (refinery == NULL || refinery->o.type != STRUCTURE_REFINERY) return false;

	return !Unit_Harvester_RefineryAccepts(refinery, unit);
}

static Structure *Unit_Harvester_FindAvailableRefinery(Unit *unit, const Structure *exclude)
{
	PoolFindStruct find;
	Structure *best = NULL;
	uint16 bestDistance = 0;

	find.type = STRUCTURE_REFINERY;
	find.houseID = Unit_GetHouseID(unit);
	find.index = 0xFFFF;
	while (true) {
		Structure *candidate = Structure_Find(&find);
		uint16 distance;

		if (candidate == NULL) break;
		if (candidate == exclude || !Unit_Harvester_RefineryAccepts(candidate, unit)) continue;
		distance = Tile_GetDistance(unit->o.position, candidate->o.position);
		if (best != NULL && distance >= bestDistance) continue;
		best = candidate;
		bestDistance = distance;
	}

	Unit_Harvester_Claim(best, unit);

	return best;
}

static bool Unit_Harvester_HasCarryallReservation(const Unit *unit)
{
	PoolFindStruct find;
	Unit *transport;
	uint16 encoded;

	if (unit == NULL) return false;
	encoded = Tools_Index_Encode(unit->o.index, IT_UNIT);

	/* A harvester may hold the old-style backlink, but normal Carryall requests
	 * store the reservation on the transport itself.  Recognise both forms. */
	if (unit->o.script.variables[4] != 0) {
		transport = Tools_Index_GetUnit(unit->o.script.variables[4]);
		if (transport != NULL && transport->o.type == UNIT_CARRYALL) return true;
	}

	find.type = UNIT_CARRYALL;
	find.houseID = unit->deviated != 0 ? (g_dune2_enhanced ? unit->deviatedHouse : HOUSE_ORDOS) : unit->o.houseID;
	find.index = 0xFFFF;
	while ((transport = Unit_Find(&find)) != NULL) {
		if (transport->targetMove == encoded || transport->o.script.variables[4] == encoded) return true;
	}

	return false;
}

static void Unit_Harvester_CancelPickup(Unit *unit)
{
	PoolFindStruct find;
	Unit *transport;
	uint16 encoded;

	if (unit == NULL) return;
	encoded = Tools_Index_Encode(unit->o.index, IT_UNIT);
	find.type = UNIT_CARRYALL;
	find.houseID = Unit_GetHouseID(unit);
	find.index = 0xFFFF;
	while ((transport = Unit_Find(&find)) != NULL) {
		if (transport->targetMove != encoded && transport->o.script.variables[4] != encoded) continue;
		Object_Script_Variable4_Clear(&transport->o);
		transport->targetMove = 0;
	}
}

/* A new player order replaces every harvester intent the previous one left
 * behind.  A stale airlift booking in particular used to survive across orders
 * and hijack the unit's next trip.  Pass ACTION_INVALID for orders that are not
 * one of the four harvester commands. */
void Unit_Harvester_BeginOrder(Unit *unit, ActionType action)
{
	HarvesterTracker *tracker;

	if (unit == NULL || unit->o.type != UNIT_HARVESTER || unit->o.index >= UNIT_INDEX_MAX) return;

	unit->harvestHoldPosition = (action == ACTION_MOVE) ? 1 : 0;
	unit->airTransitDestination = 0;

	tracker = &s_harvester[unit->o.index];
	tracker->forcedReturn = (action == ACTION_RETURN);
	tracker->nextCheck = 0;
	tracker->refineryTarget = 0;
	tracker->refineryStalledSince = 0;
	tracker->refineryRetarget = 0;
}

/* Abandon the computed route, but never while the unit is between two tiles.
 *
 * currentDestination is the tile the current step is aimed at.  Clearing it
 * mid-step makes Unit_Move() measure progress against tile (0,0) instead: the
 * arrival test fires on the very next tick and Unit_SetSpeed(unit, 0) strands
 * the harvester at a half-tile offset, typically right in the refinery doorway.
 * Letting the step finish costs one tile - the arrival code clears
 * currentDestination itself, and Script_Unit_CalculateRoute() refuses to
 * re-path before that in any case. */
static void Unit_Harvester_StopRoute(Unit *unit)
{
	unit->route[0] = 0xFF;
	if (unit->speed != 0) return;

	unit->currentDestination.x = 0;
	unit->currentDestination.y = 0;
}

/** Drop any order state that keeps a harvester waiting instead of driving. */
static void Unit_Harvester_ClearOrder(Unit *unit)
{
	Unit_Harvester_CancelPickup(unit);
	Object_Script_Variable4_Clear(&unit->o);
	unit->targetMove = 0;
	Unit_Harvester_StopRoute(unit);
}

/** Send the harvester to a spice tile as a fresh Harvest order. */
static void Unit_Harvester_GoToSpice(Unit *unit, uint16 spice)
{
	unit->harvestCenter = spice;
	Unit_SetAction(unit, ACTION_HARVEST);
	Unit_SetDestination(unit, Tools_Index_Encode(spice, IT_TILE));
	s_harvester[unit->o.index].nextCheck = g_timerGame + 90;
}

/* The airlift booked by a Harvest order is a single delivery to the chosen
 * field, not a standing preference.  Keeping it set made Unit_AirTransit_Update
 * reserve a carryall on every later trip: a full harvester then parked as
 * "awaiting pickup" and Script_Unit_Pickup flew it back onto the spice it had
 * just emptied instead of to the refinery. */
static void Unit_Harvester_UpdateAirlift(Unit *unit)
{
	HarvesterTracker *tracker = &s_harvester[unit->o.index];
	uint16 destination = unit->airTransitDestination;

	if (destination == 0) {
		tracker->airliftDeadline = 0;
		return;
	}

	/* The request is spent once it is meaningless: the unit arrived by itself,
	 * it filled up on the way, or the player replaced the order with a Move. */
	if (!Map_IsValidPosition(destination) || unit->amount >= 100 || unit->harvestHoldPosition != 0
		|| Tile_GetDistancePacked(Tile_PackTile(unit->o.position), destination) <= 2) {
		unit->airTransitDestination = 0;
		tracker->airliftDeadline = 0;
		return;
	}

	if (tracker->airliftDeadline == 0) tracker->airliftDeadline = g_timerGame + 600;
	if (Unit_Harvester_HasCarryallReservation(unit)) return;

	/* Waiting only pays while transport can realistically arrive.  With no
	 * carryall at all, or once the wait has run long, take the ground route the
	 * order would originally have used: an idle harvester earns nothing. */
	if (Unit_IsTypeOnMap(Unit_GetHouseID(unit), UNIT_CARRYALL) && tracker->airliftDeadline > g_timerGame) return;

	unit->airTransitDestination = 0;
	tracker->airliftDeadline = 0;
	Unit_Harvester_GoToSpice(unit, destination);
}

/* Mirrors the filter in Unit_CallUnitByType(): a carryall with no cargo and no
 * current assignment.  Checking before booking lets the caller decide whether
 * it is worth giving up a refinery-door reservation for the flight. */
static bool Unit_Harvester_HasFreeCarryall(uint8 houseID)
{
	PoolFindStruct find;
	Unit *transport;

	find.type = UNIT_CARRYALL;
	find.houseID = houseID;
	find.index = 0xFFFF;
	while ((transport = Unit_Find(&find)) != NULL) {
		if (transport->o.linkedID != 0xFF || transport->targetMove != 0) continue;
		return true;
	}

	return false;
}

/* Below this many tiles a flight saves nothing: the carryall spends longer
 * approaching and unloading than the harvester needs to drive. */
#define HARVESTER_AIRLIFT_MIN_DISTANCE 7

/* UNIT.EMC asks for transport exactly once, at the moment the return trip
 * starts (word 932).  If every carryall happened to be busy in that single
 * instant the script fell through to GoToClosestStructure() and walked the
 * entire way, even when a transport became free a second later.  Retry while
 * the remaining trip is still long enough to be worth a flight. */
static void Unit_Harvester_RetryLift(Unit *unit)
{
	HarvesterTracker *tracker = &s_harvester[unit->o.index];
	uint16 destination;

	if (unit->harvestHoldPosition != 0 || unit->airTransitDestination != 0) return;
	if (unit->o.script.variables[4] != 0 && Unit_Harvester_HasCarryallReservation(unit)) return;
	if (tracker->liftCheck > g_timerGame) return;
	tracker->liftCheck = g_timerGame + 60;

	destination = Tools_Index_GetPackedTile(unit->targetMove);
	if (!Map_IsValidPosition(destination)) return;
	if (Tile_GetDistancePacked(Tile_PackTile(unit->o.position), destination) <= HARVESTER_AIRLIFT_MIN_DISTANCE) return;
	if (!Unit_Harvester_HasFreeCarryall(Unit_GetHouseID(unit))) return;

	if (unit->amount < 100) {
		/* Outbound: the destination is a spice tile, which is precisely what the
		 * one-shot airlift request already expresses.  Unit_AirTransit_Update()
		 * books the transport on this same pass. */
		if (Tools_Index_GetType(unit->targetMove) != IT_TILE) return;
		unit->airTransitDestination = destination;
		tracker->airliftDeadline = 0;
		return;
	}

	{
		/* Homebound: Script_Unit_Pickup() picks the refinery itself, but only
		 * considers one with a clear reservation.  Our own door booking would
		 * hide the very refinery we are heading for, so release it - exactly what
		 * the original script does at word 906 when it gives up on transport. */
		Structure *refinery = Unit_Harvester_FindAvailableRefinery(unit, NULL);

		if (refinery == NULL) return;
		if (refinery->o.script.variables[4] != 0) Object_Script_Variable4_Clear(&unit->o);
		Unit_CallUnitByType(UNIT_CARRYALL, Unit_GetHouseID(unit), Tools_Index_Encode(unit->o.index, IT_UNIT), false);
	}
	/* The harvester deliberately keeps driving.  The transport intercepts it on
	 * the way, so a booking that falls through costs nothing. */
}

/* The original script may start an unload trip before the cargo is full.  For
 * player harvesters, a reachable spice tile takes priority until 100%; only a
 * truly exhausted working area is allowed to trigger an early unload. */
static bool Unit_Harvester_ContinueUntilFull(Unit *unit)
{
	Structure *refinery;
	uint16 spice;

	if (unit->amount >= 100) return false;
	/* An explicit Return order and an explicit Move order both outrank the
	 * automatic top-up: the player asked for something specific. */
	if (s_harvester[unit->o.index].forcedReturn || unit->harvestHoldPosition != 0) return false;
	refinery = Tools_Index_GetStructure(unit->targetMove);
	if (refinery == NULL || refinery->o.type != STRUCTURE_REFINERY) return false;

	spice = Unit_Harvester_FindPreferredSpice(unit);
	if (spice == 0) return false;

	Unit_Harvester_ClearOrder(unit);
	Unit_Harvester_GoToSpice(unit, spice);
	return true;
}

/* Ticks between two re-aims of the same return trip. */
#define HARVESTER_RETARGET_COOLDOWN 60

/* A refinery can become occupied after a harvester has already routed to its
 * entrance, and the original script then keeps pushing into the closed door
 * forever.  Watch a real return trip for lack of progress and re-route it.
 *
 * Only an active move-to-refinery order counts.  originEncoded looks like a
 * refinery for every harvester because Script_Unit_GetInfo(0x06) refreshes it
 * on demand, so treating it as an order made this run against a harvester that
 * was merely standing on spice, cancelling its harvest every three seconds. */
static void Unit_Harvester_RecoverRefinery(Unit *unit)
{
	HarvesterTracker *tracker = &s_harvester[unit->o.index];
	Structure *refinery;
	Structure *alternate;
	uint16 destination;
	uint16 packed;

	refinery = Tools_Index_GetStructure(unit->targetMove);
	if (refinery == NULL || refinery->o.type != STRUCTURE_REFINERY) {
		tracker->refineryTarget = 0;
		tracker->refineryStalledSince = 0;
		return;
	}

	destination = Tools_Index_Encode(refinery->o.index, IT_STRUCTURE);
	packed = Tile_PackTile(unit->o.position);

	/* Do not join a queue while another refinery stands empty.  The original
	 * script picks a refinery once and keeps driving to it whatever happens
	 * there, which with several refineries produces the one thing a fleet must
	 * not do: a line at the first door and idle doors behind it.  Switching only
	 * after the stall timer below meant three seconds of standing first. */
	if (!Unit_Harvester_RefineryAccepts(refinery, unit) && tracker->refineryRetarget <= g_timerGame) {
		alternate = Unit_Harvester_FindAvailableRefinery(unit, refinery);

		if (alternate != NULL) {
			/* Not more often than this.  The original script keeps aiming at the
			 * refinery it chose, so it re-aims the unit back on its own tick; a
			 * switch on every one of ours then tore up the route before the
			 * harvester had moved a single tile, and it stood in the doorway for
			 * the rest of the match with an idle refinery next to it.  The
			 * cooldown is still an order of magnitude faster than waiting for the
			 * stall timer below, which is what this replaced. */
			tracker->refineryRetarget = g_timerGame + HARVESTER_RETARGET_COOLDOWN;
			tracker->refineryTarget = Tools_Index_Encode(alternate->o.index, IT_STRUCTURE);
			tracker->lastPosition = packed;
			tracker->refineryStalledSince = g_timerGame;

			Unit_Harvester_ClearOrder(unit);
			Unit_SetDestination(unit, tracker->refineryTarget);
			return;
		}
	}

	if (tracker->refineryTarget != destination) {
		tracker->refineryTarget = destination;
		tracker->lastPosition = packed;
		tracker->refineryStalledSince = g_timerGame;
		return;
	}

	/* Any forward movement means the entrance queue is still working. */
	if (tracker->lastPosition != packed || tracker->refineryStalledSince == 0) {
		tracker->lastPosition = packed;
		tracker->refineryStalledSince = g_timerGame;
		return;
	}

	/* Give a normal entrance approach time to resolve traffic before doing
	 * anything, and never reissue an order into an unavailable door. */
	if (tracker->refineryStalledSince + 180 > g_timerGame) return;

	tracker->refineryStalledSince = g_timerGame;

	alternate = Unit_Harvester_RefineryAccepts(refinery, unit) ? refinery : Unit_Harvester_FindAvailableRefinery(unit, refinery);
	if (alternate != NULL && alternate != refinery) {
		Unit_Harvester_ClearOrder(unit);
		Unit_SetDestination(unit, Tools_Index_Encode(alternate->o.index, IT_STRUCTURE));
		return;
	}

	/* Either our own refinery is ready and something is merely standing in the
	 * way, or every refinery is busy and this is an ordinary queue.  Recompute
	 * the route and stay in line.
	 *
	 * This deliberately books no transport.  Script_Unit_Pickup() refuses to
	 * lift a harvester while no refinery can receive it and then keeps both the
	 * carryall and the harvester reserved, which is how a blocked entrance used
	 * to consume every transport in the house and park the harvesters for good.
	 * originEncoded is left alone as well; it belongs to the original script. */
	Unit_Harvester_StopRoute(unit);
}

/* Watchdog for a loaded harvester.  Whatever left it without an order - a
 * released carryall booking, a destroyed refinery, a save from an older build -
 * a full harvester must never simply stand on the sand.  This mirrors what the
 * original script does when Script_Unit_CallUnitByType() gives it no transport:
 * take the closest refinery and drive there as ACTION_MOVE. */
static void Unit_Harvester_RecoverFullCargo(Unit *unit)
{
	HarvesterTracker *tracker = &s_harvester[unit->o.index];
	Structure *refinery;

	if (unit->targetMove != 0 || unit->currentDestination.x != 0 || unit->currentDestination.y != 0) return;
	if (Unit_Harvester_HasCarryallReservation(unit)) return;
	if (tracker->nextCheck > g_timerGame) return;
	tracker->nextCheck = g_timerGame + 90;

	refinery = Unit_Harvester_FindAvailableRefinery(unit, NULL);
	if (refinery == NULL) {
		/* Everything is busy: queue at the nearest refinery that still stands. */
		Unit_FindClosestRefinery(unit);
		refinery = Tools_Index_GetStructure(unit->originEncoded);
		if (refinery == NULL || refinery->o.type != STRUCTURE_REFINERY) return;
	}

	Unit_SetAction(unit, ACTION_MOVE);
	Unit_SetDestination(unit, Tools_Index_Encode(refinery->o.index, IT_STRUCTURE));
}

/* Periodic maintenance for player harvesters, on top of the legacy script.
 * Everything here is a recovery path: it may only act when the script has left
 * the unit with nothing useful to do. */
static void Unit_Harvester_Update(Unit *unit)
{
	HarvesterTracker *tracker;
	uint16 packed;
	uint16 target;
	uint16 type;

	if (unit->o.type != UNIT_HARVESTER) return;
	/* This recovery layer was written for the player's harvesters: in the
	 * campaign nobody watches the AI closely enough to care that its
	 * harvesters stall.  A skirmish is nothing but AI, and they stall in
	 * exactly the same ways -- a route that never makes progress leaves the
	 * script sitting in ACTION_HARVEST forever, and spice collection stops
	 * for the rest of the match. */
	if (Unit_GetHouseID(unit) != g_playerHouseID && !Skirmish_IsActive()) return;
	if (unit->o.flags.s.isNotOnMap || unit->o.index >= UNIT_INDEX_MAX) return;
	tracker = &s_harvester[unit->o.index];

	/* An unloaded harvester has served its Return order. */
	if (unit->amount == 0) tracker->forcedReturn = false;

	Unit_Harvester_UpdateAirlift(unit);
	if (Unit_Harvester_ContinueUntilFull(unit)) return;
	Unit_Harvester_RecoverRefinery(unit);
	Unit_Harvester_RetryLift(unit);

	/* A full harvester's only job is to reach its refinery.  It still needs the
	 * recovery above, but it must not go looking for more spice. */
	if (unit->amount >= 100) {
		Unit_Harvester_RecoverFullCargo(unit);
		return;
	}
	/* Waiting for the airlift that a Harvest order booked. */
	if (unit->airTransitDestination != 0) return;

	/* Only an explicit Move-to-wait order may leave a partially empty player
	 * harvester parked.  Do not force Harvest before a reachable field exists:
	 * the original harvest script correctly changes itself back to Stop when it
	 * has no route, which otherwise made the two actions oscillate every tick. */
	if (unit->actionID != ACTION_HARVEST) {
		if (unit->harvestHoldPosition != 0 || tracker->nextCheck > g_timerGame) return;
		tracker->nextCheck = g_timerGame + 90;

		packed = Tile_PackTile(unit->o.position);
		type = Map_GetLandscapeType(packed);
		if (type == LST_SPICE || type == LST_THICK_SPICE) {
			Unit_SetAction(unit, ACTION_HARVEST);
			return;
		}

		target = Unit_Harvester_FindPreferredSpice(unit);
		if (target != 0) Unit_Harvester_GoToSpice(unit, target);
		return;
	}

	if (tracker->nextCheck > g_timerGame) return;
	tracker->nextCheck = g_timerGame + 90;

	packed = Tile_PackTile(unit->o.position);
	if (tracker->lastPosition != packed || tracker->lastProgress == 0) {
		tracker->lastPosition = packed;
		tracker->lastProgress = g_timerGame;
	}

	/* A valid route is left alone.  A route that has made no progress for six
	 * checks is abandoned so a fresh reachable spice field can be selected. */
	if (unit->targetMove != 0 || unit->currentDestination.x != 0 || unit->currentDestination.y != 0) {
		if (unit->o.script.variables[4] != 0 || tracker->lastProgress + 540 > g_timerGame) return;
		Unit_Harvester_ClearOrder(unit);
	}

	type = Map_GetLandscapeType(packed);
	if (type == LST_SPICE || type == LST_THICK_SPICE) return;

	target = Unit_Harvester_FindPreferredSpice(unit);
	if (target == 0) return;

	/* Keep the newly found field as the working area so the next refinery trip
	 * returns to useful spice instead of the harvester's stale last waypoint. */
	unit->harvestCenter = target;
	Unit_SetDestination(unit, Tools_Index_Encode(target, IT_TILE));
}

/** Is this a player unit that can take part in a box selection? */
static bool UnitSelection_IsControllable(const Unit *unit)
{
	const UnitInfo *ui;

	if (unit == NULL || !unit->o.flags.s.used || !unit->o.flags.s.allocated || unit->o.flags.s.isNotOnMap) return false;
	if ((unit->deviated != 0 ? (g_dune2_enhanced ? unit->deviatedHouse : HOUSE_ORDOS) : unit->o.houseID) != g_playerHouseID || unit->o.type == UNIT_CARRYALL) return false;

	ui = &g_table_unitInfo[unit->o.type];
	return ui->flags.isNormalUnit;
}

static bool UnitSelection_Contains(const Unit *unit)
{
	uint16 i;

	for (i = 0; i < g_unitSelectionCount; i++) {
		if (s_unitSelection[i] == unit->o.index) return true;
	}

	return false;
}

static void UnitSelection_Add(Unit *unit)
{
	if (!UnitSelection_IsControllable(unit) || UnitSelection_Contains(unit) || g_unitSelectionCount >= UNIT_SELECTION_MAX) return;

	s_unitSelection[g_unitSelectionCount++] = unit->o.index;
	Unit_UpdateMap(2, unit);
}

static void UnitSelection_ClearInternal(void)
{
	uint16 i;

	for (i = 0; i < g_unitSelectionCount; i++) {
		Unit *unit = Unit_Get_ByIndex(s_unitSelection[i]);
		if (unit->o.flags.s.used) Unit_UpdateMap(2, unit);
	}

	g_unitSelectionCount = 0;
}

static void UnitSelection_CaptureTargetSelection(void)
{
	uint16 i;

	s_unitTargetSelectionCount = 0;
	for (i = 0; i < g_unitSelectionCount; i++) {
		Unit *unit = Unit_Get_ByIndex(s_unitSelection[i]);
		if (!UnitSelection_IsControllable(unit)) continue;
		s_unitTargetSelection[s_unitTargetSelectionCount++] = unit->o.index;
	}
	s_unitTargetSelectionActive = s_unitTargetSelectionCount != 0;
}

/** Does a unit expose an action in its normal four-action command set? */
static bool UnitSelection_UnitHasAction(const Unit *unit, ActionType action)
{
	const uint16 *actions;
	uint16 i;

	if (!UnitSelection_IsControllable(unit)) return false;

	actions = g_table_unitInfo[unit->o.type].o.actionsPlayer;
	for (i = 0; i < 4; i++) {
		if (actions[i] == action) return true;
	}

	/* Shift variants share the capability of their normal command. */
	if (action == ACTION_AMBUSH) return UnitSelection_UnitHasAction(unit, ACTION_ATTACK);
	if (action == ACTION_AREA_GUARD) return UnitSelection_UnitHasAction(unit, ACTION_GUARD);

	return false;
}

static ActionType UnitSelection_GetUnitSpecialAction(const Unit *unit)
{
	static const ActionType specials[] = { ACTION_DEPLOY, ACTION_SABOTAGE, ACTION_DESTRUCT };
	uint16 i;

	for (i = 0; i < sizeof(specials) / sizeof(specials[0]); i++) {
		if (UnitSelection_UnitHasAction(unit, specials[i])) return specials[i];
	}

	return ACTION_INVALID;
}

/* An Attack order aimed at bare ground is an advance, not a shot at the dirt.
 * "Bare" means the clicked tile itself and nothing else: no structure, no unit,
 * and not a spice bloom - a bloom is something you shoot at on purpose.
 *
 * The classic targeting snaps to any unit in the ring around the click
 * (Unit_FindTargetAround), which is convenient when every click is meant as an
 * attack, and unacceptable now that a click on open ground has its own meaning:
 * ordering an advance past one's own line would grab a friendly unit a tile
 * away and shoot it.  Attacking is exact targeting; missing by a tile orders a
 * march, which costs nothing to correct. */
static bool UnitSelection_IsAdvanceTarget(uint16 packed)
{
	if (!Map_IsValidPosition(packed)) return false;
	if (Structure_Get_ByPackedTile(packed) != NULL) return false;
	if (Map_GetLandscapeType(packed) == LST_BLOOM_FIELD) return false;

	return Unit_Get_ByPackedTile(packed) == NULL;
}

/* Destination tiles handed out to one group order.  A group given a single
 * destination piles onto one tile and sorts itself out by shoving, which is
 * both ugly and slow; every recipient gets its own tile instead. */
static uint16 s_spreadTiles[UNIT_SELECTION_MAX];
static uint16 s_spreadCount = 0;

void UnitSelection_SpreadReset(void)
{
	s_spreadCount = 0;
}

/* Claim the free tile nearest to the target point, preferring the side the unit
 * is coming from so the group does not cross over itself.  Unit_GetTileEnterScore()
 * is the single admissibility test: it already rejects impassable ground and
 * tiles held by another unit, while leaving the caller's own tile usable - a
 * unit already standing in the target area simply keeps its place. */
uint16 UnitSelection_SpreadTake(Unit *unit, uint16 packed)
{
	uint16 unitPacked;
	uint16 radius;

	if (unit == NULL || !Map_IsValidPosition(packed)) return packed;
	if (s_spreadCount >= UNIT_SELECTION_MAX) return packed;

	unitPacked = Tile_PackTile(unit->o.position);

	for (radius = 0; radius <= 8; radius++) {
		uint16 best = 0;
		uint16 bestDistance = 0xFFFF;
		int16 dx;
		int16 dy;

		for (dy = -(int16)radius; dy <= (int16)radius; dy++) {
			for (dx = -(int16)radius; dx <= (int16)radius; dx++) {
				int16 x = Tile_GetPackedX(packed) + dx;
				int16 y = Tile_GetPackedY(packed) + dy;
				uint16 candidate;
				uint16 distance;
				uint16 i;
				bool claimed = false;

				if (max(abs(dx), abs(dy)) != (int16)radius) continue; /* ring only */
				if (x < 0 || x >= 64 || y < 0 || y >= 64) continue;
				candidate = Tile_PackXY(x, y);
				if (!Map_IsValidPosition(candidate)) continue;

				for (i = 0; i < s_spreadCount; i++) {
					if (s_spreadTiles[i] == candidate) claimed = true;
				}
				if (claimed) continue;
				if (Unit_GetTileEnterScore(unit, candidate, 0) > 255) continue;

				distance = Tile_GetDistancePacked(candidate, unitPacked);
				if (distance >= bestDistance) continue;
				bestDistance = distance;
				best = candidate;
			}
		}

		if (best != 0) {
			s_spreadTiles[s_spreadCount++] = best;
			return best;
		}
	}

	return packed;
}

/* Order the recipients of a group order by their distance to its target point,
 * so the nearest unit claims the nearest tile.  Handing tiles out in selection
 * order instead makes the group walk through itself. */
void UnitSelection_SortOrderByDistance(uint16 *order, uint16 count, uint16 packed)
{
	uint16 i;

	for (i = 1; i < count; i++) {
		uint16 index = order[i];
		Unit *unit = Unit_Get_ByIndex(index);
		uint16 distance = Tile_GetDistancePacked(Tile_PackTile(unit->o.position), packed);
		uint16 j = i;

		while (j > 0) {
			Unit *other = Unit_Get_ByIndex(order[j - 1]);

			if (Tile_GetDistancePacked(Tile_PackTile(other->o.position), packed) <= distance) break;
			order[j] = order[j - 1];
			j--;
		}
		order[j] = index;
	}
}

/* Send one unit on an advance: a Move to its own tile of the target area, with
 * the post that gives it its initiative on the way and its Guard mode on
 * arrival.
 *
 * The Move is only the transport.  What decides whether the unit answers to
 * anything on the way is the post, not the action: Unit_Autonomy_GetSearchRadius()
 * reads the post's action while one is open, so this Move searches with a
 * Guard radius, while a plain player Move - same action, no post - reads zero
 * and ignores the world until it arrives.  A post on its own would not move the
 * unit at all; RETURNING is a label meaning "travelling to the anchor", and the
 * travelling is always issued by whoever sets it (see Unit_Autonomy_ReturnToPost).
 *
 * The order of the last two calls is therefore not cosmetic.  Unit_SetAction()
 * on a manual order clears the post for every action but Attack, so the post
 * has to be opened after it - never before. */
static void UnitSelection_BeginAdvance(Unit *unit, uint16 packed)
{
	Unit_BeginManualOrder(unit);
	Unit_SetManualHunt(unit, false);
	Unit_AttackPosition_SetManual(unit, false);
	Unit_Harvester_BeginOrder(unit, ACTION_MOVE);
	Object_Script_Variable4_Clear(&unit->o);
	unit->targetAttack = 0;
	unit->targetMove = 0;
	unit->route[0] = 0xFF;

	Unit_SetGuardPosition(unit, packed);
	Unit_SetGuardAction(unit, ACTION_GUARD);
	Unit_SetAction(unit, ACTION_MOVE);
	Unit_SetDestination(unit, Tools_Index_Encode(packed, IT_TILE));
	Unit_Autonomy_BeginAdvance(unit, packed);
}

static void UnitSelection_ResetOrder(Unit *unit, ActionType action, uint16 packed)
{
	uint16 encoded;

	if (action == ACTION_ATTACK && UnitSelection_IsAdvanceTarget(packed)) {
		UnitSelection_BeginAdvance(unit, packed);
		return;
	}

	Unit_BeginManualOrder(unit);
	Unit_SetManualHunt(unit, false);
	Unit_AttackPosition_SetManual(unit, false);
	Unit_Harvester_BeginOrder(unit, action);
	Object_Script_Variable4_Clear(&unit->o);
	unit->targetAttack = 0;
	unit->targetMove = 0;
	unit->route[0] = 0xFF;

	if (action != ACTION_MOVE && action != ACTION_HARVEST) {
		encoded = Tools_Index_Encode(Unit_FindTargetAround(packed), IT_TILE);
	} else {
		encoded = Tools_Index_Encode(packed, IT_TILE);
	}

	Unit_SetAction(unit, action);

	if (action == ACTION_MOVE) {
		/* A player-directed move relocates the post; the completion hook replaces
		 * this with the exact arrival tile.  It also decides what the unit does
		 * once it is there: a Move is how the player places a unit somewhere, so
		 * it settles into the tight Guard, while an Attack order leaves it in the
		 * wide Area Guard where the fight was. */
		Unit_SetGuardPosition(unit, packed);
		Unit_SetGuardAction(unit, ACTION_GUARD);
		Unit_SetDestination(unit, encoded);
	} else if (action == ACTION_HARVEST) {
		unit->harvestCenter = packed;
		/* A Harvest order also books an automatic airlift when a carryall is
		 * available.  A busy carryall keeps the harvester waiting rather than
		 * sending it on a long ground detour; ground travel is the fallback only
		 * when the house has no carryall at all. */
		unit->airTransitDestination = packed;
		if (Unit_IsTypeOnMap(Unit_GetHouseID(unit), UNIT_CARRYALL)) {
			unit->targetMove = 0;
		} else {
			unit->targetMove = encoded;
		}
	} else {
		Unit_SetTarget(unit, encoded);
		if (action == ACTION_ATTACK || action == ACTION_AMBUSH) Unit_SetGuardAction(unit, ACTION_AREA_GUARD);
		if (action == ACTION_ATTACK) Unit_AttackPosition_SetManual(unit, true);
		unit = Tools_Index_GetUnit(unit->targetAttack);
		if (unit != NULL) unit->blinkCounter = 8;
	}
}

/* Single entry point for a targeted player order, shared by the group commands
 * and by the classic one-unit action panel.  Keeping the viewport's own copy of
 * this sequence around meant a single harvester and a selected group of
 * harvesters obeyed subtly different Harvest orders. */
void UnitSelection_IssueOrder(Unit *unit, ActionType action, uint16 packed)
{
	if (unit == NULL || action == ACTION_INVALID) return;
	UnitSelection_ResetOrder(unit, action, packed);
}

uint16 g_dirtyUnitCount = 0;
uint16 g_dirtyAirUnitCount = 0;

/**
 * Number of units of each type available at the starport.
 * \c 0 means not available, \c -1 means \c 0 units, \c >0 means that number of units available.
 */
int16 g_starportAvailable[UNIT_MAX];

/**
 * Rotate a unit (or his top).
 *
 * @param unit The Unit to operate on.
 * @param level 0 = base, 1 = top (turret etc).
 */
static void Unit_Rotate(Unit *unit, uint16 level)
{
	int8 target;
	int8 current;
	int8 newCurrent;
	int16 diff;

	assert(level == 0 || level == 1);

	if (unit->orientation[level].speed == 0) return;

	target = unit->orientation[level].target;
	current = unit->orientation[level].current;
	diff = target - current;

	if (diff > 128) diff -= 256;
	if (diff < -128) diff += 256;
	diff = abs(diff);

	newCurrent = current + unit->orientation[level].speed;

	if (abs(unit->orientation[level].speed) >= diff) {
		unit->orientation[level].speed = 0;
		newCurrent = target;
	}

	unit->orientation[level].current = newCurrent;

	if (Orientation_Orientation256ToOrientation16(newCurrent) == Orientation_Orientation256ToOrientation16(current) && Orientation_Orientation256ToOrientation8(newCurrent) == Orientation_Orientation256ToOrientation8(current)) return;

	Unit_UpdateMap(2, unit);
}

static void Unit_MovementTick(Unit *unit)
{
	uint16 speed;

	if (unit->speed == 0) return;

	speed = unit->speedRemainder;

	/* Units in the air don't feel the effect of gameSpeed */
	if (g_table_unitInfo[unit->o.type].movementType != MOVEMENT_WINGER) {
		speed += Tools_AdjustToGameSpeed(unit->speedPerTick, 1, 255, false);
	} else {
		speed += unit->speedPerTick;
	}

	if ((speed & 0xFF00) != 0) {
		Unit_Move(unit, min(unit->speed * 16, Tile_GetDistance(unit->o.position, unit->currentDestination) + 16));
	}

	unit->speedRemainder = speed & 0xFF;
}

/**
 * Loop over all units, performing various of tasks.
 */
void GameLoop_Unit(void)
{
	PoolFindStruct find;
	bool tickMovement  = false;
	bool tickRotation  = false;
	bool tickBlinking  = false;
	bool tickUnknown4  = false;
	bool tickScript    = false;
	bool tickUnknown5  = false;
	bool tickDeviation = false;

	if (g_debugScenario) return;

	if (s_tickUnitMovement <= g_timerGame) {
		tickMovement = true;
		s_tickUnitMovement = g_timerGame + 3;
	}

	if (s_tickUnitRotation <= g_timerGame) {
		tickRotation = true;
		s_tickUnitRotation = g_timerGame + Tools_AdjustToGameSpeed(4, 2, 8, true);
	}

	if (s_tickUnitBlinking <= g_timerGame) {
		tickBlinking = true;
		s_tickUnitBlinking = g_timerGame + 3;
	}

	if (s_tickUnitUnknown4 <= g_timerGame) {
		tickUnknown4 = true;
		s_tickUnitUnknown4 = g_timerGame + 20;
	}

	if (s_tickUnitScript <= g_timerGame) {
		tickScript = true;
		s_tickUnitScript = g_timerGame + 5;
	}

	if (s_tickUnitUnknown5 <= g_timerGame) {
		tickUnknown5 = true;
		s_tickUnitUnknown5 = g_timerGame + 5;
	}

	if (s_tickUnitDeviation <= g_timerGame) {
		tickDeviation = true;
		s_tickUnitDeviation = g_timerGame + 60;
	}

	find.houseID = HOUSE_INVALID;
	find.index   = 0xFFFF;
	find.type    = 0xFFFF;

	while (true) {
		const UnitInfo *ui;
		Unit *u;

		u = Unit_Find(&find);
		if (u == NULL) break;

		ui = &g_table_unitInfo[u->o.type];

		g_scriptCurrentObject    = &u->o;
		g_scriptCurrentStructure = NULL;
		g_scriptCurrentUnit      = u;
		g_scriptCurrentTeam      = NULL;

		if (u->o.flags.s.isNotOnMap) continue;

		if (tickUnknown4) {
			Unit_Autonomy_Update(u);
			Unit_Harvester_Update(u);
			Unit_AirTransit_Update(u);
			Unit_AttackPosition_Update(u);
		}

		if (tickUnknown4 && u->targetAttack != 0 && ui->o.flags.hasTurret) {
			tile32 tile;

			tile = Tools_Index_GetTile(u->targetAttack);

			Unit_SetOrientation(u, Tile_GetDirection(u->o.position, tile), false, 1);
		}

		if (tickMovement) {
			Unit_MovementTick(u);

			if (u->fireDelay != 0) {
				if (ui->movementType == MOVEMENT_WINGER && !ui->flags.isNormalUnit) {
					tile32 tile;

					tile = u->currentDestination;

					if (Tools_Index_GetType(u->targetAttack) == IT_UNIT && g_table_unitInfo[Tools_Index_GetUnit(u->targetAttack)->o.type].movementType == MOVEMENT_WINGER) {
						tile = Tools_Index_GetTile(u->targetAttack);
					}

					Unit_SetOrientation(u, Tile_GetDirection(u->o.position, tile), false, 0);
				}

				u->fireDelay--;
			}
		}

		if (tickRotation) {
			Unit_Rotate(u, 0);
			if (ui->o.flags.hasTurret) Unit_Rotate(u, 1);
		}

		if (tickBlinking && u->blinkCounter != 0) {
			u->blinkCounter--;
			if ((u->blinkCounter % 2) != 0) {
				u->o.flags.s.isHighlighted = true;
			} else {
				u->o.flags.s.isHighlighted = false;
			}

			Unit_UpdateMap(2, u);
		}

		if (tickDeviation) Unit_Deviation_Decrease(u, 1);

		if (ui->movementType != MOVEMENT_WINGER && Object_GetByPackedTile(Tile_PackTile(u->o.position)) == NULL) Unit_UpdateMap(1, u);

		if (tickUnknown5) {
			if (u->timer == 0) {
				if ((ui->movementType == MOVEMENT_FOOT && u->speed != 0) || u->o.flags.s.isSmoking) {
					if (u->spriteOffset >= 0) {
						u->spriteOffset &= 0x3F;
						u->spriteOffset++;

						Unit_UpdateMap(2, u);

						u->timer = ui->animationSpeed / 5;
						if (u->o.flags.s.isSmoking) {
							u->timer = 3;
							if (u->spriteOffset > 32) {
								u->o.flags.s.isSmoking = false;
								u->spriteOffset = 0;
							}
						}
					}
				}

				if (u->o.type == UNIT_ORNITHOPTER && u->o.flags.s.allocated && u->spriteOffset >= 0) {
					u->spriteOffset &= 0x3F;
					u->spriteOffset++;

					Unit_UpdateMap(2, u);

					u->timer = 1;
				}

				if (u->o.type == UNIT_HARVESTER) {
					if (u->actionID == ACTION_HARVEST || u->o.flags.s.isSmoking) {
						u->spriteOffset &= 0x3F;
						u->spriteOffset++;

						Unit_UpdateMap(2, u);

						u->timer = 4;
					} else {
						if (u->spriteOffset != 0) {
							Unit_UpdateMap(2, u);

							u->spriteOffset = 0;
						}
					}
				}
			} else {
				u->timer--;
			}
		}

		if (tickScript) {
			if (u->o.script.delay == 0) {
				if (Script_IsLoaded(&u->o.script)) {
					int opcodesLeft = SCRIPT_UNIT_OPCODES_PER_TICK + 2;
					if (!ui->o.flags.scriptNoSlowdown && !Map_IsPositionInViewport(u->o.position, NULL, NULL)) {
						opcodesLeft = 3;
					}

					u->o.script.variables[3] = g_playerHouseID;

					for (; opcodesLeft > 0 && u->o.script.delay == 0; opcodesLeft--) {
						if (!Script_Run(&u->o.script)) break;
					}
				}
			} else {
				u->o.script.delay--;
			}
		}

		if (u->nextActionID == ACTION_INVALID) continue;
		if (u->currentDestination.x != 0 || u->currentDestination.y != 0) continue;

		Unit_SetAction(u, u->nextActionID);
		u->nextActionID = ACTION_INVALID;
	}
}

/**
 * Get the HouseID of a unit. This is not always u->o.houseID, as a unit can be
 *  deviated by the Ordos.
 *
 * @param u Unit to get the HouseID of.
 * @return The HouseID of the unit, which might be deviated.
 */
uint8 Unit_GetHouseID(Unit *u)
{
	if (u->deviated != 0) {
		/* ENHANCEMENT -- Deviated units always belong to Ordos, no matter who did the deviating. */
		if (g_dune2_enhanced) return u->deviatedHouse;
		return HOUSE_ORDOS;
	}
	return u->o.houseID;
}

/**
 * Convert the name of a unit to the type value of that unit, or
 *  UNIT_INVALID if not found.
 */
uint8 Unit_StringToType(const char *name)
{
	uint8 type;
	if (name == NULL) return UNIT_INVALID;

	for (type = 0; type < UNIT_MAX; type++) {
		if (strcasecmp(g_table_unitInfo[type].o.name, name) == 0) return type;
	}

	return UNIT_INVALID;
}

/**
 * Convert the name of an action to the type value of that action, or
 *  ACTION_INVALID if not found.
 */
uint8 Unit_ActionStringToType(const char *name)
{
	uint8 type;
	if (name == NULL) return ACTION_INVALID;

	for (type = 0; type < ACTION_MAX; type++) {
		if (strcasecmp(g_table_actionInfo[type].name, name) == 0) return type;
	}

	return ACTION_INVALID;
}

/**
 * Convert the name of a movement to the type value of that movement, or
 *  MOVEMENT_INVALID if not found.
 */
uint8 Unit_MovementStringToType(const char *name)
{
	uint8 type;
	if (name == NULL) return MOVEMENT_INVALID;

	for (type = 0; type < MOVEMENT_MAX; type++) {
		if (strcasecmp(g_table_movementTypeName[type], name) == 0) return type;
	}

	return MOVEMENT_INVALID;
}

/**
 * Create a new Unit.
 *
 * @param index The new index of the Unit, or UNIT_INDEX_INVALID to assign one.
 * @param typeID The type of the new Unit.
 * @param houseID The House of the new Unit.
 * @param position To where on the map this Unit should be transported, or TILE_INVALID for not on the map yet.
 * @param orientation Orientation of the Unit.
 * @return The new created Unit, or NULL if something failed.
 */
Unit *Unit_Create(uint16 index, uint8 typeID, uint8 houseID, tile32 position, int8 orientation)
{
	const UnitInfo *ui;
	Unit *u;

	if (houseID >= HOUSE_MAX) return NULL;
	if (typeID >= UNIT_MAX) return NULL;

	ui = &g_table_unitInfo[typeID];
	u = Unit_Allocate(index, typeID, houseID);
	if (u == NULL) return NULL;

	u->o.houseID = houseID;

	Unit_SetOrientation(u, orientation, true, 0);
	Unit_SetOrientation(u, orientation, true, 1);

	Unit_SetSpeed(u, 0);

	u->o.position       = position;
	u->o.hitpoints      = ui->o.hitpoints;
	u->currentDestination.x = 0;
	u->currentDestination.y = 0;
	u->originEncoded    = 0x0000;
	u->route[0]         = 0xFF;

	if (position.x != 0xFFFF || position.y != 0xFFFF) {
		u->originEncoded = Unit_FindClosestRefinery(u);
		u->targetLast    = position;
		u->targetPreLast = position;
	}

	u->o.linkedID    = 0xFF;
	u->o.script.delay = 0;
	u->actionID      = ACTION_GUARD;
	u->nextActionID  = ACTION_INVALID;
	u->fireDelay     = 0;
	u->distanceToDestination = 0x7FFF;
	u->targetMove    = 0x0000;
	u->guardPosition = (position.x == 0xFFFF && position.y == 0xFFFF) ? 0 : Tile_PackTile(position);
	u->harvestCenter = (typeID == UNIT_HARVESTER && position.x != 0xFFFF && position.y != 0xFFFF) ? Tile_PackTile(position) : 0;
	u->harvestHoldPosition = 0;
	/* Pool indices are reused, so the previous owner's timers and stall
	 * counters must not leak into this unit. */
	Unit_Harvester_ResetState(u->o.index);
	u->repairReturnPosition = 0;
	u->airTransitDestination = 0;
	u->amount        = 0;
	u->wobbleIndex   = 0;
	u->spriteOffset  = 0;
	u->blinkCounter  = 0;
	u->timer   = 0;

	Script_Reset(&u->o.script, g_scriptUnit);

	u->o.flags.s.allocated = true;

	if (ui->movementType == MOVEMENT_TRACKED) {
		if (Tools_Random_256() < g_table_houseInfo[houseID].degradingChance) {
			u->o.flags.s.degrades = true;
		}
	}

	if (ui->movementType == MOVEMENT_WINGER) {
		Unit_SetSpeed(u, 255);
	} else {
		if ((position.x != 0xFFFF || position.y != 0xFFFF) && Unit_IsTileOccupied(u)) {
			Unit_Free(u);
			return NULL;
		}
	}

	/* Before the early return below: a factory creates its unit off the map and
	 * only places it when the doors open, so counting after that point misses
	 * every unit any factory ever made. */
	Skirmish_RecordBuilt(houseID, typeID);

	if ((position.x == 0xFFFF) && (position.y == 0xFFFF)) {
		u->o.flags.s.isNotOnMap = true;
		return u;
	}

	Unit_UpdateMap(1, u);

	/* Nobody scouts for a skirmish AI, and an unseen unit scores zero in
	 * Unit_GetTargetUnitPriority().  Unit_SetPosition() already reveals whatever
	 * a factory delivers, but units born straight onto the map -- the Palace
	 * special above all -- never pass through it.  Without this the Fremen an
	 * Atreides Palace summons walked into the enemy base unopposed and only ever
	 * died under a passing tank: measured at 9 shot against 111 crushed. */
	if (Skirmish_IsActive()) u->o.seenByHouses = 0xFF;

	Unit_SetAction(u, (houseID == g_playerHouseID) ? Unit_GetDefaultAction(u) : ui->actionAI);

	return u;
}

/**
 * Checks if a Unit is on the map.
 *
 * @param houseID The House of the Unit.
 * @param typeID The type of the Unit.
 * @return Returns true if and only if a Unit with the given attributes is on the map.
 */
bool Unit_IsTypeOnMap(uint8 houseID, uint8 typeID)
{
	uint16 i;

	for (i = 0; i < g_unitFindCount; i++) {
		Unit *u;

		u = g_unitFindArray[i];
		if (houseID != HOUSE_INVALID && Unit_GetHouseID(u) != houseID) continue;
		if (typeID != UNIT_INVALID && u->o.type != typeID) continue;
		if (g_validateStrictIfZero == 0 && u->o.flags.s.isNotOnMap) continue;

		return true;
	}
	return false;
}

/**
 * Sets the action the given unit will execute.
 *
 * @param u The Unit to set the action for.
 * @param action The action.
 */
void Unit_SetAction(Unit *u, ActionType action)
{
	const ActionInfo *ai;
	bool manualOrder;
	bool preserveDefensivePost;

	if (u == NULL) return;
	if (u->actionID == ACTION_DESTRUCT || u->actionID == ACTION_DIE || action == ACTION_INVALID) return;
	manualOrder = s_manualOrderStarting[u->o.index];
	s_manualOrderStarting[u->o.index] = false;

	/* Legacy Guard scripts often switch to Move/Attack themselves.  Treat that
	 * as the same defensive sortie as a target found by the autonomous layer. */
	if (!manualOrder && s_autonomousPost[u->o.index].state == AUTONOMOUS_POST_NONE
		&& (u->actionID == ACTION_GUARD || u->actionID == ACTION_AREA_GUARD)
		&& (action == ACTION_ATTACK || (action == ACTION_MOVE && Tools_Index_IsValid(u->targetAttack)))
		&& Unit_Autonomy_IsCombatUnit(u)) {
		Unit_AttackPosition_SetAutomatic(u, u->actionID);
	}

	preserveDefensivePost = !manualOrder && s_autonomousPost[u->o.index].state == AUTONOMOUS_POST_ENGAGING;
	if (action != ACTION_ATTACK && !preserveDefensivePost) Unit_AttackPosition_SetManual(u, false);

	ai = &g_table_actionInfo[action];

	switch (ai->switchType) {
		case 0:
			if (u->currentDestination.x != 0 || u->currentDestination.y != 0) {
				u->nextActionID = action;
				return;
			}
			/* FALL-THROUGH */
		case 1:
			u->actionID = action;
			u->nextActionID = ACTION_INVALID;
			u->currentDestination.x = 0;
			u->currentDestination.y = 0;
			u->o.script.delay = 0;
			Script_Reset(&u->o.script, g_scriptUnit);
			u->o.script.variables[0] = action;
			Script_Load(&u->o.script, u->o.type);
			return;

		case 2:
			u->o.script.variables[0] = action;
			Script_LoadAsSubroutine(&u->o.script, u->o.type);
			return;

		default: return;
	}
}

/* Remember the guard mode the player actually picked.  ACTION_ATTACK (0) is
 * never a guard mode, so a zero field means "player never chose" and old saves
 * keep the previous behaviour. */
void Unit_SetGuardAction(Unit *u, ActionType action)
{
	if (u == NULL) return;
	if (action != ACTION_GUARD && action != ACTION_AREA_GUARD) return;

	u->guardAction = action;
}

/* Player combat units default to a wider local defence. Harvesters, special
 * units and AI retain their original table-driven default actions. */
ActionType Unit_GetDefaultAction(const Unit *u)
{
	const UnitInfo *ui;

	if (u == NULL) return ACTION_GUARD;
	ui = &g_table_unitInfo[u->o.type];
	if (u->o.houseID == g_playerHouseID && ui->flags.isNormalUnit && ui->flags.isGroundUnit && ui->fireDistance != 0 &&
		(ui->movementType == MOVEMENT_FOOT || ui->movementType == MOVEMENT_TRACKED || ui->movementType == MOVEMENT_WHEELED)) {
		/* An explicit order outranks the fork's wider default.  The original
		 * attack script ends a finished fight with SetAction(ACTION_MOVE) when
		 * targetMove is still set (UNIT.EMC word 238), and the move script exits
		 * through SetActionDefault, which lands here.  Every unit that had to
		 * close in on its target - anything whose range is below its guard
		 * radius - therefore came back from its first sortie promoted from
		 * Guard to Area Guard, while units that fired from the spot kept Guard. */
		if (u->guardAction == ACTION_GUARD || u->guardAction == ACTION_AREA_GUARD) return (ActionType)u->guardAction;
		return ACTION_AREA_GUARD;
	}

	return ui->o.actionsPlayer[3];
}

/* Area Guard in the original unit script uses originEncoded as its home.
 * Keep it in lockstep with our explicit post so legacy and new behaviour
 * agree about where a manually moved unit should return. */
void Unit_SetGuardPosition(Unit *u, uint16 packed)
{
	const UnitInfo *ui;

	if (u == NULL || !Map_IsValidPosition(packed)) return;
	u->guardPosition = packed;

	ui = &g_table_unitInfo[u->o.type];
	if (Unit_GetHouseID(u) == g_playerHouseID && ui->flags.isNormalUnit && ui->flags.isGroundUnit && ui->fireDistance != 0) {
		u->originEncoded = Tools_Index_Encode(packed, IT_TILE);
	}
}

/* Compact, player-facing state for the individual-unit panel.  It describes
 * what the unit is doing now instead of merely exposing its selected command. */
void Unit_GetStatusText(const Unit *u, char *state, char *detail, uint16 length)
{
	uint16 packed;
	Structure *structure;

	if (state == NULL || detail == NULL || length == 0) return;
	state[0] = '\0';
	detail[0] = '\0';
	if (u == NULL) return;

	/* The legacy harvest animation also sets inTransport, so it is not a
	 * reliable indication of actual Carryall transit for a visible unit. */
	if (s_autonomousPost[u->o.index].state == AUTONOMOUS_POST_ENGAGING) {
		snprintf(state, length, "DEFENDING");
		snprintf(detail, length, "POST %u,%u", Tile_GetPackedX(s_autonomousPost[u->o.index].anchor), Tile_GetPackedY(s_autonomousPost[u->o.index].anchor));
		return;
	}
	if (s_autonomousPost[u->o.index].state == AUTONOMOUS_POST_RETURNING) {
		snprintf(state, length, "RETURNING");
		snprintf(detail, length, "POST %u,%u", Tile_GetPackedX(s_autonomousPost[u->o.index].anchor), Tile_GetPackedY(s_autonomousPost[u->o.index].anchor));
		return;
	}

	if (u->o.type == UNIT_HARVESTER) {
		structure = Tools_Index_GetStructure(u->targetMove);
		if (structure == NULL && Tools_Index_GetType(u->originEncoded) == IT_STRUCTURE) {
			structure = Tools_Index_GetStructure(u->originEncoded);
		}
		if (structure != NULL && structure->o.type == STRUCTURE_REFINERY) {
			snprintf(state, length, Unit_Harvester_RefineryAccepts(structure, u) ? "TO REF" : "WAIT REF");
			snprintf(detail, length, "CARGO %u%%", u->amount);
			return;
		}
		if (Unit_Harvester_HasCarryallReservation(u) || u->airTransitDestination != 0) {
			snprintf(state, length, "WAIT PICKUP");
			snprintf(detail, length, "CARGO %u%%", u->amount);
			return;
		}
		if (u->amount >= 100) {
			snprintf(state, length, "RETURNING");
			snprintf(detail, length, "CARGO 100%%");
			return;
		}
		if (u->actionID == ACTION_HARVEST) {
			snprintf(state, length, "HARVESTING");
			snprintf(detail, length, "CARGO %u%%", u->amount);
			return;
		}
	}

	if (u->actionID == ACTION_AREA_GUARD || u->actionID == ACTION_GUARD) {
		packed = u->guardPosition;
		snprintf(state, length, u->actionID == ACTION_AREA_GUARD ? "AREA GUARD" : "GUARD");
		snprintf(detail, length, "POST %u,%u", Tile_GetPackedX(packed), Tile_GetPackedY(packed));
		return;
	}
	/* A worm engagement is its own state: the unit is deliberately not closing
	 * in, and without saying so the panel would just show it standing idle. */
	if (Unit_IsSandwormTarget(u->targetAttack)) {
		snprintf(state, length, "VS WORM");
		snprintf(detail, length, g_table_landscapeInfo[Map_GetLandscapeType(Tile_PackTile(u->o.position))].isSand ? "ON SAND" : "ON ROCK");
		return;
	}
	if (u->actionID == ACTION_HUNT || s_manualHunt[u->o.index]) {
		snprintf(state, length, "HUNTING");
		snprintf(detail, length, "SEEK TARGET");
		return;
	}
	if (u->actionID == ACTION_ATTACK) {
		snprintf(state, length, "ATTACKING");
		/* Movement inside an attack is not a state of its own: the unit is on
		 * its way to the firing position picked for it. */
		if (s_attackPositionTile[u->o.index] != 0) snprintf(detail, length, "TO POSITION");
		else snprintf(detail, length, Tools_Index_IsValid(u->targetAttack) ? "TARGET LOCK" : "NO TARGET");
		return;
	}
	if (u->actionID == ACTION_MOVE) {
		packed = Tools_Index_GetPackedTile(u->targetMove);
		snprintf(state, length, "MOVING");
		if (Map_IsValidPosition(packed)) snprintf(detail, length, "TO %u,%u", Tile_GetPackedX(packed), Tile_GetPackedY(packed));
		return;
	}

	snprintf(state, length, "%s", String_Get_ByIndex(g_table_actionInfo[u->actionID].stringID));
	snprintf(detail, length, "IDLE");
}

/* A manual order establishes a new defensive post. Autonomous defence keeps
 * its existing post, so reacting to a threat never drifts the unit's area. */
ActionType Unit_GetDefaultActionAfterCompletion(Unit *u)
{
	if (u != NULL && s_autonomousPost[u->o.index].state == AUTONOMOUS_POST_RETURNING) {
		ActionType action = s_autonomousPost[u->o.index].action;
		Unit_Autonomy_ClearPost(u);
		return action;
	}

	if (u != NULL && Unit_GetHouseID(u) == g_playerHouseID && u->actionID == ACTION_MOVE) {
		uint16 packed = Tile_PackTile(u->o.position);

		/* Not every completed Move is a player order.  The original attack
		 * script ends a finished fight with SetAction(ACTION_MOVE) (UNIT.EMC
		 * word 238), and the sortie can already have lost its ENGAGING mark by
		 * then - Unit_Autonomy_ReturnToPost() clears the post before it decides
		 * how to return, and its Unit_SetAction() is deferred into nextActionID
		 * while the unit is still between two tiles.  Re-anchoring there moved
		 * the post a couple of tiles forward after every skirmish, which is how
		 * a defensive line walked into the enemy over a few waves.
		 *
		 * A player Move anchors the post on its destination tile when the order
		 * is given (UnitSelection_ResetOrder), so arriving at one's own post is
		 * the signature of a real order; this then only refines the post to the
		 * tile actually reached.  Anything that ends far from the post is combat
		 * movement and leaves the post alone. */
		if (!Map_IsValidPosition(u->guardPosition) || Tile_GetDistancePacked(packed, u->guardPosition) <= 2) {
			Unit_SetGuardPosition(u, packed);
		}
	} else if (u != NULL && Unit_GetHouseID(u) == g_playerHouseID && u->actionID == ACTION_ATTACK &&
		s_attackPositionManual[u->o.index] && s_autonomousPost[u->o.index].state != AUTONOMOUS_POST_ENGAGING) {
		/* A player-issued Attack is a manual order: where it ends is the new
		 * post, exactly as before. */
		Unit_SetGuardPosition(u, Tile_PackTile(u->o.position));
	}

	return Unit_GetDefaultAction(u);
}

/**
 * Adds the specified unit to the specified team.
 *
 * @param u The unit to add to the team.
 * @param t The team to add the unit to.
 * @return Amount of space left in the team.
 */
uint16 Unit_AddToTeam(Unit *u, Team *t)
{
	if (t == NULL || u == NULL) return 0;

	u->team = t->index + 1;
	t->members++;

	return t->maxMembers - t->members;
}

/**
 * Removes the specified unit from its team.
 *
 * @param u The unit to remove from the team it is in.
 * @return Amount of space left in the team.
 */
uint16 Unit_RemoveFromTeam(Unit *u)
{
	Team *t;

	if (u == NULL) return 0;
	if (u->team == 0) return 0;

	t = Team_Get_ByIndex(u->team - 1);

	t->members--;
	u->team = 0;

	return t->maxMembers - t->members;
}

/**
 * Gets the team of the given unit.
 *
 * @param u The unit to get the team of.
 * @return The team.
 */
Team *Unit_GetTeam(Unit *u)
{
	if (u == NULL) return NULL;
	if (u->team == 0) return NULL;
	return Team_Get_ByIndex(u->team - 1);
}

/**
 * ?? Sorts unit array and count enemy/allied units.
 */
void Unit_Sort(void)
{
	House *h;
	uint16 i;

	h = g_playerHouse;
	h->unitCountEnemy = 0;
	h->unitCountAllied = 0;

	for (i = 0; i < g_unitFindCount - 1; i++) {
		Unit *u1;
		Unit *u2;
		uint16 y1;
		uint16 y2;

		u1 = g_unitFindArray[i];
		u2 = g_unitFindArray[i + 1];
		y1 = Tile_GetY(u1->o.position);
		y2 = Tile_GetY(u2->o.position);
		if (g_table_unitInfo[u1->o.type].movementType == MOVEMENT_FOOT) y1 -= 0x100;
		if (g_table_unitInfo[u2->o.type].movementType == MOVEMENT_FOOT) y2 -= 0x100;

		if ((int16)y1 > (int16)y2) {
			g_unitFindArray[i] = u2;
			g_unitFindArray[i + 1] = u1;
		}
	}

	for (i = 0; i < g_unitFindCount; i++) {
		Unit *u;

		u = g_unitFindArray[i];
		if ((u->o.seenByHouses & (1 << g_playerHouseID)) != 0 && !u->o.flags.s.isNotOnMap) {
			if (House_AreAllied(u->o.houseID, g_playerHouseID)) {
				h->unitCountAllied++;
			} else {
				h->unitCountEnemy++;
			}
		}
	}
}

/**
 * Get the unit on the given packed tile.
 *
 * @param packed The packed tile to get the unit from.
 * @return The unit.
 */
Unit *Unit_Get_ByPackedTile(uint16 packed)
{
	Tile *tile;

	if (Tile_IsOutOfMap(packed)) return NULL;

	tile = &g_map[packed];
	if (!tile->hasUnit) return NULL;
	return Unit_Get_ByIndex(tile->index - 1);
}

/**
 * Determines whether a move order into the given structure is OK for
 * a particular unit.
 *
 * It handles orders to invade enemy buildings as well as going into
 * a friendly structure (e.g. refinery, repair facility).
 *
 * @param unit The Unit to operate on.
 * @param s The Structure to operate on.
 * @return
 * 0 - invalid movement
 * 1 - valid movement, will try to get close to the structure
 * 2 - valid movement, will attempt to damage/conquer the structure
 */
uint16 Unit_IsValidMovementIntoStructure(Unit *unit, Structure *s)
{
	const StructureInfo *si;
	const UnitInfo *ui;
	uint16 unitEnc;
	uint16 structEnc;

	if (unit == NULL || s == NULL) return 0;

	si = &g_table_structureInfo[s->o.type];
	ui = &g_table_unitInfo[unit->o.type];

	unitEnc = Tools_Index_Encode(unit->o.index, IT_UNIT);
	structEnc = Tools_Index_Encode(s->o.index, IT_STRUCTURE);

	/* Movement into structure of other owner. */
	if (Unit_GetHouseID(unit) != s->o.houseID) {
		/* Saboteur can always enter houses */
		if (unit->o.type == UNIT_SABOTEUR && unit->targetMove == structEnc) return 2;
		/* Entering houses is only possible for foot-units and if the structure is conquerable.
		 * Everyone else can only move close to the building. */
		if (ui->movementType == MOVEMENT_FOOT && si->o.flags.conquerable) return unit->targetMove == structEnc ? 2 : 1;
		return 0;
	}

	/* Prevent movement if target structure does not accept the unit type. */
	if ((si->enterFilter & (1 << unit->o.type)) == 0) return 0;

	/* TODO -- Not sure. */
	if (s->o.script.variables[4] == unitEnc) return 2;

	/* Enter only if structure not linked to any other unit already. */
	return s->o.linkedID == 0xFF ? 1 : 0;
}

/**
 * Sets the destination for the given unit.
 *
 * @param u The unit to set the destination for.
 * @param destination The destination (encoded index).
 */
void Unit_SetDestination(Unit *u, uint16 destination)
{
	Structure *s;

	if (u == NULL) return;
	if (!Tools_Index_IsValid(destination)) return;
	if (u->targetMove == destination) return;

	if (Tools_Index_GetType(destination) == IT_TILE) {
		Unit *u2;
		uint16 packed;

		packed = Tools_Index_Decode(destination);

		u2 = Unit_Get_ByPackedTile(packed);
		if (u2 != NULL) {
			if (u != u2) destination = Tools_Index_Encode(u2->o.index, IT_UNIT);
		} else {
			s = Structure_Get_ByPackedTile(packed);
			if (s != NULL) destination = Tools_Index_Encode(s->o.index, IT_STRUCTURE);
		}
	}

	/* Refuse to walk into a sandworm.  The tile lookup above has already turned
	 * the worm's own tile into its unit index, so this covers both the script
	 * driving at the target and a tile order that lands on top of it. */
	if (u->o.type != UNIT_SANDWORM && Unit_IsSandwormTarget(destination)) return;

	s = Tools_Index_GetStructure(destination);
	if (s != NULL && s->o.houseID == Unit_GetHouseID(u)) {
		if (Unit_IsValidMovementIntoStructure(u, s) == 1 || g_table_unitInfo[u->o.type].movementType == MOVEMENT_WINGER) {
			Object_Script_Variable4_Link(Tools_Index_Encode(u->o.index, IT_UNIT), destination);
		}
	}

	u->targetMove = destination;
	u->route[0]   = 0xFF;
}

/**
 * Get the priority a target unit has for a given unit. The higher the value,
 *  the more serious it should look at the target.
 *
 * @param unit The unit looking at a target.
 * @param target The unit to look at.
 * @return The priority of the target.
 */
uint16 Unit_GetTargetUnitPriority(Unit *unit, Unit *target)
{
	const UnitInfo *targetInfo;
	const UnitInfo *unitInfo;
	uint16 distance;
	uint16 priority;

	if (unit == NULL || target == NULL) return 0;
	if (unit == target) return 0;

	if (!target->o.flags.s.allocated) return 0;
	if ((target->o.seenByHouses & (1 << Unit_GetHouseID(unit))) == 0) return 0;

	if (House_AreAllied(Unit_GetHouseID(unit), Unit_GetHouseID(target))) return 0;

	unitInfo   = &g_table_unitInfo[unit->o.type];
	targetInfo = &g_table_unitInfo[target->o.type];

	if (!targetInfo->o.flags.priority) return 0;

	if (targetInfo->movementType == MOVEMENT_WINGER) {
		if (!unitInfo->o.flags.targetAir) return 0;
		if (target->o.houseID == g_playerHouseID && !Map_IsPositionUnveiled(Tile_PackTile(target->o.position))) return 0;
	}

	if (!Map_IsValidPosition(Tile_PackTile(target->o.position))) return 0;

	distance = Tile_GetDistanceRoundedUp(unit->o.position, target->o.position);

	if (!Map_IsValidPosition(Tile_PackTile(unit->o.position))) {
		if (targetInfo->fireDistance >= distance) return 0;
	}

	priority = targetInfo->o.priorityTarget + targetInfo->o.priorityBuild;
	if (distance != 0) priority = (priority / distance) + 1;

	if (priority > 0x7D00) return 0x7D00;
	return priority;
}

/**
 * Finds the closest refinery a harvester can go to.
 *
 * @param unit The unit to find the closest refinery for.
 * @return 1 if unit->originEncoded was not 0, else 0.
 */
uint16 Unit_FindClosestRefinery(Unit *unit)
{
	uint16 res;
	Structure *s = NULL;
	uint16 mind = 0;
	Structure *s2;
	uint16 d;
	PoolFindStruct find;

	res = (unit->originEncoded == 0) ? 0 : 1;

	if (unit->o.type != UNIT_HARVESTER) {
		unit->originEncoded = Tools_Index_Encode(Tile_PackTile(unit->o.position), IT_TILE);
		return res;
	}

	/* A choice already made is kept.
	 *
	 * The script asks this question again on every one of its ticks, so a
	 * decision made here used to survive only until the next one: a harvester
	 * that had politely gone to the second refinery was re-aimed at the nearest
	 * one the moment the first harvester stepped inside, and the fleet collapsed
	 * back onto one door.  Holding the claim until it stops being usable is what
	 * makes the choice a decision instead of a suggestion. */
	s2 = Tools_Index_GetStructure(unit->originEncoded);
	if (s2 != NULL && s2->o.type == STRUCTURE_REFINERY && Unit_Harvester_HoldsClaim(s2, unit)
		&& Unit_Harvester_RefineryAccepts(s2, unit)) {
		Unit_Harvester_Claim(s2, unit);
		return res;
	}

	find.type = STRUCTURE_REFINERY;
	find.houseID = Unit_GetHouseID(unit);
	find.index = 0xFFFF;

	while (true) {
		s2 = Structure_Find(&find);
		if (s2 == NULL) break;
		if (!Unit_Harvester_RefineryAccepts(s2, unit)) continue;
		d = Tile_GetDistance(unit->o.position, s2->o.position);
		if (mind != 0 && d >= mind) continue;
		mind = d;
		s = s2;
	}

	if (s == NULL) {
		find.type = STRUCTURE_REFINERY;
		find.houseID = Unit_GetHouseID(unit);
		find.index = 0xFFFF;

		while (true) {
			s2 = Structure_Find(&find);
			if (s2 == NULL) break;
			if (s2->o.hitpoints == 0) continue;
			d = Tile_GetDistance(unit->o.position, s2->o.position);
			if (mind != 0 && d >= mind) continue;
			mind = d;
			s = s2;
		}
	}

	if (s != NULL) {
		unit->originEncoded = Tools_Index_Encode(s->o.index, IT_STRUCTURE);
		/* Claim it here too: this is the choice the original script makes at the
		 * start of every return trip, and it is where the pile-up began. */
		Unit_Harvester_Claim(s, unit);
	}

	return res;
}

/**
 * Sets the position of the given unit.
 *
 * @param u The Unit to set the position for.
 * @position The position.
 * @return True if and only if the position changed.
 */
bool Unit_SetPosition(Unit *u, tile32 position)
{
	const UnitInfo *ui;

	if (u == NULL) return false;

	ui = &g_table_unitInfo[u->o.type];
	u->o.flags.s.isNotOnMap = false;

	u->o.position = Tile_Center(position);

	if (u->originEncoded == 0) Unit_FindClosestRefinery(u);

	u->o.script.variables[4] = 0;

	if (Unit_IsTileOccupied(u)) {
		u->o.flags.s.isNotOnMap = true;
		return false;
	}

	u->currentDestination.x = 0;
	u->currentDestination.y = 0;
	u->targetMove = 0;
	u->targetAttack = 0;
	if (u->o.houseID == g_playerHouseID && u->o.type != UNIT_HARVESTER) Unit_SetGuardPosition(u, Tile_PackTile(u->o.position));

	if (g_map[Tile_PackTile(u->o.position)].isUnveiled) {
		/* A new unit being delivered fresh from the factory; force a seenByHouses
		 *  update and add it to the statistics etc. */
		u->o.seenByHouses &= ~(1 << u->o.houseID);
		Unit_HouseUnitCount_Add(u, g_playerHouseID);
	}

	/* Nobody scouts for a skirmish AI: seeing the enemy is normally a side
	 * effect of the human unveiling the map, and an unseen unit scores zero
	 * in Unit_GetTargetUnitPriority(), so the AIs would ignore each other. */
	if (Skirmish_IsActive()) u->o.seenByHouses = 0xFF;

	if (u->o.houseID != g_playerHouseID || u->o.type == UNIT_HARVESTER || u->o.type == UNIT_SABOTEUR) {
		Unit_SetAction(u, ui->actionAI);
	} else {
		Unit_SetAction(u, Unit_GetDefaultAction(u));
	}

	u->spriteOffset = 0;

	Unit_UpdateMap(1, u);

	return true;
}

/**
 * Remove the Unit from the game, doing all required administration for it, like
 *  deselecting it, remove it from the radar count, stopping scripts, ..
 *
 * @param u The Unit to remove.
 */
void Unit_Remove(Unit *u)
{
	if (u == NULL) return;

	u->o.flags.s.allocated = true;
	Unit_UntargetMe(u);

	UnitSelection_Remove(u);

	u->o.flags.s.bulletIsBig = true;
	Unit_UpdateMap(0, u);

	Unit_HouseUnitCount_Remove(u);

	Script_Reset(&u->o.script, g_scriptUnit);

	Unit_Free(u);
}

/**
 * Gets the best target unit for the given unit.
 *
 * @param u The Unit to get the best target for.
 * @param mode How to determine the best target.
 * @return The best target or NULL if none found.
 */
Unit *Unit_FindBestTargetUnit(Unit *u, uint16 mode)
{
	tile32 position;
	uint16 distance;
	PoolFindStruct find;
	Unit *best = NULL;
	uint16 bestPriority = 0;

	if (u == NULL) return NULL;

	position = u->o.position;
	if (u->originEncoded == 0) {
		u->originEncoded = Tools_Index_Encode(Tile_PackTile(position), IT_TILE);
	} else {
		position = Tools_Index_GetTile(u->originEncoded);
	}

	distance = g_table_unitInfo[u->o.type].fireDistance << 8;
	if (mode == 2) distance <<= 1;

	find.houseID = HOUSE_INVALID;
	find.type    = 0xFFFF;
	find.index   = 0xFFFF;

	while (true) {
		Unit *target;
		uint16 priority;

		target = Unit_Find(&find);

		if (target == NULL) break;

		if (mode != 0 && mode != 4) {
			if (mode == 1) {
				if (Tile_GetDistance(u->o.position, target->o.position) > distance) continue;
			}
			if (mode == 2) {
				if (Tile_GetDistance(position, target->o.position) > distance) continue;
			}
		}

		priority = Unit_GetTargetUnitPriority(u, target);

		if ((int16)priority > (int16)bestPriority) {
			best = target;
			bestPriority = priority;
		}
	}

	if (bestPriority == 0) return NULL;

	return best;
}

/**
 * Get the priority for a target. Various of things have influence on this score,
 *  most noticeable the movementType of the target, his distance to you, and
 *  if he is moving/firing.
 * @note It only considers units on sand.
 *
 * @param unit The Unit that is requesting the score.
 * @param target The Unit that is being targeted.
 * @return The priority of the target.
 */
static uint16 Unit_Sandworm_GetTargetPriority(Unit *unit, Unit *target)
{
	uint16 res;
	uint16 distance;

	if (unit == NULL || target == NULL) return 0;
	if (!Map_IsPositionUnveiled(Tile_PackTile(target->o.position))) return 0;
	if (!g_table_landscapeInfo[Map_GetLandscapeType(Tile_PackTile(target->o.position))].isSand) return 0;

	switch(g_table_unitInfo[target->o.type].movementType) {
		case MOVEMENT_FOOT:      res = 0x64;   break;
		case MOVEMENT_TRACKED:   res = 0x3E8;  break;
		case MOVEMENT_HARVESTER: res = 0x3E8;  break;
		case MOVEMENT_WHEELED:   res = 0x1388; break;
		default:                 res = 0;      break;
	}

	if (target->speed != 0 || target->fireDelay != 0) res *= 4;

	distance = Tile_GetDistanceRoundedUp(unit->o.position, target->o.position);

	if (distance != 0 && res != 0) res /= distance;
	if (distance < 2) res *= 2;

	return res;
}

/**
 * Find the best target, based on the score. Only considers units on sand.
 *
 * @param unit The unit to search a target for.
 * @return A target Unit, or NULL if none is found.
 */
Unit *Unit_Sandworm_FindBestTarget(Unit *unit)
{
	Unit *best = NULL;
	PoolFindStruct find;
	uint16 bestPriority = 0;

	if (unit == NULL) return NULL;

	find.houseID = HOUSE_INVALID;
	find.type    = 0xFFFF;
	find.index   = 0xFFFF;

	while (true) {
		Unit *u;
		uint16 priority;

		u = Unit_Find(&find);

		if (u == NULL) break;

		priority = Unit_Sandworm_GetTargetPriority(unit, u);

		if (priority >= bestPriority) {
			best = u;
			bestPriority = priority;
		}
	}

	if (bestPriority == 0) return NULL;

	return best;
}

/**
 * Initiate the first movement of a Unit when the pathfinder has found a route.
 *
 * @param unit The Unit to operate on.
 * @return True if movement was initiated (not blocked etc).
 */
bool Unit_StartMovement(Unit *unit)
{
	const UnitInfo *ui;
	int8 orientation;
	uint16 packed;
	uint16 type;
	tile32 position;
	uint16 speed;
	int16 score;

	if (unit == NULL) return false;

	ui = &g_table_unitInfo[unit->o.type];

	orientation = (int8)((unit->orientation[0].current + 16) & 0xE0);

	Unit_SetOrientation(unit, orientation, true, 0);
	Unit_SetOrientation(unit, orientation, false, 1);

	position = Tile_MoveByOrientation(unit->o.position, orientation);

	packed = Tile_PackTile(position);

	unit->distanceToDestination = 0x7FFF;

	score = Unit_GetTileEnterScore(unit, packed, orientation / 32);

	if (score > 255 || score == -1) return false;

	/* Last line of defence against walking into a sandworm.  Every layer above
	 * can be talked into an approach - the scripts run four times as often as
	 * the tactical pass and reach the pathfinder through more than one route -
	 * so the step onto sand is refused where it is finally committed.  Leaving
	 * the worm or moving sideways stays allowed, and rock is never refused. */
	if (!Unit_Sandworm_StepAllowed(unit, packed)) return false;

	type = Map_GetLandscapeType(packed);
	if (type == LST_STRUCTURE) type = LST_CONCRETE_SLAB;

	speed = g_table_landscapeInfo[type].movementSpeed[ui->movementType];

	if (unit->o.type == UNIT_SABOTEUR && type == LST_WALL) speed = 255;
	unit->o.flags.s.isSmoking = false;

	/* ENHANCEMENT -- the flag is never set to false in original Dune2; in result, once the wobbling starts, it never stops. */
	if (g_dune2_enhanced) {
		unit->o.flags.s.isWobbling = g_table_landscapeInfo[type].letUnitWobble;
	} else {
		if (g_table_landscapeInfo[type].letUnitWobble) unit->o.flags.s.isWobbling = true;
	}

	if ((ui->o.hitpoints / 2) > unit->o.hitpoints && ui->movementType != MOVEMENT_WINGER) speed -= speed / 4;

	Unit_SetSpeed(unit, speed);

	if (ui->movementType != MOVEMENT_SLITHER) {
		tile32 positionOld;

		positionOld = unit->o.position;
		unit->o.position = position;

		Unit_UpdateMap(1, unit);

		unit->o.position = positionOld;
	}

	unit->currentDestination = position;

	Unit_Deviation_Decrease(unit, 10);

	return true;
}

/**
 * Set the target for the given unit.
 *
 * @param unit The Unit to set the target for.
 * @param encoded The encoded index of the target.
 */
void Unit_SetTarget(Unit *unit, uint16 encoded)
{
	if (unit == NULL || !Tools_Index_IsValid(encoded)) return;
	if (unit->targetAttack == encoded) return;

	if (Tools_Index_GetType(encoded) == IT_TILE) {
		uint16 packed;
		Unit *u;

		packed = Tools_Index_Decode(encoded);

		u = Unit_Get_ByPackedTile(packed);
		if (u != NULL) {
			encoded = Tools_Index_Encode(u->o.index, IT_UNIT);
		} else {
			Structure *s;

			s = Structure_Get_ByPackedTile(packed);
			if (s != NULL) {
				encoded = Tools_Index_Encode(s->o.index, IT_STRUCTURE);
			}
		}
	}

	if (Tools_Index_Encode(unit->o.index, IT_UNIT) == encoded) {
		encoded = Tools_Index_Encode(Tile_PackTile(unit->o.position), IT_TILE);
	}

	/* Some original Guard scripts acquire a target without first calling
	 * Unit_SetAction(ACTION_ATTACK).  Snapshot their post at target acquisition
	 * so their later movement cannot become a permanent formation drift. */
	if (s_autonomousPost[unit->o.index].state == AUTONOMOUS_POST_NONE
		&& (unit->actionID == ACTION_GUARD || unit->actionID == ACTION_AREA_GUARD)
		&& Unit_Autonomy_IsCombatUnit(unit)) {
		Unit_AttackPosition_SetAutomatic(unit, unit->actionID);
	}

	unit->targetAttack = encoded;

	if (!g_table_unitInfo[unit->o.type].o.flags.hasTurret) {
		/* The original scripts make acquiring a target an order to walk into it.
		 * That is the second half of the pendulum: the tactical layer sends the
		 * unit to a firing position, this sends it at the target, and the two
		 * take turns every pass.  While a unit is under tactical control its
		 * destination belongs to that layer alone. */
		unit->targetMove = (Unit_IsSandwormTarget(encoded) || Unit_AttackPosition_IsManaged(unit)) ? 0 : encoded;
		unit->route[0] = 0xFF;
	}
}

/**
 * Decrease deviation counter for the given unit.
 *
 * @param unit The Unit to decrease counter for.
 * @param amount The amount to decrease.
 * @return True if and only if the unit lost deviation.
 */
bool Unit_Deviation_Decrease(Unit *unit, uint16 amount)
{
	const UnitInfo *ui;

	if (unit == NULL || unit->deviated == 0) return false;

	ui = &g_table_unitInfo[unit->o.type];

	if (!ui->flags.isNormalUnit) return false;

	if (amount == 0) {
		amount = g_table_houseInfo[unit->o.houseID].toughness;
	}

	if (unit->deviated > amount) {
		unit->deviated -= amount;
		return false;
	}

	unit->deviated = 0;

	unit->o.flags.s.bulletIsBig = true;
	Unit_UpdateMap(2, unit);
	unit->o.flags.s.bulletIsBig = false;

	if (unit->o.houseID == g_playerHouseID) {
		Unit_SetAction(unit, ui->o.actionsPlayer[3]);
	} else {
		Unit_SetAction(unit, ui->actionAI);
	}

	Unit_UntargetMe(unit);
	unit->targetAttack = 0;
	unit->targetMove = 0;

	return true;
}

/**
 * Remove fog arount the given unit.
 *
 * @param unit The Unit to remove fog around.
 */
void Unit_RemoveFog(Unit *unit)
{
	uint16 fogUncoverRadius;

	if (unit == NULL) return;
	if (unit->o.flags.s.isNotOnMap) return;
	if ((unit->o.position.x == 0xFFFF && unit->o.position.y == 0xFFFF) || (unit->o.position.x == 0 && unit->o.position.y == 0)) return;
	if (!House_AreAllied(Unit_GetHouseID(unit), g_playerHouseID)) return;

	fogUncoverRadius = g_table_unitInfo[unit->o.type].o.fogUncoverRadius;

	if (fogUncoverRadius == 0) return;

	Tile_RemoveFogInRadius(unit->o.position, fogUncoverRadius);
}

/**
 * Deviate the given unit.
 *
 * @param unit The Unit to deviate.
 * @param probability The probability for deviation to succeed.
 * @param houseID House controlling the deviator.
 * @return True if and only if the unit beacame deviated.
 */
bool Unit_Deviate(Unit *unit, uint16 probability, uint8 houseID)
{
	const UnitInfo *ui;

	if (unit == NULL) return false;

	ui = &g_table_unitInfo[unit->o.type];

	if (!ui->flags.isNormalUnit) return false;
	if (unit->deviated != 0) return false;
	if (ui->flags.isNotDeviatable) return false;

	if (probability == 0) probability = g_table_houseInfo[unit->o.houseID].toughness;

	if (unit->o.houseID != g_playerHouseID) {
		probability -= probability / 8;
	}

	if (Tools_Random_256() >= probability) return false;

	unit->deviated = 120;
	unit->deviatedHouse = houseID;

	Unit_UpdateMap(2, unit);

	if (g_playerHouseID == unit->deviatedHouse) {
		Unit_SetAction(unit, ui->o.actionsPlayer[3]);
	} else {
		Unit_SetAction(unit, ui->actionAI);
	}

	Unit_UntargetMe(unit);
	unit->targetAttack = 0;
	unit->targetMove = 0;

	return true;
}

/**
 * Moves the given unit.
 *
 * @param unit The Unit to move.
 * @param distance The maximum distance to pass through.
 * @return ??.
 */
bool Unit_Move(Unit *unit, uint16 distance)
{
	const UnitInfo *ui;
	uint16 d;
	uint16 packed;
	tile32 newPosition;
	bool ret;
	tile32 currentDestination;
	bool isSpiceBloom = false;
	bool isSpecialBloom = false;

	if (unit == NULL || !unit->o.flags.s.used) return false;

	ui = &g_table_unitInfo[unit->o.type];

	newPosition = Tile_MoveByDirection(unit->o.position, unit->orientation[0].current, distance);

	if ((newPosition.x == unit->o.position.x) && (newPosition.y == unit->o.position.y)) return false;

	if (!Tile_IsValid(newPosition)) {
		if (!ui->flags.mustStayInMap) {
			Unit_Remove(unit);
			return true;
		}

		if (unit->o.flags.s.byScenario && unit->o.linkedID == 0xFF && unit->o.script.variables[4] == 0) {
			Unit_Remove(unit);
			return true;
		}

		newPosition = unit->o.position;
		Unit_SetOrientation(unit, unit->orientation[0].current + (Tools_Random_256() & 0xF), false, 0);
	}

	unit->wobbleIndex = 0;
	if (ui->flags.canWobble && unit->o.flags.s.isWobbling) {
		unit->wobbleIndex = Tools_Random_256() & 7;
	}

	d = Tile_GetDistance(newPosition, unit->currentDestination);
	packed = Tile_PackTile(newPosition);

	if (ui->flags.isTracked && d < 48) {
		Unit *u;
		u = Unit_Get_ByPackedTile(packed);

		/* Driving over a foot unit */
		if (u != NULL && g_table_unitInfo[u->o.type].movementType == MOVEMENT_FOOT && u->o.flags.s.allocated) {
			if (UnitSelection_IsSelected(u)) UnitSelection_Remove(u);

			Unit_UntargetMe(u);
			u->o.script.variables[1] = 1;
			if (Skirmish_IsActive()) Skirmish_RecordKill(Unit_GetHouseID(u), true);
			Unit_SetAction(u, ACTION_DIE);
		} else {
			uint16 type = Map_GetLandscapeType(packed);
			/* Produce tracks in the sand */
			if ((type == LST_NORMAL_SAND || type == LST_ENTIRELY_DUNE) && g_map[packed].overlayTileID == 0) {
				uint8 animationID = Orientation_Orientation256ToOrientation8(unit->orientation[0].current);

				assert(animationID < 8);
				Animation_Start(g_table_animation_unitMove[animationID], unit->o.position, 0, unit->o.houseID, 5);
			}
		}
	}

	Unit_UpdateMap(0, unit);

	if (ui->movementType == MOVEMENT_WINGER) {
		unit->o.flags.s.animationFlip = !unit->o.flags.s.animationFlip;
	}

	currentDestination = unit->currentDestination;
	distance = Tile_GetDistance(newPosition, currentDestination);

	if (unit->o.type == UNIT_SONIC_BLAST) {
		Unit *u;
		uint16 damage;

		damage = (unit->o.hitpoints / 4) + 1;
		ret = false;

		u = Unit_Get_ByPackedTile(packed);

		if (u != NULL) {
			if (!g_table_unitInfo[u->o.type].flags.sonicProtection) {
				Unit_Damage(u, damage, 0);
			}
		} else {
			Structure *s;

			s = Structure_Get_ByPackedTile(packed);

			if (s != NULL) {
				/* ENHANCEMENT -- make sonic blast trigger counter attack, but
				 * do not warn about base under attack (original behaviour). */
				if (g_dune2_enhanced && s->o.houseID != g_playerHouseID && !House_AreAllied(unit->o.houseID, s->o.houseID)) {
					Structure_HouseUnderAttack(s->o.houseID);
				}

				Structure_Damage(s, damage, 0);
			} else {
				if (Map_GetLandscapeType(packed) == LST_WALL && g_table_structureInfo[STRUCTURE_WALL].o.hitpoints > damage) Tools_Random_256();
			}
		}

		if (unit->o.hitpoints < (ui->damage / 2)) {
			unit->o.flags.s.bulletIsBig = true;
		}

		if (--unit->o.hitpoints == 0 || unit->fireDelay == 0) {
			Unit_Remove(unit);
		}
	} else {
		if (unit->o.type == UNIT_BULLET) {
			uint16 type = Map_GetLandscapeType(Tile_PackTile(newPosition));
			if (type == LST_WALL || type == LST_STRUCTURE) {
				if (Tools_Index_GetType(unit->originEncoded) == IT_STRUCTURE) {
					if (g_map[Tile_PackTile(newPosition)].houseID == unit->o.houseID) {
						type = LST_NORMAL_SAND;
					}
				}
			}

			if (type == LST_WALL || type == LST_STRUCTURE || type == LST_ENTIRELY_MOUNTAIN) {
				unit->o.position = newPosition;

				Map_MakeExplosion((ui->explosionType + unit->o.hitpoints / 10) & 3, unit->o.position, unit->o.hitpoints, unit->originEncoded);

				Unit_Remove(unit);
				return true;
			}
		}

		ret = (unit->distanceToDestination < distance || distance < 16) ? true : false;

		if (ret) {
			if (ui->flags.isBullet) {
				if (unit->fireDelay == 0 || unit->o.type == UNIT_MISSILE_TURRET) {
					if (unit->o.type == UNIT_MISSILE_HOUSE) {
						uint8 i;

						for (i = 0; i < 17; i++) {
							static const int16 offsetX[17] = { 0, 0, 200, 256, 200, 0, -200, -256, -200, 0, 400, 512, 400, 0, -400, -512, -400 };
							static const int16 offsetY[17] = { 0, -256, -200, 0, 200, 256, 200, 0, -200, -512, -400, 0, 400, 512, 400, 0, -400 };
							tile32 p = newPosition;
							p.y += offsetY[i];
							p.x += offsetX[i];

							if (Tile_IsValid(p)) {
								Map_MakeExplosion(ui->explosionType, p, 200, 0);
							}
						}
					} else if (ui->explosionType != 0xFFFF) {
						if (ui->flags.impactOnSand && g_map[Tile_PackTile(unit->o.position)].index == 0 && Map_GetLandscapeType(Tile_PackTile(unit->o.position)) == LST_NORMAL_SAND) {
							Map_MakeExplosion(EXPLOSION_SAND_BURST, newPosition, unit->o.hitpoints, unit->originEncoded);
						} else if (unit->o.type == UNIT_MISSILE_DEVIATOR) {
							Map_DeviateArea(ui->explosionType, newPosition, 32, unit->o.houseID);
						} else {
							Map_MakeExplosion((ui->explosionType + unit->o.hitpoints / 20) & 3, newPosition, unit->o.hitpoints, unit->originEncoded);
						}
					}

					Unit_Remove(unit);
					return true;
				}
			} else if (ui->flags.isGroundUnit) {
				if (currentDestination.x != 0 || currentDestination.y != 0) newPosition = currentDestination;
				unit->targetPreLast = unit->targetLast;
				unit->targetLast    = unit->o.position;
				unit->currentDestination.x = 0;
				unit->currentDestination.y = 0;

				if (unit->o.flags.s.degrades && (Tools_Random_256() & 3) == 0) {
					Unit_Damage(unit, 1, 0);
				}

				if (unit->o.type == UNIT_SABOTEUR) {
					bool detonate = (Map_GetLandscapeType(Tile_PackTile(newPosition)) == LST_WALL);

					if (!detonate) {
						/* ENHANCEMENT -- Saboteurs tend to forget their goal, depending on terrain and game speed: to blow up on reaching their destination. */
						if (g_dune2_enhanced) {
							detonate = (unit->targetMove != 0 && Tile_GetDistance(newPosition, Tools_Index_GetTile(unit->targetMove)) < 16);
						} else {
							detonate = (unit->targetMove != 0 && Tile_GetDistance(unit->o.position, Tools_Index_GetTile(unit->targetMove)) < 32);
						}
					}
					
					if (detonate) {
						Map_MakeExplosion(EXPLOSION_SABOTEUR_DEATH, newPosition, 500, 0);

						Unit_Remove(unit);
						return true;
					}
				}

				Unit_SetSpeed(unit, 0);

				if (unit->targetMove == Tools_Index_Encode(packed, IT_TILE)) {
					unit->targetMove = 0;
				}

				{
					Structure *s;

					s = Structure_Get_ByPackedTile(packed);
					if (s != NULL) {
						unit->targetPreLast.x = 0;
						unit->targetPreLast.y = 0;
						unit->targetLast.x    = 0;
						unit->targetLast.y    = 0;
						Unit_EnterStructure(unit, s);
						return true;
					}
				}

				if (unit->o.type != UNIT_SANDWORM) {
					if (g_map[packed].groundTileID == g_bloomTileID || g_map[packed].groundTileID == g_bloomTileID + 1) {
						isSpiceBloom = true;
					}
				}
			}
		}
	}

	unit->distanceToDestination = distance;
	unit->o.position = newPosition;

	Unit_UpdateMap(1, unit);

	if (isSpecialBloom) Map_Bloom_ExplodeSpecial(packed, Unit_GetHouseID(unit));
	if (isSpiceBloom) Map_Bloom_ExplodeSpice(packed, Unit_GetHouseID(unit));

	return ret;
}

/**
 * Applies damages to the given unit.
 *
 * @param unit The Unit to apply damages on.
 * @param damage The amount of damage to apply.
 * @param range ??.
 * @return True if and only if the unit has no hitpoints left.
 */
bool Unit_Damage(Unit *unit, uint16 damage, uint16 range)
{
	const UnitInfo *ui;
	bool alive = false;
	uint8 houseID;

	if (unit == NULL || !unit->o.flags.s.allocated) return false;

	ui = &g_table_unitInfo[unit->o.type];

	if (!ui->flags.isNormalUnit && unit->o.type != UNIT_SANDWORM) return false;

	if (unit->o.hitpoints != 0) alive = true;

	if (Skirmish_IsActive()) {
		Skirmish_RecordDamage(Unit_GetHouseID(unit), min(damage, unit->o.hitpoints));
	}

	if (unit->o.hitpoints >= damage) {
		unit->o.hitpoints -= damage;
	} else {
		unit->o.hitpoints = 0;
	}

	Unit_Deviation_Decrease(unit, 0);

	houseID = Unit_GetHouseID(unit);

	if (unit->o.hitpoints == 0) {
		Unit_RemovePlayer(unit);

		if (unit->o.type == UNIT_HARVESTER) Map_FillCircleWithSpice(Tile_PackTile(unit->o.position), unit->amount / 32);

		if (unit->o.type == UNIT_SABOTEUR) {
			Sound_Output_Feedback(20);
		} else {
			if (!ui->o.flags.noMessageOnDeath && alive) {
				Sound_Output_Feedback((houseID == g_playerHouseID || g_campaignID > 3) ? houseID + 14 : 13);
			}
		}

		if (Skirmish_IsActive()) Skirmish_RecordKill(houseID, false);

		Unit_SetAction(unit, ACTION_DIE);
		return true;
	}

	if (range != 0) {
		Map_MakeExplosion((damage < 25) ? EXPLOSION_IMPACT_SMALL : EXPLOSION_IMPACT_MEDIUM, unit->o.position, 0, 0);
	}

	if (houseID != g_playerHouseID && unit->actionID == ACTION_AMBUSH && unit->o.type != UNIT_HARVESTER) {
		Unit_SetAction(unit, ACTION_ATTACK);
	}

	if (unit->o.hitpoints >= ui->o.hitpoints / 2) return false;

	if (unit->o.type == UNIT_SANDWORM) {
		Unit_SetAction(unit, ACTION_DIE);
	}

	if (unit->o.type == UNIT_TROOPERS || unit->o.type == UNIT_INFANTRY) {
		unit->o.type += 2;
		ui = &g_table_unitInfo[unit->o.type];
		unit->o.hitpoints = ui->o.hitpoints;

		Unit_UpdateMap(2, unit);

		if (Tools_Random_256() < g_table_houseInfo[unit->o.houseID].toughness) {
			Unit_SetAction(unit, ACTION_RETREAT);
		}
	}

	if (ui->movementType != MOVEMENT_TRACKED && ui->movementType != MOVEMENT_HARVESTER && ui->movementType != MOVEMENT_WHEELED) return false;

	unit->o.flags.s.isSmoking = true;
	unit->spriteOffset = 0;
	unit->timer = 0;

	return false;
}

/**
 * Untarget the given Unit.
 *
 * @param unit The Unit to untarget.
 */
void Unit_UntargetMe(Unit *unit)
{
	PoolFindStruct find;
	uint16 encoded = Tools_Index_Encode(unit->o.index, IT_UNIT);

	Object_Script_Variable4_Clear(&unit->o);

	find.houseID = HOUSE_INVALID;
	find.type    = 0xFFFF;
	find.index   = 0xFFFF;

	while (true) {
		Unit *u;

		u = Unit_Find(&find);
		if (u == NULL) break;

		if (u->targetMove == encoded) u->targetMove = 0;
		if (u->targetAttack == encoded) u->targetAttack = 0;
		if (u->o.script.variables[4] == encoded) Object_Script_Variable4_Clear(&u->o);
	}

	find.houseID = HOUSE_INVALID;
	find.type    = 0xFFFF;
	find.index   = 0xFFFF;

	while (true) {
		Structure *s;

		s = Structure_Find(&find);
		if (s == NULL) break;

		if (s->o.type != STRUCTURE_TURRET && s->o.type != STRUCTURE_ROCKET_TURRET) continue;
		if (s->o.script.variables[2] == encoded) s->o.script.variables[2] = 0;
	}

	Unit_RemoveFromTeam(unit);

	find.houseID = HOUSE_INVALID;
	find.type    = 0xFFFF;
	find.index   = 0xFFFF;

	while (true) {
		Team *t;

		t = Team_Find(&find);
		if (t == NULL) break;

		if (t->target == encoded) t->target = 0;
	}
}

/**
 * Set the new orientation of the unit.
 *
 * @param unit The Unit to operate on.
 * @param orientation The new orientation of the unit.
 * @param rotateInstantly If true, rotation is instant. Else the unit turns over the next few ticks slowly.
 * @param level 0 = base, 1 = top (turret etc).
 */
void Unit_SetOrientation(Unit *unit, int8 orientation, bool rotateInstantly, uint16 level)
{
	int16 diff;

	assert(level == 0 || level == 1);

	if (unit == NULL) return;

	unit->orientation[level].speed = 0;
	unit->orientation[level].target = orientation;

	if (rotateInstantly) {
		unit->orientation[level].current = orientation;
		return;
	}

	if (unit->orientation[level].current == orientation) return;

	unit->orientation[level].speed = g_table_unitInfo[unit->o.type].turningSpeed * 4;

	diff = orientation - unit->orientation[level].current;

	if ((diff > -128 && diff < 0) || diff > 128) {
		unit->orientation[level].speed = -unit->orientation[level].speed;
	}
}

/** Clear the UI-only group selection and any target command in progress. */
void UnitSelection_Clear(void)
{
	UnitSelection_ClearInternal();
	UnitSelection_CancelPendingAction();
}

/* Remove one member without turning a surviving group into an empty single
 * selection.  Carryalls use this while taking a unit off the map. */
void UnitSelection_Remove(Unit *unit)
{
	uint16 i;
	bool wasPrimary;
	bool removed = false;

	if (unit == NULL) return;
	wasPrimary = (unit == g_unitSelected);
	for (i = 0; i < g_unitSelectionCount; i++) {
		if (s_unitSelection[i] != unit->o.index) continue;
		for (; i + 1 < g_unitSelectionCount; i++) s_unitSelection[i] = s_unitSelection[i + 1];
		g_unitSelectionCount--;
		removed = true;
		break;
	}

	if (!wasPrimary) {
		if (removed) GUI_Widget_ActionPanel_Draw(true);
		return;
	}
	if (g_unitSelectionCount == 0) {
		g_unitSelected = NULL;
		GUI_ChangeSelectionType(SELECTIONTYPE_STRUCTURE);
	} else {
		Unit_Select(Unit_Get_ByIndex(s_unitSelection[0]));
	}
	GUI_Widget_ActionPanel_Draw(true);
}

/* Keep the primary UI selection consistent with the persistent group model.
 * The index set is the source of truth; g_unitSelected only supplies the
 * portrait/status.  This runs at selection-mode boundaries, not per order. */
void UnitSelection_Reconcile(void)
{
	Unit *primary = NULL;
	uint16 i = 0;

	/* Restore the captured group first.  The mutable working selection can be
	 * changed by old widget code during a target command, whereas this snapshot
	 * represents precisely the units that received the order. */
	if (s_unitTargetSelectionActive) {
		UnitSelection_ClearInternal();
		for (i = 0; i < s_unitTargetSelectionCount; i++) {
			Unit *unit = Unit_Get_ByIndex(s_unitTargetSelection[i]);
			if (UnitSelection_IsControllable(unit)) UnitSelection_Add(unit);
		}
		s_unitTargetSelectionActive = false;
		s_unitTargetSelectionCount = 0;
		i = 0;
	}

	while (i < g_unitSelectionCount) {
		Unit *unit = Unit_Get_ByIndex(s_unitSelection[i]);

		if (!UnitSelection_IsControllable(unit)) {
			uint16 j;

			for (j = i; j + 1 < g_unitSelectionCount; j++) s_unitSelection[j] = s_unitSelection[j + 1];
			g_unitSelectionCount--;
			continue;
		}
		if (primary == NULL) primary = unit;
		i++;
	}
	if (primary == NULL) return;
	if (UnitSelection_Contains(g_unitSelected)) return;

	Unit_Select(primary);
}

/* Called when a target modal is abandoned for a non-unit selection mode. */
void UnitSelection_AbortTargeting(void)
{
	s_unitTargetSelectionActive = false;
	s_unitTargetSelectionCount = 0;
}

bool UnitSelection_HasTargetingSnapshot(void)
{
	return s_unitTargetSelectionActive;
}

/* Begin the target-command selection transaction.  Both the group command
 * panel and legacy single-unit panel use this entry point. */
void UnitSelection_BeginTargeting(void)
{
	UnitSelection_CaptureTargetSelection();
}

/** Select exactly one controllable unit. */
void UnitSelection_SelectSingle(Unit *unit)
{
	UnitSelection_ClearInternal();
	UnitSelection_Add(unit);
	Unit_Select(unit);

	GUI_Widget_ActionPanel_Draw(true);
}

/* Take one unit in or out of the group.  Shift could only ever add, so a
 * mis-click could not be corrected without rebuilding the whole group.
 * Returns false when the unit is not ours to command, leaving the caller to
 * fall back on the classic "show me what this is" selection. */
bool UnitSelection_Toggle(Unit *unit)
{
	if (unit == NULL || !UnitSelection_IsControllable(unit)) return false;

	if (UnitSelection_IsSelected(unit)) {
		UnitSelection_Remove(unit);
		return true;
	}

	if (g_unitSelectionCount == 0) {
		UnitSelection_SelectSingle(unit);
		return true;
	}

	UnitSelection_Add(unit);
	GUI_Widget_ActionPanel_Draw(true);
	return true;
}

/* Every unit of one type currently on screen.  Deliberately limited to the
 * viewport: "all my quads" means the ones in this fight, not the harvester
 * escort three screens away. */
bool UnitSelection_SelectSameTypeOnScreen(Unit *unit, bool additive)
{
	uint16 i;

	if (unit == NULL || !UnitSelection_IsControllable(unit)) return false;

	if (!additive) UnitSelection_ClearInternal();

	for (i = 0; i < UNIT_INDEX_MAX; i++) {
		Unit *other = Unit_Get_ByIndex(i);

		if (!UnitSelection_IsControllable(other)) continue;
		if (other->o.type != unit->o.type) continue;
		if (!Map_IsPositionInViewport(other->o.position, NULL, NULL)) continue;
		UnitSelection_Add(other);
	}

	UnitSelection_Add(unit);
	Unit_Select(unit);
	GUI_Widget_ActionPanel_Draw(true);
	return true;
}

/**
 * Select player-controlled units inside a rectangle described in map tiles.
 * The caller deliberately supplies packed map coordinates rather than pixels,
 * which keeps this selection stable when the viewport scrolls.
 */
void UnitSelection_SelectBox(uint16 packedA, uint16 packedB, bool additive)
{
	uint16 minX = min(Tile_GetPackedX(packedA), Tile_GetPackedX(packedB));
	uint16 maxX = max(Tile_GetPackedX(packedA), Tile_GetPackedX(packedB));
	uint16 minY = min(Tile_GetPackedY(packedA), Tile_GetPackedY(packedB));
	uint16 maxY = max(Tile_GetPackedY(packedA), Tile_GetPackedY(packedB));
	Unit *primary = NULL;
	Structure *selectedStructure = NULL;
	uint16 i;

	if (!additive) UnitSelection_ClearInternal();

	for (i = 0; i < UNIT_INDEX_MAX; i++) {
		Unit *unit = Unit_Get_ByIndex(i);
		uint16 x;
		uint16 y;

		if (!UnitSelection_IsControllable(unit)) continue;

		x = Tile_GetPackedX(Tile_PackTile(unit->o.position));
		y = Tile_GetPackedY(Tile_PackTile(unit->o.position));
		if (x < minX || x > maxX || y < minY || y > maxY) continue;

		UnitSelection_Add(unit);
	}

	if (g_unitSelectionCount != 0) {
		primary = Unit_Get_ByIndex(s_unitSelection[0]);
	} else if (!additive) {
		/* A box is allowed to select one building.  Test its complete footprint,
		 * so dragging over any visible part of a 2x2/3x3 structure works. */
		PoolFindStruct find;

		find.houseID = HOUSE_INVALID;
		find.type = 0xFFFF;
		find.index = 0xFFFF;
		while (true) {
			Structure *s = Structure_Find(&find);
			const XYSize *size;
			uint16 x;
			uint16 y;

			if (s == NULL) break;
			if (s->o.flags.s.isNotOnMap) continue;
			size = &g_table_structure_layoutSize[g_table_structureInfo[s->o.type].layout];
			x = Tile_GetPackedX(Tile_PackTile(s->o.position));
			y = Tile_GetPackedY(Tile_PackTile(s->o.position));
			if (x + size->width - 1 < minX || x > maxX || y + size->height - 1 < minY || y > maxY) continue;
			if (selectedStructure != NULL) {
				selectedStructure = NULL;
				break;
			}
			selectedStructure = s;
		}
	}

	Unit_Select(primary);
	if (selectedStructure != NULL) Map_SetSelection(Tile_PackTile(selectedStructure->o.position));

	GUI_Widget_ActionPanel_Draw(true);
}

/** Return whether this unit is part of the visible group selection. */
bool UnitSelection_IsSelected(const Unit *unit)
{
	return unit != NULL && (unit == g_unitSelected || UnitSelection_Contains(unit));
}

/* Noninteractive regression test used by --selection-self-test.  It runs on
 * real units loaded from a save and checks the exact failure mode that used to
 * collapse groups after M/A: changing the primary display unit, issuing a
 * targeted Move, and issuing a targeted Attack must all preserve membership. */
int UnitSelection_RunRegressionTest(void)
{
	uint16 expected[UNIT_SELECTION_MAX];
	PoolFindStruct find;
	Unit *primary = NULL;
	uint16 expectedCount = 0;
	uint16 i;

	UnitSelection_ClearInternal();
	find.houseID = HOUSE_INVALID;
	find.type = 0xFFFF;
	find.index = 0xFFFF;
	while (expectedCount < UNIT_SELECTION_MAX) {
		Unit *unit = Unit_Find(&find);

		if (unit == NULL) break;
		if (!UnitSelection_IsControllable(unit)) continue;
		UnitSelection_Add(unit);
		expected[expectedCount++] = unit->o.index;
		if (primary == NULL && UnitSelection_UnitHasAction(unit, ACTION_MOVE) && UnitSelection_UnitHasAction(unit, ACTION_ATTACK)) primary = unit;
	}
	if (expectedCount < 2 || primary == NULL) return -1;

	Unit_Select(primary);
	if (g_unitSelectionCount != expectedCount) return 0;
	for (i = 0; i < expectedCount; i++) {
		if (!UnitSelection_Contains(Unit_Get_ByIndex(expected[i]))) return 0;
	}

	/* Legacy callers may update or clear the portrait/status primary at any
	 * time.  Neither operation is allowed to mutate persistent membership. */
	Unit_Select(Unit_Get_ByIndex(expected[expectedCount - 1]));
	Unit_Select(NULL);
	Unit_Select(primary);
	if (g_unitSelectionCount != expectedCount) return 0;
	for (i = 0; i < expectedCount; i++) {
		if (!UnitSelection_Contains(Unit_Get_ByIndex(expected[i]))) return 0;
	}

	for (i = 0; i < 2; i++) {
		ActionType action = i == 0 ? ACTION_MOVE : ACTION_ATTACK;
		Widget viewport;
		uint16 j;

		if (!UnitSelection_BeginAction(action)) return 0;
		g_unitActive = g_unitSelected;
		g_activeAction = action;
		GUI_ChangeSelectionType(SELECTIONTYPE_TARGET);

		/* Exercise the real mouse lifecycle.  Before the release-tail guard was
		 * added, the one-pixel drag below started a fresh box after the target
		 * press had returned to UNIT mode and cleared the whole group. */
		memset(&viewport, 0, sizeof(viewport));
		viewport.index = 43;
		g_mouseClickX = 32;
		g_mouseClickY = 72;
		g_mouseX = 32;
		g_mouseY = 72;
		viewport.state.buttonState = 0x01;
		GUI_Widget_Viewport_Click(&viewport);
		g_mouseX = 33;
		g_mouseY = 73;
		viewport.state.buttonState = 0x02;
		GUI_Widget_Viewport_Click(&viewport);
		viewport.state.buttonState = 0x04;
		GUI_Widget_Viewport_Click(&viewport);
		if (g_unitSelectionCount != expectedCount) return 0;
		for (j = 0; j < expectedCount; j++) {
			if (!UnitSelection_Contains(Unit_Get_ByIndex(expected[j]))) return 0;
		}
	}

	/* Shift on a unit that is already in the group takes it out again. */
	UnitSelection_SelectSingle(primary);
	if (!UnitSelection_Toggle(primary) || UnitSelection_Contains(primary)) return 0;
	if (!UnitSelection_Toggle(primary) || !UnitSelection_Contains(primary)) return 0;

	/* A digit holds the group it was given and hands it back unchanged. */
	UnitSelection_ClearControlGroups();
	UnitSelection_ClearInternal();
	for (i = 0; i < expectedCount; i++) UnitSelection_Add(Unit_Get_ByIndex(expected[i]));
	UnitSelection_AssignControlGroup(3);
	UnitSelection_SelectSingle(primary);
	if (g_unitSelectionCount != 1) return 0;
	if (!UnitSelection_RecallControlGroup(3)) return 0;
	if (g_unitSelectionCount != expectedCount) return 0;
	for (i = 0; i < expectedCount; i++) {
		if (!UnitSelection_Contains(Unit_Get_ByIndex(expected[i]))) return 0;
	}
	/* An unbound digit is not allowed to disturb the current selection. */
	if (UnitSelection_RecallControlGroup(4) || g_unitSelectionCount != expectedCount) return 0;

	/* Double click takes every unit of that type on screen.  Driven through the
	 * real click lifecycle on purpose: the selection function itself was never
	 * the broken part, the pairing in the click path was - it was measured on
	 * the game timer, which runs at double rate in Fast mode.  A headless run
	 * cannot check the width of that window (GUI time barely advances here), so
	 * what this pins down is that a second click on the same unit pairs at all,
	 * and that a first one still selects exactly that unit. */
	{
		Widget viewport;
		uint16 packed = Tile_PackTile(primary->o.position);
		uint16 sameType = 0;
		int16 x;
		int16 y;

		/* Start from "nothing selected", which is how the player meets a unit:
		 * the first click has to change the selection mode as well as select. */
		UnitSelection_Clear();
		GUI_ChangeSelectionType(SELECTIONTYPE_STRUCTURE);
		Map_SetViewportPosition(packed);
		/* The draw loop is what normally copies this over; there is none here. */
		g_minimapPosition = g_viewportPosition;

		x = ((int16)Tile_GetPackedX(packed) - (int16)Tile_GetPackedX(g_minimapPosition)) * 16 + 8;
		y = ((int16)Tile_GetPackedY(packed) - (int16)Tile_GetPackedY(g_minimapPosition)) * 16 + 48;
		if (x < 0 || x > 239 || y < 40 || y > 199) return 0;

		for (i = 0; i < UNIT_INDEX_MAX; i++) {
			Unit *other = Unit_Get_ByIndex(i);

			if (!UnitSelection_IsControllable(other)) continue;
			if (other->o.type != primary->o.type) continue;
			if (!Map_IsPositionInViewport(other->o.position, NULL, NULL)) continue;
			sameType++;
		}
		if (sameType == 0) return 0;

		memset(&viewport, 0, sizeof(viewport));
		viewport.index = 43;
		g_mouseClickX = x;
		g_mouseClickY = y;
		g_mouseX = x;
		g_mouseY = y;

		/* Three deliveries of the same gesture: complete, and with either event
		 * of the second click dropped.  The interface rebuild triggered by the
		 * first click on a fresh unit can swallow one, which is what made the
		 * double click need a third click in the game. */
		for (i = 0; i < 3; i++) {
			/* Let the pairing window lapse between runs, so each starts clean.
			 * Nothing else advances GUI time in a headless run. */
			g_timerGUI += 64;

			viewport.state.buttonState = 0x01;
			GUI_Widget_Viewport_Click(&viewport);
			viewport.state.buttonState = 0x04;
			GUI_Widget_Viewport_Click(&viewport);
			if (g_unitSelectionCount != 1 || !UnitSelection_Contains(primary)) return 0;

			if (i != 2) {
				viewport.state.buttonState = 0x01;
				GUI_Widget_Viewport_Click(&viewport);
			}
			if (i != 1) {
				viewport.state.buttonState = 0x04;
				GUI_Widget_Viewport_Click(&viewport);
			}
			if (g_unitSelectionCount != sameType || !UnitSelection_Contains(primary)) return 0;
		}
	}

	return 1;
}

/** Return how many selected units can execute a command category. */
uint16 UnitSelection_GetActionCount(ActionType action)
{
	uint16 count = 0;
	uint16 i;

	for (i = 0; i < g_unitSelectionCount; i++) {
		Unit *unit = Unit_Get_ByIndex(s_unitSelection[i]);
		if (action == ACTION_MAX) {
			if (UnitSelection_GetUnitSpecialAction(unit) != ACTION_INVALID) count++;
		} else if (UnitSelection_UnitHasAction(unit, action)) {
			count++;
		}
	}

	return count;
}

/** Map the eight compact group-command rows to their command categories. */
/* Translation between a unit's command table and the player's action panel.
 * The table's Guard is offered as Area Guard: plain Guard is what a unit falls
 * back to by itself after a Move, so the command is worth more to the player as
 * the wide defensive mode.  Shift asks for the narrow one, the same way it asks
 * for Ambush behind Attack.  The keyboard shortcut stays with the table entry,
 * so the command is still G and does not collide with Attack. */
ActionType UnitSelection_GetPanelAction(ActionType action, bool narrow)
{
	if (action == ACTION_GUARD) return narrow ? ACTION_GUARD : ACTION_AREA_GUARD;

	return action;
}

ActionType UnitSelection_GetActionForSlot(uint16 slot)
{
	static const ActionType actions[] = {
		ACTION_ATTACK, ACTION_MOVE, ACTION_RETREAT, ACTION_GUARD,
		ACTION_HARVEST, ACTION_RETURN, ACTION_STOP, ACTION_MAX
	};

	if (slot >= sizeof(actions) / sizeof(actions[0])) return ACTION_INVALID;
	return UnitSelection_GetActionCount(actions[slot]) == 0 ? ACTION_INVALID : actions[slot];
}

/** Return a single special action only when the group has one unambiguous kind. */
ActionType UnitSelection_GetSpecialAction(void)
{
	ActionType action = ACTION_INVALID;
	uint16 i;

	for (i = 0; i < g_unitSelectionCount; i++) {
		ActionType special = UnitSelection_GetUnitSpecialAction(Unit_Get_ByIndex(s_unitSelection[i]));
		if (special == ACTION_INVALID) continue;
		if (action != ACTION_INVALID && action != special) return ACTION_INVALID;
		action = special;
	}

	return action;
}

/** Return the most useful targeted order for one selected unit. */
static ActionType UnitSelection_GetUnitDefaultAction(const Unit *unit)
{
	static const ActionType defaults[] = { ACTION_MOVE, ACTION_HARVEST, ACTION_ATTACK };
	uint16 i;

	for (i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++) {
		if (UnitSelection_UnitHasAction(unit, defaults[i])) return defaults[i];
	}

	return ACTION_INVALID;
}

/* A right click on something hostile is an attack, on anything else a move.
 * The tile is taken exactly, like the Attack order: missing an enemy by one
 * tile should order a march, never a shot at whatever stands there. */
static bool UnitSelection_IsHostileTarget(uint16 packed)
{
	Unit *unit;
	const Structure *s;

	if (!Map_IsValidPosition(packed)) return false;
	if (g_map[packed].overlayTileID == g_veiledTileID && !g_debugScenario) return false;

	unit = Unit_Get_ByPackedTile(packed);
	if (unit != NULL) return !House_AreAllied(Unit_GetHouseID(unit), g_playerHouseID);

	s = Structure_Get_ByPackedTile(packed);
	if (s != NULL) return !House_AreAllied(s->o.houseID, g_playerHouseID);

	return false;
}

/** Issue the most useful targeted order available to each selected unit. */
void UnitSelection_IssueDefaultOrder(uint16 packed)
{
	uint16 order[UNIT_SELECTION_MAX];
	uint16 count = 0;
	bool hostile = UnitSelection_IsHostileTarget(packed);
	uint16 i;

	UnitSelection_CancelPendingAction();

	/* Sorted on a copy: the order tiles are handed out in is a property of this
	 * one command, not of the group. */
	for (i = 0; i < g_unitSelectionCount; i++) order[count++] = s_unitSelection[i];
	UnitSelection_SortOrderByDistance(order, count, packed);
	UnitSelection_SpreadReset();

	for (i = 0; i < count; i++) {
		Unit *unit = Unit_Get_ByIndex(order[i]);
		ActionType action;

		/* Everything that can shoot attacks; a harvester right-clicked onto an
		 * enemy still does the only thing it can, which is drive there. */
		if (hostile && UnitSelection_UnitHasAction(unit, ACTION_ATTACK)) {
			action = ACTION_ATTACK;
		} else {
			action = UnitSelection_GetUnitDefaultAction(unit);
		}

		if (action == ACTION_INVALID) continue;
		UnitSelection_ResetOrder(unit, action, action == ACTION_MOVE ? UnitSelection_SpreadTake(unit, packed) : packed);
	}
}

/* Control groups.  Session state on purpose: they are a property of how the
 * player is holding the mouse right now, not of the battlefield, and keeping
 * them out of the save format avoids touching it for a convenience. */
#define UNIT_CONTROL_GROUPS 10

static uint16 s_controlGroup[UNIT_CONTROL_GROUPS][UNIT_SELECTION_MAX];
static uint8 s_controlGroupType[UNIT_CONTROL_GROUPS][UNIT_SELECTION_MAX];
static uint16 s_controlGroupCount[UNIT_CONTROL_GROUPS];
/* A digit holds either a group of units or one building - the game never has
 * both selected at once, so one slot each is enough. */
static uint16 s_controlGroupStructure[UNIT_CONTROL_GROUPS] = {
	0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF
};
static uint8 s_controlGroupStructureType[UNIT_CONTROL_GROUPS];

/* Pool indices are reused between scenarios, so a group left over from the last
 * one would name whatever now occupies those slots. */
void UnitSelection_ClearControlGroups(void)
{
	uint16 i;

	for (i = 0; i < UNIT_CONTROL_GROUPS; i++) {
		s_controlGroupCount[i] = 0;
		s_controlGroupStructure[i] = 0xFFFF;
	}
}

/** Bind the current selection to a digit. An empty selection clears the group. */
void UnitSelection_AssignControlGroup(uint16 group)
{
	uint16 i;

	if (group >= UNIT_CONTROL_GROUPS) return;

	s_controlGroupCount[group] = 0;
	s_controlGroupStructure[group] = 0xFFFF;

	if (g_unitSelectionCount == 0) {
		Structure *s = Structure_Get_ByPackedTile(g_selectionPosition);

		if (s != NULL && s->o.houseID == g_playerHouseID) {
			s_controlGroupStructure[group] = s->o.index;
			s_controlGroupStructureType[group] = (uint8)s->o.type;
		}
		return;
	}

	for (i = 0; i < g_unitSelectionCount; i++) {
		Unit *unit = Unit_Get_ByIndex(s_unitSelection[i]);
		uint16 slot;

		if (!UnitSelection_IsControllable(unit)) continue;
		slot = s_controlGroupCount[group]++;
		s_controlGroup[group][slot] = unit->o.index;
		/* The pool reuses the index of a dead unit, so the type is stored as a
		 * cheap check that the recalled unit is still the same one. */
		s_controlGroupType[group][slot] = (uint8)unit->o.type;
	}
}

/** Select a bound group again, skipping whatever has died since. */
bool UnitSelection_RecallControlGroup(uint16 group)
{
	Unit *primary = NULL;
	uint16 i;

	if (group >= UNIT_CONTROL_GROUPS) return false;

	if (s_controlGroupStructure[group] != 0xFFFF) {
		Structure *s = Structure_Get_ByIndex(s_controlGroupStructure[group]);

		if (s == NULL || !s->o.flags.s.used || s->o.type != s_controlGroupStructureType[group]) return false;
		if (s->o.houseID != g_playerHouseID) return false;

		/* Map_SetSelection() does the rest: it drops the unit selection and
		 * switches the interface into structure mode by itself. */
		Map_SetSelection(Tile_PackTile(s->o.position));
		return true;
	}

	if (s_controlGroupCount[group] == 0) return false;

	UnitSelection_ClearInternal();
	for (i = 0; i < s_controlGroupCount[group]; i++) {
		Unit *unit = Unit_Get_ByIndex(s_controlGroup[group][i]);

		if (!UnitSelection_IsControllable(unit)) continue;
		if (unit->o.type != s_controlGroupType[group][i]) continue;
		UnitSelection_Add(unit);
		if (primary == NULL) primary = unit;
	}

	if (primary == NULL) return false;
	Unit_Select(primary);
	GUI_Widget_ActionPanel_Draw(true);
	return true;
}

/* Hunt is intentionally a keyboard-only advanced order: it applies only to
 * normal combat units, leaving harvesters and special units untouched. */
void UnitSelection_OrderHunt(void)
{
	uint16 i;

	UnitSelection_CancelPendingAction();
	for (i = 0; i < g_unitSelectionCount; i++) {
		Unit *unit = Unit_Get_ByIndex(s_unitSelection[i]);

		if (!UnitSelection_UnitHasAction(unit, ACTION_ATTACK)) continue;
		Unit_SetManualHunt(unit, false);
		Unit_AttackPosition_SetManual(unit, false);
		Object_Script_Variable4_Clear(&unit->o);
		unit->targetAttack = 0;
		unit->targetMove = 0;
		unit->route[0] = 0xFF;
		/* Keep the stable Area Guard script alive.  The manual-hunt flag makes
		 * autonomous target search global without loading legacy Hunt code. */
		Unit_SetAction(unit, ACTION_AREA_GUARD);
		Unit_SetManualHunt(unit, true);
	}
	GUI_Widget_ActionPanel_Draw(true);
}

static bool UnitSelection_CanAirTransit(const Unit *unit)
{
	const UnitInfo *ui;

	if (!UnitSelection_IsControllable(unit)) return false;
	ui = &g_table_unitInfo[unit->o.type];
	return ui->o.flags.canBePickedUp && ui->flags.isGroundUnit;
}

/* Begin a targeted carryall command.  Units without an immediately free
 * carryall remain queued and are picked up as transport becomes available. */
bool UnitSelection_BeginAirTransit(void)
{
	uint16 i;

	UnitSelection_CancelPendingAction();
	for (i = 0; i < g_unitSelectionCount; i++) {
		Unit *unit = Unit_Get_ByIndex(s_unitSelection[i]);
		if (UnitSelection_CanAirTransit(unit)) s_unitOrder[s_unitOrderCount++] = unit->o.index;
	}
	if (s_unitOrderCount == 0) return false;
	UnitSelection_CaptureTargetSelection();
	s_unitOrderAirTransit = true;
	return true;
}

/** Begin a group command. Returns true when the next map click is its target. */
bool UnitSelection_BeginAction(ActionType action)
{
	uint16 i;

	UnitSelection_CancelPendingAction();

	if (action == ACTION_INVALID || UnitSelection_GetActionCount(action) == 0) return false;

	if (action != ACTION_MAX && g_table_actionInfo[action].selectionType == SELECTIONTYPE_TARGET) {
		for (i = 0; i < g_unitSelectionCount; i++) {
			Unit *unit = Unit_Get_ByIndex(s_unitSelection[i]);
			if (UnitSelection_UnitHasAction(unit, action)) s_unitOrder[s_unitOrderCount++] = unit->o.index;
		}
		UnitSelection_CaptureTargetSelection();
		s_unitOrderAction = action;
		return s_unitOrderCount != 0;
	}

	for (i = 0; i < g_unitSelectionCount; i++) {
		Unit *unit = Unit_Get_ByIndex(s_unitSelection[i]);
		ActionType unitAction = (action == ACTION_MAX) ? UnitSelection_GetUnitSpecialAction(unit) : action;
		if (unitAction == ACTION_INVALID || !UnitSelection_UnitHasAction(unit, unitAction)) continue;

		Object_Script_Variable4_Clear(&unit->o);
		Unit_BeginManualOrder(unit);
		Unit_SetManualHunt(unit, false);
		Unit_Harvester_BeginOrder(unit, unitAction);
		unit->targetAttack = 0;
		unit->targetMove = 0;
		unit->route[0] = 0xFF;
		if (unitAction == ACTION_GUARD || unitAction == ACTION_AREA_GUARD) {
			Unit_SetGuardPosition(unit, Tile_PackTile(unit->o.position));
			Unit_SetGuardAction(unit, unitAction);
		}
		Unit_SetAction(unit, unitAction);
	}

	GUI_Widget_ActionPanel_Draw(true);
	return false;
}

bool UnitSelection_HasPendingAction(void)
{
	return (s_unitOrderAction != ACTION_INVALID || s_unitOrderAirTransit) && s_unitOrderCount != 0;
}

/** Apply the pending target action to the recipients captured at command time. */
void UnitSelection_ApplyPendingAction(uint16 packed)
{
	uint16 i;
	bool advance;
	bool spread;

	if (!UnitSelection_HasPendingAction()) return;
	if (s_unitOrderAirTransit) {
		for (i = 0; i < s_unitOrderCount; i++) {
			Unit *unit = Unit_Get_ByIndex(s_unitOrder[i]);

			if (!UnitSelection_CanAirTransit(unit)) continue;
			Unit_SetManualHunt(unit, false);
			Unit_AttackPosition_SetManual(unit, false);
			Unit_Harvester_BeginOrder(unit, ACTION_INVALID);
			Object_Script_Variable4_Clear(&unit->o);
			unit->targetAttack = 0;
			unit->targetMove = 0;
			unit->route[0] = 0xFF;
			Unit_Autonomy_ClearPost(unit);
			unit->airTransitDestination = packed;
			/* Do not switch to ACTION_STOP here.  On a moving combat unit that
			 * re-enters the legacy action script as a subroutine and can overflow
			 * its tiny script stack.  The carryall request below is independent of
			 * the unit's current action and will pick it up safely. */
		}
		UnitSelection_CancelPendingAction();
		GUI_Widget_ActionPanel_Draw(true);
		return;
	}

	/* A Move, and an Attack that turns out to be an advance, place the group on
	 * the ground: every recipient needs a tile of its own.  An Attack on a real
	 * target is the opposite - the whole group shoots at the same thing, and the
	 * decision is taken once here, not per tile, because a handed-out tile can
	 * land next to a unit and would read as an ordinary attack order. */
	advance = s_unitOrderAction == ACTION_ATTACK && UnitSelection_IsAdvanceTarget(packed);
	spread = advance || s_unitOrderAction == ACTION_MOVE;

	if (spread) {
		UnitSelection_SortOrderByDistance(s_unitOrder, s_unitOrderCount, packed);
		UnitSelection_SpreadReset();
	}

	for (i = 0; i < s_unitOrderCount; i++) {
		Unit *unit = Unit_Get_ByIndex(s_unitOrder[i]);

		if (!UnitSelection_UnitHasAction(unit, s_unitOrderAction)) continue;
		if (advance) {
			UnitSelection_BeginAdvance(unit, UnitSelection_SpreadTake(unit, packed));
		} else {
			UnitSelection_ResetOrder(unit, s_unitOrderAction, spread ? UnitSelection_SpreadTake(unit, packed) : packed);
		}
	}

	UnitSelection_CancelPendingAction();
}

void UnitSelection_CancelPendingAction(void)
{
	s_unitOrderCount = 0;
	s_unitOrderAction = ACTION_INVALID;
	s_unitOrderAirTransit = false;
}

/**
 * Select the primary unit displayed by the UI.
 *
 * Group membership is owned exclusively by UnitSelection_SelectSingle(),
 * UnitSelection_SelectBox(), UnitSelection_Remove(), and UnitSelection_Clear().
 * Keeping it out of this legacy display function prevents unrelated status
 * updates and mode transitions from collapsing a group to one unit.
 *
 * @param unit The Unit to display as primary.
 */
void Unit_Select(Unit *unit)
{
	if (unit != NULL && !unit->o.flags.s.allocated && !g_debugGame) {
		unit = NULL;
	}

	if (unit != NULL && (unit->o.seenByHouses & (1 << g_playerHouseID)) == 0 && !g_debugGame) {
		unit = NULL;
	}

	if (unit == g_unitSelected) {
		GUI_Widget_ActionPanel_Draw(true);
		return;
	}

	if (g_unitSelected != NULL) Unit_UpdateMap(2, g_unitSelected);

	if (unit == NULL) {
		g_unitSelected = NULL;

		GUI_ChangeSelectionType(SELECTIONTYPE_STRUCTURE);
		return;
	}

	if (Unit_GetHouseID(unit) == g_playerHouseID) {
		const UnitInfo *ui;

		ui = &g_table_unitInfo[unit->o.type];

		/* Plays the 'reporting' sound file. */
		Sound_StartSound(ui->movementType == MOVEMENT_FOOT ? 18 : 19);

		GUI_DisplayHint(ui->o.hintStringID, ui->o.spriteID);
	}

	if (g_unitSelected != NULL) {
		if (g_unitSelected != unit) Unit_DisplayStatusText(unit);

		g_unitSelected = unit;

		GUI_Widget_ActionPanel_Draw(true);
	} else {
		Unit_DisplayStatusText(unit);
		g_unitSelected = unit;

		GUI_ChangeSelectionType(SELECTIONTYPE_UNIT);
	}

	Unit_UpdateMap(2, g_unitSelected);

	Map_SetSelectionObjectPosition(0xFFFF);
}

/**
 * Create a unit (and a carryall if needed).
 *
 * @param houseID The House of the new Unit.
 * @param typeID The type of the new Unit.
 * @param destination To where on the map this Unit should move.
 * @return The new created Unit, or NULL if something failed.
 */
Unit *Unit_CreateWrapper(uint8 houseID, UnitType typeID, uint16 destination)
{
	tile32 tile;
	House *h;
	int8 orientation;
	Unit *unit;
	Unit *carryall;

	tile = Tile_UnpackTile(Map_FindLocationTile(Tools_Random_256() & 3, houseID));

	h = House_Get_ByIndex(houseID);

	{
		tile32 t;
		t.x = 0x2000;
		t.y = 0x2000;
		orientation = Tile_GetDirection(tile, t);
	}

	if (g_table_unitInfo[typeID].movementType == MOVEMENT_WINGER) {
		g_validateStrictIfZero++;
		unit = Unit_Create(UNIT_INDEX_INVALID, typeID, houseID, tile, orientation);
		g_validateStrictIfZero--;

		if (unit == NULL) return NULL;

		unit->o.flags.s.byScenario = true;

		if (destination != 0) {
			Unit_SetDestination(unit, destination);
		}

		return unit;
	}

	g_validateStrictIfZero++;
	carryall = Unit_Create(UNIT_INDEX_INVALID, UNIT_CARRYALL, houseID, tile, orientation);
	g_validateStrictIfZero--;

	if (carryall == NULL) {
		if (typeID == UNIT_HARVESTER && h->harvestersIncoming == 0) h->harvestersIncoming++;
		return NULL;
	}

	if (House_AreAllied(houseID, g_playerHouseID) || Unit_IsTypeOnMap(houseID, UNIT_CARRYALL)) {
		carryall->o.flags.s.byScenario = true;
	}

	tile.x = 0xFFFF;
	tile.y = 0xFFFF;

	g_validateStrictIfZero++;
	unit = Unit_Create(UNIT_INDEX_INVALID, typeID, houseID, tile, 0);
	g_validateStrictIfZero--;

	if (unit == NULL) {
		Unit_Remove(carryall);
		if (typeID == UNIT_HARVESTER && h->harvestersIncoming == 0) h->harvestersIncoming++;
		return NULL;
	}

	carryall->o.flags.s.inTransport = true;
	carryall->o.linkedID = unit->o.index & 0xFF;
	if (typeID == UNIT_HARVESTER) unit->amount = 1;

	if (destination != 0) {
		Unit_SetDestination(carryall, destination);
	}

	return unit;
}

/**
 * Find a target around the given packed tile.
 *
 * @param packed The packed tile around where to look.
 * @return A packed tile where a Unit/Structure is, or the given packed tile if nothing found.
 */
uint16 Unit_FindTargetAround(uint16 packed)
{
	static const int16 around[] = {0, -1, 1, -64, 64, -65, -63, 65, 63};

	uint8 i;

	if (g_selectionType == SELECTIONTYPE_PLACE) return packed;

	if (Structure_Get_ByPackedTile(packed) != NULL) return packed;

	if (Map_GetLandscapeType(packed) == LST_BLOOM_FIELD) return packed;

	for (i = 0; i < lengthof(around); i++) {
		Unit *u;

		u = Unit_Get_ByPackedTile(packed + around[i]);
		if (u == NULL) continue;

		return Tile_PackTile(u->o.position);
	}

	return packed;
}

/**
 * Check if the position the unit is on is already occupied.
 *
 * @param unit The Unit to operate on.
 * @return True if and only if the position of the unit is already occupied.
 */
bool Unit_IsTileOccupied(Unit *unit)
{
	const UnitInfo *ui;
	uint16 packed;
	Unit *unit2;
	uint16 speed;

	if (unit == NULL) return true;

	ui = &g_table_unitInfo[unit->o.type];
	packed = Tile_PackTile(unit->o.position);

	speed = g_table_landscapeInfo[Map_GetLandscapeType(packed)].movementSpeed[ui->movementType];
	if (speed == 0) return true;

	if (unit->o.type == UNIT_SANDWORM || ui->movementType == MOVEMENT_WINGER) return false;

	unit2 = Unit_Get_ByPackedTile(packed);
	if (unit2 != NULL && unit2 != unit) {
		if (House_AreAllied(Unit_GetHouseID(unit2), Unit_GetHouseID(unit))) return true;
		if (ui->movementType != MOVEMENT_TRACKED) return true;
		if (g_table_unitInfo[unit2->o.type].movementType != MOVEMENT_FOOT) return true;
	}

	return (Structure_Get_ByPackedTile(packed) != NULL);
}

/**
 * Set the speed of a Unit.
 *
 * @param unit The Unit to operate on.
 * @param speed The new speed of the unit (a percent value between 0 and 255).
 */
void Unit_SetSpeed(Unit *unit, uint16 speed)
{
	uint16 speedPerTick;

	assert(unit != NULL);

	speedPerTick = 0;

	unit->speed          = 0;
	unit->speedRemainder = 0;
	unit->speedPerTick   = 0;

	if (unit->o.type == UNIT_HARVESTER) {
		speed = ((255 - unit->amount) * speed) / 256;
	}

	if (speed == 0 || speed >= 256) {
		unit->movingSpeed = 0;
		return;
	}

	unit->movingSpeed = speed & 0xFF;
	speed = g_table_unitInfo[unit->o.type].movingSpeedFactor * speed / 256;

	/* Units in the air don't feel the effect of gameSpeed */
	if (g_table_unitInfo[unit->o.type].movementType != MOVEMENT_WINGER) {
		speed = Tools_AdjustToGameSpeed(speed, 1, 255, false);
	}

	speedPerTick = speed << 4;
	speed        = speed >> 4;

	if (speed != 0) {
		speedPerTick = 255;
	} else {
		speed = 1;
	}

	unit->speed = speed & 0xFF;
	unit->speedPerTick = speedPerTick & 0xFF;
}

/**
 * Create a new bullet Unit.
 *
 * @param position Where on the map this bullet Unit is created.
 * @param typeID The type of the new bullet Unit.
 * @param houseID The House of the new bullet Unit.
 * @param damage The hitpoints of the new bullet Unit.
 * @param target The target of the new bullet Unit.
 * @return The new created Unit, or NULL if something failed.
 */
Unit *Unit_CreateBullet(tile32 position, UnitType type, uint8 houseID, uint16 damage, uint16 target)
{
	const UnitInfo *ui;
	tile32 tile;

	if (!Tools_Index_IsValid(target)) return NULL;

	ui = &g_table_unitInfo[type];
	tile = Tools_Index_GetTile(target);

	switch (type) {
		case UNIT_MISSILE_HOUSE:
		case UNIT_MISSILE_ROCKET:
		case UNIT_MISSILE_TURRET:
		case UNIT_MISSILE_DEVIATOR:
		case UNIT_MISSILE_TROOPER: {
			int8 orientation;
			Unit *bullet;
			Unit *u;

			orientation = Tile_GetDirection(position, tile);

			bullet = Unit_Create(UNIT_INDEX_INVALID, type, houseID, position, orientation);
			if (bullet == NULL) return NULL;

			Voice_PlayAtTile(ui->bulletSound, position);

			bullet->targetAttack = target;
			bullet->o.hitpoints = damage;
			bullet->currentDestination = tile;

			if (ui->flags.notAccurate) {
				bullet->currentDestination = Tile_MoveByRandom(tile, (Tools_Random_256() & 0xF) != 0 ? Tile_GetDistance(position, tile) / 256 + 8 : Tools_Random_256() + 8, false);
			}

			bullet->fireDelay = ui->fireDistance & 0xFF;

			u = Tools_Index_GetUnit(target);
			if (u != NULL && g_table_unitInfo[u->o.type].movementType == MOVEMENT_WINGER) {
				bullet->fireDelay <<= 1;
			}

			if (type == UNIT_MISSILE_HOUSE || (bullet->o.seenByHouses & (1 << g_playerHouseID)) != 0) return bullet;

			Tile_RemoveFogInRadius(bullet->o.position, 2);

			return bullet;
		}

		case UNIT_BULLET:
		case UNIT_SONIC_BLAST: {
			int8 orientation;
			tile32 t;
			Unit *bullet;

			orientation = Tile_GetDirection(position, tile);

			t = Tile_MoveByDirection(Tile_MoveByDirection(position, 0, 32), orientation, 128);

			bullet = Unit_Create(UNIT_INDEX_INVALID, type, houseID, t, orientation);
			if (bullet == NULL) return NULL;

			if (type == UNIT_SONIC_BLAST) {
				bullet->fireDelay = ui->fireDistance & 0xFF;
			}

			bullet->currentDestination = tile;
			bullet->o.hitpoints = damage;

			if (damage > 15) bullet->o.flags.s.bulletIsBig = true;

			if ((bullet->o.seenByHouses & (1 << g_playerHouseID)) != 0) return bullet;

			Tile_RemoveFogInRadius(bullet->o.position, 2);

			return bullet;
		}

		default: return NULL;
	}
}

/**
 * Display status text for the given unit.
 *
 * @param unit The Unit to display status text for.
 */
void Unit_DisplayStatusText(Unit *unit)
{
	const UnitInfo *ui;
	char buffer[81];

	if (unit == NULL) return;

	ui = &g_table_unitInfo[unit->o.type];

	if (unit->o.type == UNIT_SANDWORM) {
		snprintf(buffer, sizeof(buffer), "%s", String_Get_ByIndex(ui->o.stringID_abbrev));
	} else {
		const char *houseName = g_table_houseInfo[Unit_GetHouseID(unit)].name;
		if (g_config.language == LANGUAGE_FRENCH) {
			snprintf(buffer, sizeof(buffer), "%s %s", String_Get_ByIndex(ui->o.stringID_abbrev), houseName);
		} else {
			snprintf(buffer, sizeof(buffer), "%s %s", houseName, String_Get_ByIndex(ui->o.stringID_abbrev));
		}
	}

	if (unit->o.type == UNIT_HARVESTER) {
		uint16 stringID;

		stringID = STR_IS_D_PERCENT_FULL;

		if (unit->actionID == ACTION_HARVEST && unit->amount < 100) {
			uint16 type = Map_GetLandscapeType(Tile_PackTile(unit->o.position));

			if (type == LST_SPICE || type == LST_THICK_SPICE) stringID = STR_IS_D_PERCENT_FULL_AND_HARVESTING;
		}

		if (unit->actionID == ACTION_MOVE && Tools_Index_GetStructure(unit->targetMove) != NULL) {
			stringID = STR_IS_D_PERCENT_FULL_AND_HEADING_BACK;
		} else {
			if (unit->o.script.variables[4] != 0) {
				stringID = STR_IS_D_PERCENT_FULL_AND_AWAITING_PICKUP;
			}
		}

		if (unit->amount == 0) stringID += 4;

		{
			size_t len = strlen(buffer);
			char *s = buffer + len;

			snprintf(s, sizeof(buffer) - len, String_Get_ByIndex(stringID), unit->amount);
		}
	}

	{
		/* add a dot "." at the end of the buffer */
		size_t len = strlen(buffer);
		if (len < sizeof(buffer) - 1) {
			buffer[len] = '.';
			buffer[len + 1] = '\0';
		}
	}
	GUI_DisplayText(buffer, 2);
}

/**
 * Hide a unit from the viewport. Happens when a unit enters a structure or
 *  gets picked up by a carry-all.
 *
 * @param unit The Unit to hide.
 */
void Unit_Hide(Unit *unit)
{
	if (unit == NULL) return;

	UnitSelection_Remove(unit);

	unit->o.flags.s.bulletIsBig = true;
	Unit_UpdateMap(0, unit);
	unit->o.flags.s.bulletIsBig = false;

	Script_Reset(&unit->o.script, g_scriptUnit);
	Unit_UntargetMe(unit);

	unit->o.flags.s.isNotOnMap = true;
	Unit_HouseUnitCount_Remove(unit);
}

bool Unit_RepairReturnIsSafe(Unit *unit, uint16 packed)
{
	PoolFindStruct find;
	bool alliedForce = false;
	bool enemyForce = false;

	if (unit == NULL || !Map_IsValidPosition(packed)) return false;
	find.houseID = HOUSE_INVALID;
	find.type = 0xFFFF;
	find.index = 0xFFFF;
	while (true) {
		Unit *other = Unit_Find(&find);
		const UnitInfo *ui;

		if (other == NULL) break;
		if (other == unit || other->o.flags.s.isNotOnMap) continue;
		if (Tile_GetDistancePacked(packed, Tile_PackTile(other->o.position)) > 6) continue;
		ui = &g_table_unitInfo[other->o.type];
		if (!ui->flags.isNormalUnit || ui->fireDistance == 0) continue;
		if (House_AreAllied(Unit_GetHouseID(unit), Unit_GetHouseID(other))) alliedForce = true;
		else enemyForce = true;
	}

	return !enemyForce || alliedForce;
}

static void Unit_AirTransit_Update(Unit *unit)
{
	Unit *carryall;
	uint16 encoded;

	if (unit->airTransitDestination == 0 || !Map_IsValidPosition(unit->airTransitDestination)) return;
	if (!UnitSelection_CanAirTransit(unit) || unit->o.script.variables[4] != 0) return;
	/* A loaded harvester belongs to the refinery, never to an airlift order. */
	if (unit->o.type == UNIT_HARVESTER && unit->amount >= 100) return;

	encoded = Tools_Index_Encode(unit->o.index, IT_UNIT);
	carryall = Unit_CallUnitByType(UNIT_CARRYALL, Unit_GetHouseID(unit), encoded, false);
	if (carryall == NULL) return;
	Object_Script_Variable4_Link(encoded, Tools_Index_Encode(carryall->o.index, IT_UNIT));
}

/**
 * Call a specified type of unit owned by the house to you.
 *
 * @param type The type of the Unit to find.
 * @param houseID The houseID of the Unit to find.
 * @param target To where the found Unit should move.
 * @param createCarryall Create a carryall if none found.
 * @return The found Unit, or NULL if none found.
 */
Unit *Unit_CallUnitByType(UnitType type, uint8 houseID, uint16 target, bool createCarryall)
{
	PoolFindStruct find;
	Unit *unit = NULL;

	find.houseID = houseID;
	find.type    = type;
	find.index   = 0xFFFF;

	while (true) {
		Unit *u;

		u = Unit_Find(&find);
		if (u == NULL) break;
		if (u->o.linkedID != 0xFF) continue;
		if (u->targetMove != 0) continue;
		unit = u;
	}

	if (createCarryall && unit == NULL && type == UNIT_CARRYALL) {
		tile32 position;

		g_validateStrictIfZero++;
		position.x = 0;
		position.y = 0;
		unit = Unit_Create(UNIT_INDEX_INVALID, type, houseID, position, 96);
		g_validateStrictIfZero--;

		if (unit != NULL) unit->o.flags.s.byScenario = true;
	}

	if (unit != NULL) {
		unit->targetMove = target;

		Object_Script_Variable4_Set(&unit->o, target);
	}

	return unit;
}

/**
 * Handles what happens when the given unit enters into the given structure.
 *
 * @param unit The Unit.
 * @param s The Structure.
 */
void Unit_EnterStructure(Unit *unit, Structure *s)
{
	const StructureInfo *si;
	const UnitInfo *ui;
	bool selectStructure;

	if (unit == NULL || s == NULL) return;
	selectStructure = g_dune2_enhanced && unit == g_unitSelected && g_unitSelectionCount <= 1;

	ui = &g_table_unitInfo[unit->o.type];
	si = &g_table_structureInfo[s->o.type];

	if (!unit->o.flags.s.allocated || s->o.hitpoints == 0) {
		Unit_Remove(unit);
		return;
	}

	unit->o.seenByHouses |= s->o.seenByHouses;
	Unit_Hide(unit);
	/* Preserve surviving group members.  Select the destination structure only
	 * when this was the sole selected unit. */
	if (selectStructure) Map_SetSelection(Tile_PackTile(s->o.position));

	if (House_AreAllied(s->o.houseID, Unit_GetHouseID(unit))) {
		Structure_SetState(s, si->o.flags.busyStateIsIncoming ? STRUCTURE_STATE_READY : STRUCTURE_STATE_BUSY);

		if (s->o.type == STRUCTURE_REPAIR) {
			uint16 countDown;

			countDown = ((ui->o.hitpoints - unit->o.hitpoints) * 256 / ui->o.hitpoints) * (ui->o.buildTime << 6) / 256;

			if (countDown > 1) {
				s->countDown = countDown;
			} else {
				s->countDown = 1;
			}
			unit->o.hitpoints = ui->o.hitpoints;
			unit->o.flags.s.isSmoking = false;
			unit->spriteOffset = 0;
		}
		unit->o.linkedID = s->o.linkedID;
		s->o.linkedID = unit->o.index & 0xFF;
		return;
	}

	if (unit->o.type == UNIT_SABOTEUR) {
		Structure_Damage(s, 500, 1);
		Unit_Remove(unit);
		return;
	}

	/* Take over the building when low on hitpoints */
	if (s->o.hitpoints < si->o.hitpoints / 4) {
		House *h;

		h = House_Get_ByIndex(s->o.houseID);
		s->o.houseID = Unit_GetHouseID(unit);
		h->structuresBuilt = Structure_GetStructuresBuilt(h);

		/* ENHANCEMENT -- recalculate the power and credits for the house losing the structure. */
		if (g_dune2_enhanced) House_CalculatePowerAndCredit(h);

		h = House_Get_ByIndex(s->o.houseID);
		h->structuresBuilt = Structure_GetStructuresBuilt(h);

		if (s->o.linkedID != 0xFF) {
			Unit *u = Unit_Get_ByIndex(s->o.linkedID);
			if (u != NULL) u->o.houseID = Unit_GetHouseID(unit);
		}

		House_CalculatePowerAndCredit(House_Get_ByIndex(s->o.houseID));
		Structure_UpdateMap(s);

		/* ENHANCEMENT -- When taking over a structure, untarget it. Else you will destroy the structure you just have taken over very easily */
		if (g_dune2_enhanced) Structure_UntargetMe(s);

		/* ENHANCEMENT -- When taking over a structure, unveil the fog around the structure. */
		if (g_dune2_enhanced) Structure_RemoveFog(s);
	} else {
		Structure_Damage(s, min(unit->o.hitpoints * 2, s->o.hitpoints / 2), 1);
	}

	Object_Script_Variable4_Clear(&s->o);

	Unit_Remove(unit);
}

/**
 * Gets the best target structure for the given unit.
 *
 * @param unit The Unit to get the best target for.
 * @param mode How to determine the best target.
 * @return The best target or NULL if none found.
 */
static Structure *Unit_FindBestTargetStructure(Unit *unit, uint16 mode)
{
	Structure *best = NULL;
	uint16 bestPriority = 0;
	tile32 position;
	uint16 distance;
	PoolFindStruct find;

	if (unit == NULL) return NULL;

	position = Tools_Index_GetTile(unit->originEncoded);
	distance = g_table_unitInfo[unit->o.type].fireDistance << 8;

	find.houseID = HOUSE_INVALID;
	find.index   = 0xFFFF;
	find.type    = 0xFFFF;

	while (true) {
		Structure *s;
		tile32 curPosition;
		uint16 priority;

		s = Structure_Find(&find);
		if (s == NULL) break;
		if (s->o.type == STRUCTURE_SLAB_1x1 || s->o.type == STRUCTURE_SLAB_2x2 || s->o.type == STRUCTURE_WALL) continue;

		curPosition.x = s->o.position.x + g_table_structure_layoutTileDiff[g_table_structureInfo[s->o.type].layout].x;
		curPosition.y = s->o.position.y + g_table_structure_layoutTileDiff[g_table_structureInfo[s->o.type].layout].y;

		if (mode != 0 && mode != 4) {
			if (mode == 1) {
				if (Tile_GetDistance(unit->o.position, curPosition) > distance) continue;
			} else {
				if (mode != 2) continue;
				if (Tile_GetDistance(position, curPosition) > distance * 2) continue;
			}
		}

		priority = Unit_GetTargetStructurePriority(unit, s);

		if (priority >= bestPriority) {
			best = s;
			bestPriority = priority;
		}
	}

	if (bestPriority == 0) return NULL;

	return best;
}

/**
 * Get the score of entering this tile from a direction.
 *
 * @param unit The Unit to operate on.
 * @param packed The packed tile.
 * @param direction The direction entering this tile from.
 * @return 256 if tile is not accessable, -1 when it is an accessable structure,
 *   or a score to enter the tile otherwise.
 */
int16 Unit_GetTileEnterScore(Unit *unit, uint16 packed, uint16 orient8)
{
	const UnitInfo *ui;
	Unit *u;
	Structure *s;
	uint16 type;
	uint16 res;

	if (unit == NULL) return 0;

	ui = &g_table_unitInfo[unit->o.type];

	if (!Map_IsValidPosition(packed) && ui->movementType != MOVEMENT_WINGER) return 256;

	u = Unit_Get_ByPackedTile(packed);
	if (u != NULL && u != unit && unit->o.type != UNIT_SANDWORM) {
		if (unit->o.type == UNIT_SABOTEUR && unit->targetMove == Tools_Index_Encode(u->o.index, IT_UNIT)) return 0;

		if (House_AreAllied(Unit_GetHouseID(u), Unit_GetHouseID(unit))) return 256;
		if (g_table_unitInfo[u->o.type].movementType != MOVEMENT_FOOT || (ui->movementType != MOVEMENT_TRACKED && ui->movementType != MOVEMENT_HARVESTER)) return 256;
	}

	s = Structure_Get_ByPackedTile(packed);
	if (s != NULL) {
		res = Unit_IsValidMovementIntoStructure(unit, s);
		if (res == 0) return 256;
		return -res;
	}

	type = Map_GetLandscapeType(packed);

	if (g_dune2_enhanced) {
		res = g_table_landscapeInfo[type].movementSpeed[ui->movementType] * ui->movingSpeedFactor / 256;
	} else {
		res = g_table_landscapeInfo[type].movementSpeed[ui->movementType];
	}

	if (unit->o.type == UNIT_SABOTEUR && type == LST_WALL) {
		if (!House_AreAllied(g_map[packed].houseID, Unit_GetHouseID(unit))) res = 255;
	}

	if (res == 0) return 256;

	/* Check if the unit is travelling diagonally. */
	if ((orient8 & 1) != 0) {
		res -= res / 4 + res / 8;
	}

	/* 'Invert' the speed to get a rough estimate of the time taken. */
	res ^= 0xFF;

	return (int16)res;
}

/**
 * Gets the best target for the given unit.
 *
 * @param unit The Unit to get the best target for.
 * @param mode How to determine the best target.
 * @return The encoded index of the best target or 0 if none found.
 */
uint16 Unit_FindBestTargetEncoded(Unit *unit, uint16 mode)
{
	Structure *s;
	Unit *target;

	if (unit == NULL) return 0;

	s = NULL;

	if (mode == 4) {
		s = Unit_FindBestTargetStructure(unit, mode);

		if (s != NULL) return Tools_Index_Encode(s->o.index, IT_STRUCTURE);

		target = Unit_FindBestTargetUnit(unit, mode);

		if (target == NULL) return 0;
		return Tools_Index_Encode(target->o.index, IT_UNIT);
	}

	target = Unit_FindBestTargetUnit(unit, mode);

	if (unit->o.type != UNIT_DEVIATOR) s = Unit_FindBestTargetStructure(unit, mode);

	if (target != NULL && s != NULL) {
		uint16 priority;

		priority = Unit_GetTargetUnitPriority(unit, target);

		if (Unit_GetTargetStructurePriority(unit, s) >= priority) return Tools_Index_Encode(s->o.index, IT_STRUCTURE);
		return Tools_Index_Encode(target->o.index, IT_UNIT);
	}

	if (target != NULL) return Tools_Index_Encode(target->o.index, IT_UNIT);
	if (s != NULL) return Tools_Index_Encode(s->o.index, IT_STRUCTURE);

	return 0;
}

/**
 * Check if the Unit belonged the the current human, and do some extra tasks.
 *
 * @param unit The Unit to operate on.
 */
void Unit_RemovePlayer(Unit *unit)
{
	bool wasPrimary;

	if (unit == NULL) return;
	if (Unit_GetHouseID(unit) != g_playerHouseID) return;
	if (!unit->o.flags.s.allocated) return;

	wasPrimary = unit == g_unitSelected;
	unit->o.flags.s.allocated = false;
	Unit_RemoveFromTeam(unit);
	UnitSelection_Remove(unit);

	if (!wasPrimary) return;

	if (g_selectionType == SELECTIONTYPE_TARGET) {
		g_unitActive = NULL;
		g_activeAction = 0xFFFF;

		GUI_ChangeSelectionType(g_unitSelectionCount != 0 ? SELECTIONTYPE_UNIT : SELECTIONTYPE_STRUCTURE);
	}

}

/**
 * Update the map around the Unit depending on the type (entering tile, leaving, staying).
 * @param type The type of action on the map.
 * @param unit The Unit doing the action.
 */
void Unit_UpdateMap(uint16 type, Unit *unit)
{
	const UnitInfo *ui;
	tile32 position;
	uint16 packed;
	Tile *t;
	uint16 radius;

	if (unit == NULL || unit->o.flags.s.isNotOnMap || !unit->o.flags.s.used) return;

	ui = &g_table_unitInfo[unit->o.type];

	if (ui->movementType == MOVEMENT_WINGER) {
		if (type != 0) {
			unit->o.flags.s.isDirty = true;
			g_dirtyAirUnitCount++;
		}

		radius = g_table_unitInfo[unit->o.type].dimension;
		/* Health bars are drawn as an overlay above the sprite.  Invalidate a
		 * full neighbourhood at both the old and new air position so their old
		 * pixels are restored before the next overlay is drawn. */
		if (g_gameConfig.unitHealthBars && ui->flags.isNormalUnit) radius = max(radius, 40);
		Map_UpdateAround(radius, unit->o.position, unit, g_functions[0][type]);
		return;
	}

	position = unit->o.position;
	packed = Tile_PackTile(position);
	t = &g_map[packed];

	if (t->isUnveiled || unit->o.houseID == g_playerHouseID) {
		Unit_HouseUnitCount_Add(unit, g_playerHouseID);
	} else {
		Unit_HouseUnitCount_Remove(unit);
	}

	if (type == 1) {
		if (House_AreAllied(Unit_GetHouseID(unit), g_playerHouseID) && !Map_IsPositionUnveiled(packed) && unit->o.type != UNIT_SANDWORM) {
			Tile_RemoveFogInRadius(position, 1);
		}

		if (Object_GetByPackedTile(packed) == NULL) {
			t->index = unit->o.index + 1;
			t->hasUnit = true;
		}
	}

	if (type != 0) {
		unit->o.flags.s.isDirty = true;
		g_dirtyUnitCount++;
	}

	radius = ui->dimension + 3;

	if (unit->o.flags.s.bulletIsBig || unit->o.flags.s.isSmoking || (unit->o.type == UNIT_HARVESTER && unit->actionID == ACTION_HARVEST)) radius = 33;
	/* The bar extends above and beyond the unit sprite.  A small unit normally
	 * dirties only its own tile, leaving the primitive bar at its previous
	 * location.  40 quarter-tiles covers that overlay on either side. */
	if (g_gameConfig.unitHealthBars && ui->flags.isNormalUnit) radius = max(radius, 40);

	Map_UpdateAround(radius, position, unit, g_functions[1][type]);

	if (unit->o.type != UNIT_HARVESTER) return;

	/* The harvester is the only 2x1 unit, so also update tiles in behind us. */
	Map_UpdateAround(radius, unit->targetPreLast, unit, g_functions[1][type]);
	Map_UpdateAround(radius, unit->targetLast, unit, g_functions[1][type]);
}

/**
 * Removes the Unit from the given packed tile.
 *
 * @param unit The Unit to remove.
 * @param packed The packed tile.
 */
void Unit_RemoveFromTile(Unit *unit, uint16 packed)
{
	Tile *t = &g_map[packed];

	if (t->hasUnit && Unit_Get_ByPackedTile(packed) == unit && (packed != Tile_PackTile(unit->currentDestination) || unit->o.flags.s.bulletIsBig)) {
		t->index = 0;
		t->hasUnit = false;
	}

	Map_MarkTileDirty(packed);

	Map_Update(packed, 0, false);
}

void Unit_AddToTile(Unit *unit, uint16 packed)
{
	Map_UnveilTile(packed, Unit_GetHouseID(unit));
	Map_MarkTileDirty(packed);
	Map_Update(packed, 1, false);
}

/**
 * Get the priority a target structure has for a given unit. The higher the value,
 *  the more serious it should look at the target.
 *
 * @param unit The unit looking at a target.
 * @param target The structure to look at.
 * @return The priority of the target.
 */
uint16 Unit_GetTargetStructurePriority(Unit *unit, Structure *target)
{
	const StructureInfo *si;
	uint16 priority;
	uint16 distance;

	if (unit == NULL || target == NULL) return 0;

	if (House_AreAllied(Unit_GetHouseID(unit), target->o.houseID)) return 0;
	if ((target->o.seenByHouses & (1 << Unit_GetHouseID(unit))) == 0) return 0;

	si = &g_table_structureInfo[target->o.type];
	priority = si->o.priorityBuild + si->o.priorityTarget;
	distance = Tile_GetDistanceRoundedUp(unit->o.position, target->o.position);

	/* A turret close enough to be shooting is worth more than anything behind it.
	 *
	 * On the table a Heavy Factory is 600 and a Rocket Turret 175, so a team
	 * walked its whole length past the defence line to reach the factory, took
	 * the line's fire the entire way, and died without having returned a shot at
	 * what killed it.  Clearing the line first is not a preference, it is the
	 * only way through -- so within its reach a turret outranks the base.
	 *
	 * Eight tiles is an approximation of that reach: the turret's own radius
	 * lives in the structure script, not in a table this side can read.  Skirmish
	 * only, so campaign targeting is untouched. */
	if (Skirmish_IsActive() && distance <= 8
		&& (target->o.type == STRUCTURE_TURRET || target->o.type == STRUCTURE_ROCKET_TURRET)) {
		priority += 700;
	}

	if (distance != 0) priority /= distance;

	return min(priority, 32000);
}

void Unit_LaunchHouseMissile(uint16 packed)
{
	tile32 tile;
	bool isAI;
	House *h;

	if (g_unitHouseMissile == NULL) return;

	h = House_Get_ByIndex(g_unitHouseMissile->o.houseID);

	tile = Tile_UnpackTile(packed);
	tile = Tile_MoveByRandom(tile, 160, false);

	packed = Tile_PackTile(tile);

	isAI = g_unitHouseMissile->o.houseID != g_playerHouseID;

	Unit_Free(g_unitHouseMissile);

	Sound_Output_Feedback(0xFFFE);

	Unit_CreateBullet(h->palacePosition, g_unitHouseMissile->o.type, g_unitHouseMissile->o.houseID, 0x1F4, Tools_Index_Encode(packed, IT_TILE));

	g_houseMissileCountdown = 0;
	g_unitHouseMissile = NULL;

	if (isAI) {
		Sound_Output_Feedback(39);
		return;
	}

	GUI_ChangeSelectionType(SELECTIONTYPE_STRUCTURE);
}

/**
 * This unit is about to disapear from the map. So remove it from the house
 *  statistics about allies/enemies.
 * @param unit The unit to remove.
 */
void Unit_HouseUnitCount_Remove(Unit *unit)
{
	PoolFindStruct find;

	if (unit == NULL) return;
	if (unit->o.seenByHouses == 0) return;

	find.houseID = HOUSE_INVALID;
	find.index   = 0xFFFF;
	find.type    = 0xFFFF;

	while (true) {
		House *h;

		h = House_Find(&find);
		if (h == NULL) break;

		if ((unit->o.seenByHouses & (1 << h->index)) == 0) continue;

		if (!House_AreAllied((uint8)h->index, Unit_GetHouseID(unit))) {
			h->unitCountEnemy--;
		} else {
			h->unitCountAllied--;
		}

		unit->o.seenByHouses &= ~(1 << h->index);
	}

	if (g_dune2_enhanced) unit->o.seenByHouses = 0;
}

/**
 * This unit is about to appear on the map. So add it from the house
 *  statistics about allies/enemies, and do some other logic.
 * @param unit The unit to add.
 * @param houseID The house registering the add.
 */
void Unit_HouseUnitCount_Add(Unit *unit, uint8 houseID)
{
	const UnitInfo *ui;
	uint16 houseIDBit;
	House *hp;
	House *h;

	if (unit == NULL) return;

	hp = House_Get_ByIndex(g_playerHouseID);
	ui = &g_table_unitInfo[unit->o.type];
	h = House_Get_ByIndex(houseID);
	houseIDBit = (1 << houseID);

	if (houseID == HOUSE_ATREIDES && unit->o.type != UNIT_SANDWORM) {
		houseIDBit |= (1 << HOUSE_FREMEN);
	}

	if ((unit->o.seenByHouses & houseIDBit) != 0 && h->flags.isAIActive) {
		unit->o.seenByHouses |= houseIDBit;
		return;
	}

	if (!ui->flags.isNormalUnit && unit->o.type != UNIT_SANDWORM) {
		return;
	}

	if ((unit->o.seenByHouses & houseIDBit) == 0) {
		if (House_AreAllied(houseID, Unit_GetHouseID(unit))) {
			h->unitCountAllied++;
		} else {
			h->unitCountEnemy++;
		}
	}

	if (ui->movementType != MOVEMENT_WINGER) {
		if (!House_AreAllied(houseID, Unit_GetHouseID(unit))) {
			h->flags.isAIActive = true;
			House_Get_ByIndex(Unit_GetHouseID(unit))->flags.isAIActive = true;
		}
	}

	if (houseID == g_playerHouseID && g_selectionType != SELECTIONTYPE_MENTAT) {
		if (unit->o.type == UNIT_SANDWORM) {
			if (hp->timerSandwormAttack == 0) {
				if (g_musicInBattle == 0) g_musicInBattle = 1;

				Sound_Output_Feedback(37);

				if (g_config.language == LANGUAGE_ENGLISH) {
					GUI_DisplayHint(STR_WARNING_SANDWORMS_SHAIHULUD_ROAM_DUNE_DEVOURING_ANYTHING_ON_THE_SAND, 105);
				}

				hp->timerSandwormAttack = 8;
			}
		} else if (!House_AreAllied(g_playerHouseID, Unit_GetHouseID(unit))) {
			Team *t;

			if (hp->timerUnitAttack == 0) {
				if (g_musicInBattle == 0) g_musicInBattle = 1;

				if (unit->o.type == UNIT_SABOTEUR) {
					Sound_Output_Feedback(12);
				} else {
					if (g_scenarioID < 3) {
						PoolFindStruct find;
						Structure *s;
						uint16 feedbackID;

						find.houseID = g_playerHouseID;
						find.index   = 0xFFFF;
						find.type    = STRUCTURE_CONSTRUCTION_YARD;

						s = Structure_Find(&find);
						if (s != NULL) {
							feedbackID = ((Orientation_Orientation256ToOrientation8(Tile_GetDirection(s->o.position, unit->o.position)) + 1) & 7) / 2 + 2;
						} else {
							feedbackID = 1;
						}

						Sound_Output_Feedback(feedbackID);
					} else {
						Sound_Output_Feedback(unit->o.houseID + 6);
					}
				}

				hp->timerUnitAttack = 8;
			}

			t = Team_Get_ByIndex(unit->team);
			if (t != NULL) t->script.variables[4] = 1;
		}
	}

	if (!House_AreAllied(houseID, unit->o.houseID) && unit->actionID == ACTION_AMBUSH) Unit_SetAction(unit, ACTION_HUNT);

	if (unit->o.houseID == g_playerHouseID || (unit->o.houseID == HOUSE_FREMEN && g_playerHouseID == HOUSE_ATREIDES)) {
		unit->o.seenByHouses = 0xFF;
	} else {
		unit->o.seenByHouses |= houseIDBit;
	}
}
