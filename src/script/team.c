/** @file src/script/team.c %Team script routines. */

#include <stdio.h>
#include "types.h"
#include "../os/endian.h"

#include "script.h"

#include "../gui/gui.h"
#include "../house.h"
#include "../pool/team.h"
#include "../pool/pool.h"
#include "../pool/unit.h"
#include "../skirmish.h"
#include "../structure.h"
#include "../team.h"
#include "../tile.h"
#include "../tools.h"
#include "../unit.h"


/**
 * Gets the amount of members in the current team.
 *
 * Stack: *none*.
 *
 * @param script The script engine to operate on.
 * @return Amount of members in current team.
 */
uint16 Script_Team_GetMembers(ScriptEngine *script)
{
	VARIABLE_NOT_USED(script);
	return g_scriptCurrentTeam->members;
}

/**
 * Gets the variable_06 of the current team.
 *
 * Stack: *none*.
 *
 * @param script The script engine to operate on.
 * @return The variable_06 of the current team.
 */
uint16 Script_Team_GetVariable6(ScriptEngine *script)
{
	VARIABLE_NOT_USED(script);
	return g_scriptCurrentTeam->minMembers;
}

/**
 * Gets the target for the current team.
 *
 * Stack: *none*.
 *
 * @param script The script engine to operate on.
 * @return The encoded target.
 */
uint16 Script_Team_GetTarget(ScriptEngine *script)
{
	VARIABLE_NOT_USED(script);
	return g_scriptCurrentTeam->target;
}

/**
 * Tries to add the closest unit to the current team.
 *
 * Stack: *none*.
 *
 * @param script The script engine to operate on.
 * @return The amount of space left in current team.
 */
uint16 Script_Team_AddClosestUnit(ScriptEngine *script)
{
	Team *t;
	Unit *closest = NULL;
	Unit *closest2 = NULL;
	uint16 minDistance = 0;
	uint16 minDistance2 = 0;
	PoolFindStruct find;

	VARIABLE_NOT_USED(script);

	t = g_scriptCurrentTeam;

	if (t->members >= t->maxMembers) return 0;

	/* A wave does not grow while it is still a wave.
	 *
	 * Recruiting during the run is what dissolved attacks into a stream between
	 * the two bases: every unit leaving a factory joined whichever team was out
	 * and trickled after it alone.  A team that has a target keeps the cohort it
	 * left with -- but only while that cohort is still at strength.  Once it has
	 * been ground below its minimum the wave is spent, and the survivors are the
	 * seed of the next one rather than a team that can never fill again. */
	if (Skirmish_IsActive() && t->target != 0 && t->members >= t->minMembers) return 0;

	/* A spent wave takes a fresh objective with its new cohort. */
	if (Skirmish_IsActive() && t->members == 0) t->target = 0;

	find.houseID = t->houseID;
	find.index   = 0xFFFF;
	find.type    = 0xFFFF;

	while (true) {
		Unit *u;
		Team *t2;
		uint16 distance;

		u = Unit_Find(&find);
		if (u == NULL) break;
		/* Only scenario units may be recruited: the campaign feeds its AI teams
		 * from the INI and from reinforcements, so factory output never joins a
		 * team.  A skirmish AI has no scenario to draw on -- keeping the filter
		 * there means its teams stay empty and nobody ever attacks. */
		if (!u->o.flags.s.byScenario && !Skirmish_IsActive()) continue;
		if (u->o.type == UNIT_SABOTEUR) continue;
		if (g_table_unitInfo[u->o.type].movementType != t->movementType) continue;
		if (u->team == 0) {
			distance = Tile_GetDistance(t->position, u->o.position);
			if (distance >= minDistance && minDistance != 0) continue;
			minDistance = distance;
			closest = u;
			continue;
		}

		t2 = Team_Get_ByIndex(u->team - 1);
		if (t2->members > t2->minMembers) continue;

		distance = Tile_GetDistance(t->position, u->o.position);
		if (distance >= minDistance2 && minDistance2 != 0) continue;
		minDistance2 = distance;
		closest2 = u;
	}

	if (closest == NULL) closest = closest2;
	if (closest == NULL) return 0;

	Unit_RemoveFromTeam(closest);
	return Unit_AddToTeam(closest, t);
}

/**
 * Gets the average distance between current team members, and set the
 *  position of the team to the average position.
 *
 * Stack: *none*.
 *
 * @param script The script engine to operate on.
 * @return The average distance.
 */
uint16 Script_Team_GetAverageDistance(ScriptEngine *script)
{
	uint16 averageX = 0;
	uint16 averageY = 0;
	uint16 count = 0;
	uint16 distance = 0;
	Team *t;
	PoolFindStruct find;

	VARIABLE_NOT_USED(script);

	t = g_scriptCurrentTeam;

	find.houseID = t->houseID;
	find.index   = 0xFFFF;
	find.type    = 0xFFFF;

	while (true) {
		Unit *u;

		u = Unit_Find(&find);
		if (u == NULL) break;
		if (t->index != u->team - 1) continue;
		count++;
		averageX += (u->o.position.x >> 8) & 0x3f;
		averageY += (u->o.position.y >> 8) & 0x3f;
	}

	if (count == 0) return 0;
	averageX /= count;
	averageY /= count;

	Tile_MakeXY(t->position, averageX, averageY);

	find.houseID = t->houseID;
	find.index   = 0xFFFF;
	find.type    = 0xFFFF;

	while (true) {
		Unit *u;

		u = Unit_Find(&find);
		if (u == NULL) break;
		if (t->index != u->team - 1) continue;
		distance += Tile_GetDistanceRoundedUp(u->o.position, t->position);
	}

	distance /= count;

	if (t->target == 0 || t->targetTile == 0) return distance;

	if (Tile_GetDistancePacked(Tile_PackXY(averageX, averageY), Tools_Index_GetPackedTile(t->target)) <= 10) t->targetTile = 2;

	return distance;
}

/**
 * Unknown function 0543.
 *
 * Stack: 1 - A distance.
 *
 * @param script The script engine to operate on.
 * @return The number of moving units.
 */
uint16 Script_Team_Unknown0543(ScriptEngine *script)
{
	Team *t;
	uint16 count = 0;
	uint16 distance;
	PoolFindStruct find;

	uint16 order[TEAM_MEMBERS_MAX];
	uint16 members = 0;
	uint16 rallyPacked;
	uint16 i;

	t = g_scriptCurrentTeam;
	distance = STACK_PEEK(1);
	rallyPacked = Tile_PackTile(t->position);

	find.houseID = t->houseID;
	find.index   = 0xFFFF;
	find.type    = 0xFFFF;

	while (members < TEAM_MEMBERS_MAX) {
		Unit *u;

		u = Unit_Find(&find);
		if (u == NULL) break;
		if (t->index != u->team - 1) continue;

		order[members++] = u->o.index;
	}

	/* Same positioning as the attack below: a stragger is called in to a tile of
	 * its own next to the team's centre of mass.  It used to be a fresh random
	 * tile within the gather radius each time the script looked, which is why a
	 * team forming up wandered instead of closing ranks. */
	UnitSelection_SortOrderByDistance(order, members, rallyPacked);
	UnitSelection_SpreadReset();

	for (i = 0; i < members; i++) {
		Unit *u = Unit_Get_ByIndex(order[i]);
		tile32 tile;
		uint16 distanceUnitDest;
		uint16 distanceUnitTeam;
		uint16 distanceTeamDest;

		tile = Tools_Index_GetTile(u->targetMove);
		distanceUnitTeam = Tile_GetDistanceRoundedUp(u->o.position, t->position);

		if (u->targetMove != 0) {
			distanceUnitDest = Tile_GetDistanceRoundedUp(u->o.position, tile);
			distanceTeamDest = Tile_GetDistanceRoundedUp(t->position, tile);
		} else {
			distanceUnitDest = 64;
			distanceTeamDest = 64;
		}

		if ((distanceUnitDest < distanceTeamDest && (distance + 2) < distanceUnitTeam) || (distanceUnitDest >= distanceTeamDest && distanceUnitTeam > distance)) {
			Unit_SetAction(u, ACTION_MOVE);

			Unit_SetDestination(u, Tools_Index_Encode(UnitSelection_SpreadTake(u, rallyPacked), IT_TILE));
			count++;
			continue;
		}

		Unit_SetAction(u, ACTION_GUARD);
	}

	return count;
}

/**
 * Gets the best target for the current team.
 *
 * Stack: *none*.
 *
 * @param script The script engine to operate on.
 * @return The encoded index of the best target or 0 if none found.
 */
uint16 Script_Team_FindBestTarget(ScriptEngine *script)
{
	Team *t;
	PoolFindStruct find;

	VARIABLE_NOT_USED(script);

	t = g_scriptCurrentTeam;

	/* Hold fire until the house is ready to attack with everything at once. */
	if (Skirmish_IsActive() && t->target == 0 && !Skirmish_AI_WaveReady(t->houseID)) return 0;

	find.houseID = t->houseID;
	find.index   = 0xFFFF;
	find.type    = 0xFFFF;

	while (true) {
		Unit *u;
		uint16 target;

		u = Unit_Find(&find);
		if (u == NULL) break;
		if (u->team - 1 != t->index) continue;
		target = Unit_FindBestTargetEncoded(u, t->action == TEAM_ACTION_KAMIKAZE ? 4 : 0);
		if (target == 0) continue;
		if (t->target == target) return target;

		t->target = target;
		t->targetTile = Tile_GetTileInDirectionOf(Tile_PackTile(u->o.position), Tools_Index_GetPackedTile(target));
		return target;
	}

	return 0;
}

/**
 * Loads a new script for the current team.
 *
 * Stack: 1 - The script type.
 *
 * @param script The script engine to operate on.
 * @return The value 0. Always.
 */
uint16 Script_Team_Load(ScriptEngine *script)
{
	Team *t;
	uint16 type;

	t = g_scriptCurrentTeam;
	type = STACK_PEEK(1);

	if (t->action == type) return 0;

	t->action = type;

	Script_Reset(&t->script, g_scriptTeam);
	Script_Load(&t->script, type & 0xFF);

	return 0;
}

/**
 * Loads a new script for the current team.
 *
 * Stack: *none*.
 *
 * @param script The script engine to operate on.
 * @return The value 0. Always.
 */
uint16 Script_Team_Load2(ScriptEngine *script)
{
	Team *t;
	uint16 type;

	VARIABLE_NOT_USED(script);

	t = g_scriptCurrentTeam;
	type = t->actionStart;

	if (t->action == type) return 0;

	t->action = type;

	Script_Reset(&t->script, g_scriptTeam);
	Script_Load(&t->script, type & 0xFF);

	return 0;
}

/**
 * Unknown function 0788.
 *
 * Stack: *none*.
 *
 * @param script The script engine to operate on.
 * @return The value 0. Always.
 */
uint16 Script_Team_Unknown0788(ScriptEngine *script)
{
	Team *t;
	tile32 tile;
	uint16 order[TEAM_MEMBERS_MAX];
	uint16 count = 0;
	uint16 targetPacked;
	uint16 i;
	PoolFindStruct find;

	VARIABLE_NOT_USED(script);

	t = g_scriptCurrentTeam;
	if (t->target == 0) return 0;

	tile = Tools_Index_GetTile(t->target);
	targetPacked = Tile_PackTile(tile);

	find.houseID = t->houseID;
	find.index   = 0xFFFF;
	find.type    = 0xFFFF;

	while (count < TEAM_MEMBERS_MAX) {
		Unit *u;

		u = Unit_Find(&find);
		if (u == NULL) break;
		if (u->team - 1 != t->index) continue;

		order[count++] = u->o.index;
	}

	/* Give the team the same firing line the player's group orders get.  The
	 * original picked each unit's stand-off tile as a quadrant of the bearing
	 * plus up to half a turn of noise, re-rolled on every script tick, and fell
	 * back to the target's own tile whenever that landed on something -- so the
	 * team converged into a shoving heap on top of what it was shooting at and
	 * never settled.  Sorted by distance and one claimed tile each, they arrive
	 * as an arc facing the target and stay put. */
	UnitSelection_SortOrderByDistance(order, count, targetPacked);
	UnitSelection_SpreadReset();

	for (i = 0; i < count; i++) {
		Unit *u = Unit_Get_ByIndex(order[i]);
		uint16 reach = g_table_unitInfo[u->o.type].fireDistance;
		uint16 mine = 0;
		uint16 distance;
		uint16 packed;

		/* Clear the way, then do the job.
		 *
		 * A team picks its objective once -- Script_Team_FindBestTarget() keeps
		 * the one it has -- and this routine used to put every member on it, so a
		 * team that had settled on a factory drove the length of the defence line
		 * to reach it and died without ever returning fire.  Anything in the way
		 * counts, at one priority: enemy units, harvesters, turrets.
		 *
		 * Three tiles beyond the unit's own weapon range, so a unit meeting
		 * something head-on has acquired it before either is in range and gets to
		 * shoot first rather than being shot at.  What it picks it keeps until
		 * that is dead or has left, and only then does the objective come back --
		 * re-deciding every tick is what turned attacks into a smear. */
		if (Tools_Index_IsValid(u->targetAttack)
			&& Tile_GetDistance(u->o.position, Tools_Index_GetTile(u->targetAttack)) <= ((reach + 5) << 8)) {
			mine = u->targetAttack;
		}

		if (mine == 0) mine = Unit_Autonomy_FindTargetWithin(u, reach + 3);
		if (mine == 0) mine = t->target;

		tile = Tools_Index_GetTile(mine);

		distance = reach << 8;
		if (u->actionID == ACTION_ATTACK && u->targetAttack == mine) {
			if (u->targetMove != 0) continue;
			if (Tile_GetDistance(u->o.position, tile) >= distance) continue;
		}

		if (u->actionID != ACTION_ATTACK) Unit_SetAction(u, ACTION_ATTACK);

		/* Stand off along the bearing the unit is already on, so nobody crosses
		 * the target to reach its post. */
		packed = Tile_PackTile(Tile_MoveByDirection(tile, Tile_GetDirection(tile, u->o.position), distance));
		packed = UnitSelection_SpreadTake(u, packed);

		Unit_SetDestination(u, Tools_Index_Encode(packed, IT_TILE));
		Unit_SetTarget(u, mine);
	}

	return 0;
}

/**
 * Draws a string.
 *
 * Stack: 1 - The index of the string to draw.
 *        2-4 - The arguments for the string.
 *
 * @param script The script engine to operate on.
 * @return The value 0. Always.
 */
uint16 Script_Team_DisplayText(ScriptEngine *script)
{
	Team *t;
	char *text;
	uint16 offset;

	t = g_scriptCurrentTeam;
	if (t->houseID == g_playerHouseID) return 0;

	offset = BETOH16(*(script->scriptInfo->text + STACK_PEEK(1)));
	text = (char *)script->scriptInfo->text + offset;

	GUI_DisplayText(text, 0, STACK_PEEK(2), STACK_PEEK(3), STACK_PEEK(4));

	return 0;
}
