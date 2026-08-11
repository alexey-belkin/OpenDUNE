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
#include "map.h"
#include "opendune.h"
#include "pool/pool.h"
#include "pool/house.h"
#include "pool/structure.h"
#include "pool/unit.h"
#include "pool/team.h"
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
static uint16 s_unitOrderCount = 0;
static ActionType s_unitOrderAction = ACTION_INVALID;
static bool s_unitOrderAirTransit = false;
static bool s_unitSelectionChanging = false;

/* Runtime-only tactical state.  The actual route remains owned by the unit
 * script, so save-game layouts and the normal movement system stay intact. */
static bool s_attackPositionManual[UNIT_INDEX_MAX];
static uint16 s_attackPositionTile[UNIT_INDEX_MAX];
static uint16 s_attackPositionTarget[UNIT_INDEX_MAX];
static uint32 s_attackPositionETA[UNIT_INDEX_MAX];
static uint32 s_attackPositionNextCheck[UNIT_INDEX_MAX];
static bool s_autonomousAttack[UNIT_INDEX_MAX];
static ActionType s_autonomousReturnAction[UNIT_INDEX_MAX];
static bool s_manualHunt[UNIT_INDEX_MAX];
static uint32 s_autonomyNextCheck[UNIT_INDEX_MAX];
static uint32 s_harvesterNextCheck[UNIT_INDEX_MAX];
static uint16 s_harvesterLastPosition[UNIT_INDEX_MAX];
static uint32 s_harvesterLastProgress[UNIT_INDEX_MAX];
static uint16 s_houseThreatTarget[HOUSE_MAX];
static uint32 s_houseThreatUntil[HOUSE_MAX];

static void Unit_AirTransit_Update(Unit *unit);

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
	s_autonomousAttack[unit->o.index] = false;
	s_autonomousReturnAction[unit->o.index] = ACTION_INVALID;
	Unit_AttackPosition_Clear(unit);
	if (enabled) s_attackPositionNextCheck[unit->o.index] = g_timerGame + (unit->o.index % 7) * 3;
}

static void Unit_AttackPosition_SetAutomatic(Unit *unit, ActionType returnAction)
{
	if (unit == NULL || unit->o.index >= UNIT_INDEX_MAX) return;

	s_attackPositionManual[unit->o.index] = true;
	s_autonomousAttack[unit->o.index] = true;
	s_autonomousReturnAction[unit->o.index] = returnAction;
	Unit_AttackPosition_Clear(unit);
	s_attackPositionNextCheck[unit->o.index] = g_timerGame + (unit->o.index % 7) * 3;
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
	uint16 fallbackPacked = 0;
	uint32 fallbackETA = 0xFFFFFFFF;
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
		if (s_attackPositionTile[unit->o.index] != 0 && unit->targetMove == Tools_Index_Encode(s_attackPositionTile[unit->o.index], IT_TILE)) {
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
			if (Unit_GetTileEnterScore(unit, packed, 0) > 255) continue;
			if (!Script_Unit_HasRoute(unit, packedSource, packed, &travelTicks)) continue;

			candidate = unit->o;
			candidate.position = Tile_UnpackTile(packed);
			firingDistance = Object_GetDistanceToEncoded(&candidate, unit->targetAttack);
			if (firingDistance > (ui->fireDistance << 8)) continue;
			eta = g_timerGame + travelTicks;
			if (eta < fallbackETA) {
				fallbackETA = eta;
				fallbackPacked = packed;
			}

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

	if (bestPacked == 0) {
		if (refreshedOldETA != 0) s_attackPositionETA[unit->o.index] = refreshedOldETA;
		else {
			Unit_AttackPosition_Clear(unit);
			/* All viable firing spots may already be reserved.  Keep turreted
			 * units advancing toward the target instead of leaving targetMove
			 * at zero; the next tactical pass can then assign an opened slot. */
			if (fallbackPacked != 0) Unit_SetDestination(unit, Tools_Index_Encode(fallbackPacked, IT_TILE));
			else if (ui->o.flags.hasTurret) Unit_SetDestination(unit, unit->targetAttack);
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
	if (s_manualHunt[unit->o.index]) return 63;
	switch (unit->actionID) {
		case ACTION_GUARD:      return 5;
		case ACTION_AREA_GUARD: return 14;
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
	uint16 radius = Unit_Autonomy_GetSearchRadius(unit);
	uint16 anchor = unit->guardPosition;

	if (radius == 0 || !Tools_Index_IsValid(target)) return false;
	if (s_manualHunt[unit->o.index] || unit->actionID == ACTION_HUNT) return true;
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

static uint16 Unit_Autonomy_FindTarget(Unit *unit)
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
		uint16 maxHitpoints;
		uint16 hitpoints;
		uint32 priority;
		uint32 eta;
		uint32 score;
		uint32 coverage;
		uint32 focus;

		priority = Unit_Autonomy_BasePriority(unit, candidates[i], &maxHitpoints, &hitpoints);
		if (priority == 0 || !Unit_AttackPosition_EstimateTravel(unit, candidates[i], &eta)) continue;

		/* ETA is the pathfinder-derived time to an actual firing tile, rather
		 * than a straight-line distance. */
		score = priority * 1024 / (eta / 15 + 1);
		score = score * (256 + ((maxHitpoints - min(hitpoints, maxHitpoints)) * 179 / max(maxHitpoints, 1))) / 256;

		coverage = Unit_Autonomy_IncomingDamage(unit, candidates[i]) * 256 / max(hitpoints, 1);
		if (coverage < 256) {
			focus = 256 + min(154, coverage * 3 / 5);
		} else {
			focus = 410 - min(180, (coverage - 256) * 3 / 5);
		}
		score = score * focus / 256;
		if (s_houseThreatUntil[g_playerHouseID] > g_timerGame && candidates[i] == s_houseThreatTarget[g_playerHouseID]) score *= 2;

		if (score > bestScore) {
			bestScore = score;
			best = candidates[i];
		}
	}

	return best;
}

/* Consume an autonomous combat sortie and restore the unit's original post.
 * This is also called from the unit script completion path, which runs before
 * the periodic tactical update and therefore cannot lose the return order. */
bool Unit_Autonomy_ReturnToPost(Unit *unit)
{
	ActionType returnAction;
	uint16 anchor;

	if (unit == NULL) return false;
	returnAction = s_autonomousReturnAction[unit->o.index];
	anchor = unit->guardPosition;
	if (!s_autonomousAttack[unit->o.index] || returnAction == ACTION_INVALID) return false;
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
	unit->nextActionID = returnAction;
	return true;
}

static void Unit_Autonomy_BeginAttack(Unit *unit, uint16 target)
{
	ActionType returnAction = unit->actionID;

	if (!Tools_Index_IsValid(target)) return;
	Unit_AttackPosition_SetAutomatic(unit, returnAction);
	Unit_SetAction(unit, ACTION_ATTACK);
	Unit_SetTarget(unit, target);
}

static void Unit_Autonomy_Update(Unit *unit)
{
	uint16 target;

	if (!Unit_Autonomy_IsCombatUnit(unit)) return;
	if (unit->actionID == ACTION_ATTACK && s_autonomousAttack[unit->o.index]) {
		if (!Tools_Index_IsValid(unit->targetAttack)) Unit_Autonomy_ReturnToPost(unit);
		return;
	}
	if (Unit_Autonomy_GetSearchRadius(unit) == 0) return;

	if (s_autonomyNextCheck[unit->o.index] == 0) {
		s_autonomyNextCheck[unit->o.index] = g_timerGame + (unit->o.index % 8) * 20;
		return;
	}
	if (s_autonomyNextCheck[unit->o.index] > g_timerGame) return;
	s_autonomyNextCheck[unit->o.index] = g_timerGame + 120;

	if (Tools_Index_IsValid(unit->targetAttack)) {
		Unit_Autonomy_BeginAttack(unit, unit->targetAttack);
		return;
	}

	target = Unit_Autonomy_FindTarget(unit);
	if (target != 0) Unit_Autonomy_BeginAttack(unit, target);
}

void Unit_Autonomy_ReportThreat(uint8 houseID, uint16 attacker, uint16 packed)
{
	if (houseID != g_playerHouseID || !Tools_Index_IsValid(attacker) || !Map_IsValidPosition(packed)) return;

	s_houseThreatTarget[houseID] = attacker;
	s_houseThreatUntil[houseID] = g_timerGame + 180;
}

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
 * idle when another explored spice field is reachable. */
uint16 Unit_Harvester_FindPreferredSpice(Unit *unit)
{
	uint16 packed;
	uint16 target;

	if (unit == NULL || unit->o.type != UNIT_HARVESTER || unit->o.flags.s.isNotOnMap) return 0;
	packed = Tile_PackTile(unit->o.position);
	if (Map_IsValidPosition(unit->harvestCenter) && Unit_Harvester_FindSpice(unit, unit->harvestCenter, 12, &target)) return target;
	if (!Unit_Harvester_FindSpice(unit, packed, 0, &target)) return 0;

	return target;
}

static void Unit_Harvester_Update(Unit *unit)
{
	uint16 packed;
	uint16 target;
	uint16 type;

	if (unit->o.type != UNIT_HARVESTER || Unit_GetHouseID(unit) != g_playerHouseID || unit->amount >= 100) return;
	if (unit->o.flags.s.isNotOnMap) return;
	/* Only an explicit Move-to-wait order may leave a partially empty player
	 * harvester parked.  All other idle states recover into Harvest. */
	if (unit->actionID != ACTION_HARVEST) {
		if (unit->harvestHoldPosition != 0) return;
		Unit_SetAction(unit, ACTION_HARVEST);
	}
	if (s_harvesterNextCheck[unit->o.index] > g_timerGame) return;
	s_harvesterNextCheck[unit->o.index] = g_timerGame + 90;

	packed = Tile_PackTile(unit->o.position);
	if (s_harvesterLastPosition[unit->o.index] != packed || s_harvesterLastProgress[unit->o.index] == 0) {
		s_harvesterLastPosition[unit->o.index] = packed;
		s_harvesterLastProgress[unit->o.index] = g_timerGame;
	}

	/* A valid route is left alone.  A route that has made no progress for six
	 * checks is abandoned so a fresh reachable spice field can be selected. */
	if (unit->targetMove != 0 || unit->currentDestination.x != 0 || unit->currentDestination.y != 0) {
		if (unit->o.script.variables[4] != 0 || s_harvesterLastProgress[unit->o.index] + 540 > g_timerGame) return;
		Object_Script_Variable4_Clear(&unit->o);
		unit->targetMove = 0;
		unit->currentDestination.x = 0;
		unit->currentDestination.y = 0;
		unit->route[0] = 0xFF;
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

static void UnitSelection_ResetOrder(Unit *unit, ActionType action, uint16 packed)
{
	uint16 encoded;

	Unit_SetManualHunt(unit, false);
	Unit_AttackPosition_SetManual(unit, false);
	if (unit->o.type == UNIT_HARVESTER) unit->harvestHoldPosition = (action == ACTION_MOVE);
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
		/* A player-directed move relocates the default Area Guard post. The
		 * completion hook below replaces this with the exact arrival tile. */
		Unit_SetGuardPosition(unit, packed);
		Unit_SetDestination(unit, encoded);
	} else if (action == ACTION_HARVEST) {
		unit->harvestCenter = packed;
		unit->targetMove = encoded;
	} else {
		Unit_SetTarget(unit, encoded);
		if (action == ACTION_ATTACK) Unit_AttackPosition_SetManual(unit, true);
		unit = Tools_Index_GetUnit(unit->targetAttack);
		if (unit != NULL) unit->blinkCounter = 8;
	}
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

	if ((position.x == 0xFFFF) && (position.y == 0xFFFF)) {
		u->o.flags.s.isNotOnMap = true;
		return u;
	}

	Unit_UpdateMap(1, u);

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

	if (u == NULL) return;
	if (u->actionID == ACTION_DESTRUCT || u->actionID == ACTION_DIE || action == ACTION_INVALID) return;
	if (action != ACTION_ATTACK) Unit_AttackPosition_SetManual(u, false);

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

/* Player combat units default to a wider local defence. Harvesters, special
 * units and AI retain their original table-driven default actions. */
ActionType Unit_GetDefaultAction(const Unit *u)
{
	const UnitInfo *ui;

	if (u == NULL) return ACTION_GUARD;
	ui = &g_table_unitInfo[u->o.type];
	if (u->o.houseID == g_playerHouseID && ui->flags.isNormalUnit && ui->flags.isGroundUnit && ui->fireDistance != 0 &&
		(ui->movementType == MOVEMENT_FOOT || ui->movementType == MOVEMENT_TRACKED || ui->movementType == MOVEMENT_WHEELED)) {
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

/* A manual order establishes a new defensive post. Autonomous defence keeps
 * its existing post, so reacting to a threat never drifts the unit's area. */
ActionType Unit_GetDefaultActionAfterCompletion(Unit *u)
{
	if (u != NULL && Unit_GetHouseID(u) == g_playerHouseID &&
		(u->actionID == ACTION_MOVE || (u->actionID == ACTION_ATTACK && s_attackPositionManual[u->o.index] && !s_autonomousAttack[u->o.index]))) {
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

	find.type = STRUCTURE_REFINERY;
	find.houseID = Unit_GetHouseID(unit);
	find.index = 0xFFFF;

	while (true) {
		s2 = Structure_Find(&find);
		if (s2 == NULL) break;
		if (s2->state != STRUCTURE_STATE_BUSY) continue;
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
			d = Tile_GetDistance(unit->o.position, s2->o.position);
			if (mind != 0 && d >= mind) continue;
			mind = d;
			s = s2;
		}
	}

	if (s != NULL) unit->originEncoded = Tools_Index_Encode(s->o.index, IT_STRUCTURE);

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

	unit->targetAttack = encoded;

	if (!g_table_unitInfo[unit->o.type].o.flags.hasTurret) {
		unit->targetMove = encoded;
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
			if (u == g_unitSelected) Unit_Select(NULL);

			Unit_UntargetMe(u);
			u->o.script.variables[1] = 1;
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
		s_unitSelectionChanging = true;
		Unit_Select(Unit_Get_ByIndex(s_unitSelection[0]));
		s_unitSelectionChanging = false;
	}
	GUI_Widget_ActionPanel_Draw(true);
}

/** Select exactly one controllable unit. */
void UnitSelection_SelectSingle(Unit *unit)
{
	s_unitSelectionChanging = true;
	UnitSelection_ClearInternal();
	UnitSelection_Add(unit);
	Unit_Select(unit);
	s_unitSelectionChanging = false;

	GUI_Widget_ActionPanel_Draw(true);
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
	uint16 i;

	s_unitSelectionChanging = true;
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

	if (g_unitSelectionCount != 0) primary = Unit_Get_ByIndex(s_unitSelection[0]);
	Unit_Select(primary);
	s_unitSelectionChanging = false;

	GUI_Widget_ActionPanel_Draw(true);
}

/** Return whether this unit is part of the visible group selection. */
bool UnitSelection_IsSelected(const Unit *unit)
{
	return unit != NULL && (unit == g_unitSelected || UnitSelection_Contains(unit));
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

/** Issue the most useful targeted order available to each selected unit. */
void UnitSelection_IssueDefaultOrder(uint16 packed)
{
	uint16 i;

	UnitSelection_CancelPendingAction();
	for (i = 0; i < g_unitSelectionCount; i++) {
		Unit *unit = Unit_Get_ByIndex(s_unitSelection[i]);
		ActionType action = UnitSelection_GetUnitDefaultAction(unit);

		if (action != ACTION_INVALID) UnitSelection_ResetOrder(unit, action, packed);
	}
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
		s_unitOrderAction = action;
		return s_unitOrderCount != 0;
	}

	for (i = 0; i < g_unitSelectionCount; i++) {
		Unit *unit = Unit_Get_ByIndex(s_unitSelection[i]);
		ActionType unitAction = (action == ACTION_MAX) ? UnitSelection_GetUnitSpecialAction(unit) : action;
		if (unitAction == ACTION_INVALID || !UnitSelection_UnitHasAction(unit, unitAction)) continue;

		Object_Script_Variable4_Clear(&unit->o);
		Unit_SetManualHunt(unit, false);
		unit->targetAttack = 0;
		unit->targetMove = 0;
		unit->route[0] = 0xFF;
		if (unitAction == ACTION_GUARD || unitAction == ACTION_AREA_GUARD) Unit_SetGuardPosition(unit, Tile_PackTile(unit->o.position));
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

	if (!UnitSelection_HasPendingAction()) return;
	if (s_unitOrderAirTransit) {
		for (i = 0; i < s_unitOrderCount; i++) {
			Unit *unit = Unit_Get_ByIndex(s_unitOrder[i]);

			if (!UnitSelection_CanAirTransit(unit)) continue;
			Unit_SetManualHunt(unit, false);
			Unit_AttackPosition_SetManual(unit, false);
			Object_Script_Variable4_Clear(&unit->o);
			unit->targetAttack = 0;
			unit->targetMove = 0;
			unit->route[0] = 0xFF;
			unit->airTransitDestination = packed;
			Unit_SetAction(unit, ACTION_STOP);
		}
		UnitSelection_CancelPendingAction();
		GUI_Widget_ActionPanel_Draw(true);
		return;
	}

	for (i = 0; i < s_unitOrderCount; i++) {
		Unit *unit = Unit_Get_ByIndex(s_unitOrder[i]);
		if (UnitSelection_UnitHasAction(unit, s_unitOrderAction)) {
			UnitSelection_ResetOrder(unit, s_unitOrderAction, packed);
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
 * Selects the given unit.
 *
 * @param unit The Unit to select.
 */
void Unit_Select(Unit *unit)
{
	if (unit != NULL && !unit->o.flags.s.allocated && !g_debugGame) {
		unit = NULL;
	}

	if (unit != NULL && (unit->o.seenByHouses & (1 << g_playerHouseID)) == 0 && !g_debugGame) {
		unit = NULL;
	}

	if (!s_unitSelectionChanging) {
		UnitSelection_ClearInternal();
		UnitSelection_Add(unit);
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

	if (unit == NULL || s == NULL) return;

	if (unit == g_unitSelected) {
		/* ENHANCEMENT -- When a Unit enters a Structure, the last tile the Unit was on becomes selected rather than the entire Structure. */
		if (g_dune2_enhanced) {
			Map_SetSelection(Tile_PackTile(s->o.position));
		} else {
			Unit_Select(NULL);
		}
	}

	ui = &g_table_unitInfo[unit->o.type];
	si = &g_table_structureInfo[s->o.type];

	if (!unit->o.flags.s.allocated || s->o.hitpoints == 0) {
		Unit_Remove(unit);
		return;
	}

	unit->o.seenByHouses |= s->o.seenByHouses;
	Unit_Hide(unit);

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
	if (unit == NULL) return;
	if (Unit_GetHouseID(unit) != g_playerHouseID) return;
	if (!unit->o.flags.s.allocated) return;

	unit->o.flags.s.allocated = false;
	Unit_RemoveFromTeam(unit);

	if (unit != g_unitSelected) return;

	if (g_selectionType == SELECTIONTYPE_TARGET) {
		g_unitActive = NULL;
		g_activeAction = 0xFFFF;

		GUI_ChangeSelectionType(SELECTIONTYPE_STRUCTURE);
	}

	Unit_Select(NULL);
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

		Map_UpdateAround(g_table_unitInfo[unit->o.type].dimension, unit->o.position, unit, g_functions[0][type]);
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
