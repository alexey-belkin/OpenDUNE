/** @file src/pathfinder.c A* route search over the tile grid.
 *
 * Westwood's own router (Script_Unit_Pathfinder() in script/unit.c) walks
 * straight at the destination and, when the next tile is blocked, gropes along
 * the obstacle clockwise and counter-clockwise for up to a hundred tiles.  It
 * sees exactly one tile ahead, so a unit finds out about a pocket by driving
 * into it.  This is the replacement: a real shortest-path search over all 4096
 * tiles, which cannot walk into a pocket because it has already looked inside
 * every one of them.
 *
 * Three things decide whether it works, and all three are about the cost:
 *
 * 1. **It is measured in game ticks, not in Westwood's score.**
 *    Unit_GetTileEnterScore() ends with `res ^= 0xFF`, turning a speed into a
 *    time by subtracting it from 255.  That is an affine proxy, not a
 *    reciprocal, and with g_dune2_enhanced the speeds are first scaled by
 *    movingSpeedFactor/256 -- which for ground units is between 5/256 and
 *    60/256 -- so every step lands in 211..252 and the terrain is worth a few
 *    percent.  Measured properly a Tank crosses sand in 78 ticks and concrete
 *    in 51 -- a factor of 1.53, which the score reports as 1.06 -- and for a
 *    Soldier the real factor is 2.18 against a reported 1.04.  Diagonals are
 *    the same story: 1.38 in reality, 1.01 in the score.  The old router only
 *    ever compared two ways round one obstacle, so the distortion never
 *    surfaced; a shortest-path search optimises exactly what it is given, so it
 *    would have produced a flawless route through the wrong metric.
 *    Pathfinder_TicksForStep() therefore reproduces what the movement layer
 *    actually does -- Unit_StartMovement() picks the terrain rate of the tile
 *    being *entered*, Unit_SetSpeed() quantises it, and GameLoop_Unit()
 *    advances movement once every three game ticks -- and the self-test checks
 *    the answer by driving a unit across a tile and counting.
 *
 * 2. **The heuristic is derived, not chosen.**  Pathfinder_MaxSpeed() asks the
 *    same cost function for the fastest ground this unit can be on, and the
 *    octile bound is built from it.  A unit cannot beat its own best terrain,
 *    so the estimate can never exceed the truth, whatever the balance module
 *    has done to the tables.  An inadmissible heuristic returns a non-optimal
 *    route silently, which is the one failure nobody would notice.
 *
 * 3. **Ties break on the packed tile index.**  This is a lockstep multiplayer
 *    game: two clients that expand nodes in a different order compute different
 *    routes and the match desyncs.  Nothing here may depend on a pointer value,
 *    on iteration order over a pool, or on the local game speed --
 *    Tools_AdjustToGameSpeed() is deliberately *not* applied, even though the
 *    movement layer applies it, because it reads g_gameConfig.gameSpeed and
 *    that is a local setting.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "types.h"
#include "os/math.h"

#include "pathfinder.h"

#include "house.h"
#include "inifile.h"
#include "map.h"
#include "pool/structure.h"
#include "pool/unit.h"
#include "structure.h"
#include "tile.h"
#include "unit.h"

/** The whole map.  Nothing here is per-match state, so none of it is saved. */
#define PATHFINDER_TILE_COUNT (64 * 64)

/** Unreachable, and the value a cost may never be compared as less than. */
#define PATHFINDER_INFINITE 0xFFFFFFFF

/**
 * How far the search may go before it gives up and answers best-effort.
 *
 * The map is the natural bound and the heuristic keeps a normal search far
 * under it -- an open-field route expands roughly its own length.  The cap
 * exists for the pathological case: a destination walled off from the unit
 * makes the search enumerate every tile it can reach before it can say so, and
 * that happens per unit per re-path.
 */
#define PATHFINDER_EXPANSION_LIMIT PATHFINDER_TILE_COUNT

/** Tile index change when moving in a direction.  Same table as script/unit.c. */
static const int16 s_mapDirection[8] = {-64, -63, 1, 65, 64, 63, -1, -65};

/** Column change per direction, so a step off the west edge cannot wrap east. */
static const int8 s_directionX[8] = { 0,  1, 1, 1, 0, -1, -1, -1};
static const int8 s_directionY[8] = {-1, -1, 0, 1, 1,  1,  0, -1};

/**
 * Whether the A* search runs at all.
 *
 * Off is Westwood's router, unchanged.  Being able to switch between the two on
 * one binary is what makes the change measurable: --war-metrics and
 * --economy-baseline can be run twice without rebuilding.
 */
static bool s_enabled = true;

static uint32 s_gScore[PATHFINDER_TILE_COUNT];
static uint8  s_cameFrom[PATHFINDER_TILE_COUNT];
static uint8  s_closed[PATHFINDER_TILE_COUNT];

/* The open set is a binary heap of tile indices with the key held per tile, so
 * a tile is in it at most once and a cheaper route to it updates in place.
 * That keeps the heap bounded by the map and, more to the point, keeps the pop
 * order a function of (key, tile) alone. */
static uint16 s_heap[PATHFINDER_TILE_COUNT];
static uint16 s_heapPos[PATHFINDER_TILE_COUNT];
static uint32 s_heapKey[PATHFINDER_TILE_COUNT];
static uint16 s_heapSize;

/* Clearing forty kilobytes per search would cost more than most searches do.
 * A generation stamp retires the previous run instead; the arrays above are
 * only meaningful where the stamp is current. */
static uint16 s_generation[PATHFINDER_TILE_COUNT];
static uint16 s_generationCurrent;
static uint16 s_goalStamp[PATHFINDER_TILE_COUNT];

static uint16 s_packedSrc;
static uint32 s_hStraight;
static uint32 s_hDiagonal;
static uint16 s_goalMinX;
static uint16 s_goalMaxX;
static uint16 s_goalMinY;
static uint16 s_goalMaxY;
static uint16 s_nearestGoal;
static uint16 s_bestEffort;
static uint32 s_bestEffortH;
static uint32 s_expansions;

/** Scratch for unwinding a route; a path may be longer than any route buffer. */
static uint8 s_unwind[PATHFINDER_TILE_COUNT];

void Pathfinder_Init(void)
{
	s_enabled = (IniFile_GetInteger("pathfinder_astar", 1) != 0);
}

/** Both settings of the rule, for the self-test that has to see both answers. */
void Pathfinder_SetEnabled(bool enabled)
{
	s_enabled = enabled;
}

bool Pathfinder_IsEnabled(void)
{
	return s_enabled;
}

uint32 Pathfinder_GetExpansions(void)
{
	return s_expansions;
}

/** The value Pathfinder_StepCost() returns for a step that may not be taken. */
uint32 Pathfinder_Unreachable(void)
{
	return PATHFINDER_INFINITE;
}

/**
 * The rate this unit would move at on a given landscape, in engine speed units.
 *
 * This is Unit_StartMovement() followed by Unit_SetSpeed(), in that order and
 * with the same rounding: the terrain rate of the tile being entered, the
 * quarter off a damaged unit, the harvester's cargo, and finally the unit's own
 * movingSpeedFactor.  Zero means the unit cannot move there at all -- and note
 * that a rate which survives the terrain but is rounded away by the factor is
 * genuinely stuck, because Unit_SetSpeed() then leaves speedPerTick at 0 and
 * Unit_MovementTick()'s accumulator never overflows.
 *
 * Tools_AdjustToGameSpeed() is left out on purpose; see the file header.
 */
static uint16 Pathfinder_SpeedOnLandscape(const Unit *unit, uint16 type)
{
	const UnitInfo *ui;
	uint16 speed;

	ui = &g_table_unitInfo[unit->o.type];

	if (type == LST_STRUCTURE) type = LST_CONCRETE_SLAB;
	if (type >= LST_MAX) return 0;

	speed = g_table_landscapeInfo[type].movementSpeed[ui->movementType];

	/* A Saboteur walks through walls, and does it at full speed. */
	if (unit->o.type == UNIT_SABOTEUR && type == LST_WALL) speed = 255;

	if (speed == 0) return 0;

	if ((ui->o.hitpoints / 2) > unit->o.hitpoints && ui->movementType != MOVEMENT_WINGER) speed -= speed / 4;

	if (unit->o.type == UNIT_HARVESTER) speed = ((255 - unit->amount) * speed) / 256;

	return (uint16)(ui->movingSpeedFactor * speed / 256);
}

/**
 * Game ticks to cross one tile at a given rate.
 *
 * Reproducing what the movement layer does rather than what it looks like it
 * does, because the difference is large and it runs the wrong way.  Three
 * quirks, and the measurement in the self-test exists because the first of them
 * was missed on the first attempt:
 *
 *  - **The rate is quantised.**  Unit_SetSpeed() splits the rate into
 *    `speed = v >> 4` whole sixteenths of a tile per move and `speedPerTick`,
 *    and above sixteen it pins speedPerTick at 255 and throws the low four bits
 *    of the rate away.  A Trike at 28 and a Trike at 31 travel at the same
 *    speed.  This is why concrete is worth less than the raw rates suggest: a
 *    Tank's 24 on concrete quantises down to 16, the same as its 15 on rock.
 *  - **A move is a whole sixteenth of a tile**, so a tile takes a whole number
 *    of them, and Unit_MovementTick()'s accumulator lets one through when it
 *    passes 256.
 *  - **Arrival is early.**  Unit_Move() stops when Tile_GetDistance() to the
 *    destination drops under sixteen, and that is the `max + min/2` metric --
 *    384 for a diagonal against its real length of 256*sqrt(2) = 362 -- so a
 *    diagonal step finishes a fraction sooner than its length implies.
 *
 * GameLoop_Unit() runs the movement tick once every three game ticks, which is
 * the factor of three.
 */
static uint32 Pathfinder_TicksForStep(uint16 speed, bool diagonal)
{
	uint32 distance;
	uint32 metric;
	uint32 perMove;
	uint32 rate;
	uint32 moves;

	if (speed == 0) return PATHFINDER_INFINITE;

	if ((speed >> 4) != 0) {
		perMove = (uint32)(speed >> 4) * 16;
		rate    = 255;
	} else {
		perMove = 16;
		rate    = (uint32)speed << 4;
	}

	distance = diagonal ? 362 : 256;
	metric   = diagonal ? 384 : 256;

	moves = distance * (metric - 16) / (metric * perMove) + 1;

	return 3 * ((moves * 256 + rate - 1) / rate);
}

uint32 Pathfinder_StepTicks(Unit *unit, uint16 packed, uint8 orient8)
{
	if (unit == NULL) return PATHFINDER_INFINITE;

	return Pathfinder_TicksForStep(Pathfinder_SpeedOnLandscape(unit, Map_GetLandscapeType(packed)), (orient8 & 1) != 0);
}

/**
 * Whether a tile refused by Unit_GetTileEnterScore() is merely busy right now.
 *
 * An allied unit standing on a tile is a wall; an allied unit *crossing* it is
 * traffic.  The distinction matters because the old router looked one tile
 * ahead and so flowed round a column by accident, while a search that plans
 * forty tiles at once would otherwise commit to a detour around somebody who
 * will have moved on in three ticks.
 *
 * Deliberately narrow.  An enemy unit is never transient -- pathing through an
 * enemy formation is not a thing the unit can do -- and a tile holding a
 * structure is never transient either.  Note that a moving unit holds two tiles,
 * the one it is on and the one it reserved in Unit_StartMovement(), and both
 * answer true here: both are free again within one step.
 */
static bool Pathfinder_BlockerIsTransient(Unit *unit, uint16 packed)
{
	Unit *blocker;

	if (Structure_Get_ByPackedTile(packed) != NULL) return false;

	blocker = Unit_Get_ByPackedTile(packed);
	if (blocker == NULL || blocker == unit) return false;
	if (blocker->speed == 0) return false;

	return House_AreAllied(Unit_GetHouseID(blocker), Unit_GetHouseID(unit));
}

/**
 * Cost in game ticks of stepping onto `packed` from direction `orient8`, or
 * PATHFINDER_INFINITE when the search may not.
 *
 * Passability is Unit_GetTileEnterScore()'s answer, because it is the one the
 * movement layer will ask again at the moment of commitment and it knows about
 * allies, transports, conquerable buildings, the Saboteur and the sandworm.
 * Only the *number* is recomputed here.
 *
 * The negative returns are the trap.  Unit_GetTileEnterScore() answers -1 for a
 * structure the unit may drive up to but not into, and -2 for one it may enter;
 * script/unit.c's wrapper maps -1 to 256 and then lets -2 through as a negative
 * score, which was harmless when the score only chose between two detours and
 * is not harmless at all in a search that assumes non-negative edges.  So: -2
 * is passable, everything else negative is a wall, and no negative number ever
 * reaches the cost.
 */
uint32 Pathfinder_StepCost(Unit *unit, uint16 packed, uint8 orient8)
{
	int16 score;
	uint16 speed;
	uint32 ticks;
	bool blocked = false;

	score = Unit_GetTileEnterScore(unit, packed, orient8);

	if (score < 0) {
		if (score != -2) return PATHFINDER_INFINITE;
	} else if (score > 255) {
		if (!Pathfinder_BlockerIsTransient(unit, packed)) return PATHFINDER_INFINITE;
		blocked = true;
	}

	speed = Pathfinder_SpeedOnLandscape(unit, Map_GetLandscapeType(packed));
	if (speed == 0) return PATHFINDER_INFINITE;

	ticks = Pathfinder_TicksForStep(speed, (orient8 & 1) != 0);

	/* Traffic is charged, not waved through.  Free passage would send every
	 * route straight down the middle of a jam; roughly a step of waiting means
	 * the detour wins exactly when the detour is in fact shorter. */
	if (blocked) ticks += ticks;

	return ticks;
}

/**
 * The fastest this unit can possibly move, over any ground it can be on.
 *
 * This is what makes the heuristic provably admissible without a constant
 * anybody has to keep in step with the balance module: the bound comes out of
 * the same table the cost does.
 */
static uint16 Pathfinder_MaxSpeed(const Unit *unit)
{
	uint16 type;
	uint16 best = 0;

	for (type = 0; type < LST_MAX; type++) {
		uint16 speed = Pathfinder_SpeedOnLandscape(unit, type);

		if (speed > best) best = speed;
	}

	return best;
}

bool Pathfinder_CheckHeuristic(Unit *unit, uint16 *failedLandscape)
{
	uint16 type;
	uint16 vmax;
	uint32 straight;
	uint32 diagonal;

	if (unit == NULL) return false;

	vmax = Pathfinder_MaxSpeed(unit);
	if (vmax == 0) return false;

	straight = Pathfinder_TicksForStep(vmax, false);
	diagonal = Pathfinder_TicksForStep(vmax, true);

	/* The octile combination is only a lower bound while a diagonal step is
	 * cheaper than the two straight ones it replaces. */
	if (diagonal > 2 * straight) {
		if (failedLandscape != NULL) *failedLandscape = LST_MAX;
		return false;
	}

	for (type = 0; type < LST_MAX; type++) {
		uint16 speed = Pathfinder_SpeedOnLandscape(unit, type);

		if (speed == 0) continue;

		if (Pathfinder_TicksForStep(speed, false) < straight || Pathfinder_TicksForStep(speed, true) < diagonal) {
			if (failedLandscape != NULL) *failedLandscape = type;
			return false;
		}
	}

	return true;
}

/**
 * Octile distance from a tile to the goal box, in game ticks.
 *
 * A box rather than a point because the callers that matter ask about a ring of
 * candidate tiles round one target, not about a single destination.  A bound to
 * the enclosing box is a bound to every goal inside it, so one search answers
 * for all of them, and it stays consistent -- one step cannot reduce it by more
 * than that step costs -- which is what makes a closed tile's score final.
 */
static uint32 Pathfinder_Heuristic(uint16 packed)
{
	uint32 dx = 0;
	uint32 dy = 0;
	uint32 lo;
	uint32 hi;
	uint16 x = Tile_GetPackedX(packed);
	uint16 y = Tile_GetPackedY(packed);

	if (x < s_goalMinX) dx = s_goalMinX - x;
	if (x > s_goalMaxX) dx = x - s_goalMaxX;
	if (y < s_goalMinY) dy = s_goalMinY - y;
	if (y > s_goalMaxY) dy = y - s_goalMaxY;

	lo = min(dx, dy);
	hi = max(dx, dy);

	return s_hDiagonal * lo + s_hStraight * (hi - lo);
}

/**
 * Order on the open set: cheapest estimate first, and on a tie the lower tile
 * index.  The tile index is unique, so there are no ties left after it and the
 * expansion order is a function of the map alone.  That is the whole of the
 * determinism argument.
 */
static bool Pathfinder_HeapBefore(uint16 a, uint16 b)
{
	if (s_heapKey[a] != s_heapKey[b]) return s_heapKey[a] < s_heapKey[b];

	return a < b;
}

static void Pathfinder_HeapSiftUp(uint16 index)
{
	uint16 tile = s_heap[index];

	while (index > 0) {
		uint16 parent = (uint16)((index - 1) / 2);

		if (!Pathfinder_HeapBefore(tile, s_heap[parent])) break;

		s_heap[index] = s_heap[parent];
		s_heapPos[s_heap[index]] = index;
		index = parent;
	}

	s_heap[index] = tile;
	s_heapPos[tile] = index;
}

static void Pathfinder_HeapSiftDown(uint16 index)
{
	uint16 tile = s_heap[index];

	while (true) {
		uint16 child = (uint16)(index * 2 + 1);

		if (child >= s_heapSize) break;
		if (child + 1 < s_heapSize && Pathfinder_HeapBefore(s_heap[child + 1], s_heap[child])) child++;
		if (!Pathfinder_HeapBefore(s_heap[child], tile)) break;

		s_heap[index] = s_heap[child];
		s_heapPos[s_heap[index]] = index;
		index = child;
	}

	s_heap[index] = tile;
	s_heapPos[tile] = index;
}

static void Pathfinder_HeapPush(uint16 tile, uint32 key)
{
	s_heapKey[tile] = key;

	if (s_generation[tile] == s_generationCurrent && s_heapPos[tile] != 0xFFFF) {
		Pathfinder_HeapSiftUp(s_heapPos[tile]);
		return;
	}

	s_heap[s_heapSize] = tile;
	s_heapPos[tile] = s_heapSize;
	s_heapSize++;
	Pathfinder_HeapSiftUp((uint16)(s_heapSize - 1));
}

static uint16 Pathfinder_HeapPop(void)
{
	uint16 tile = s_heap[0];

	s_heapPos[tile] = 0xFFFF;
	s_heapSize--;

	if (s_heapSize > 0) {
		s_heap[0] = s_heap[s_heapSize];
		s_heapPos[s_heap[0]] = 0;
		Pathfinder_HeapSiftDown(0);
	}

	return tile;
}

/** Retire the previous search without walking the arrays. */
static void Pathfinder_BeginGeneration(void)
{
	s_generationCurrent++;

	if (s_generationCurrent == 0) {
		memset(s_generation, 0, sizeof(s_generation));
		memset(s_goalStamp, 0, sizeof(s_goalStamp));
		s_generationCurrent = 1;
	}

	s_heapSize = 0;
}

/** Bring a tile into the current search, unvisited. */
static void Pathfinder_TouchTile(uint16 packed)
{
	if (s_generation[packed] == s_generationCurrent) return;

	s_generation[packed] = s_generationCurrent;
	s_gScore[packed]  = PATHFINDER_INFINITE;
	s_cameFrom[packed] = 0xFF;
	s_closed[packed]  = 0;
	s_heapPos[packed] = 0xFFFF;
}

/**
 * Search from `packedSrc` for the cheapest of `goals`.
 *
 * With stopAtFirstGoal the search returns as soon as one goal is settled, and
 * that goal is the cheapest of the set: the first goal popped minimises
 * g + h and h is zero on a goal, so no other goal can still turn out cheaper.
 * Without it the search runs until every goal is settled, which is what a caller
 * needs when it mixes the travel time with something else before choosing.
 *
 * Afterwards Pathfinder_GetTicks() answers for any settled tile and
 * Pathfinder_GetRoute() unwinds one.  The workspace lives until the next call,
 * so a caller must finish reading before it starts another search.
 */
bool Pathfinder_Run(Unit *unit, uint16 packedSrc, const uint16 *goals, uint16 goalCount, bool stopAtFirstGoal)
{
	uint16 vmax;
	uint16 i;
	uint16 remaining = 0;
	bool found = false;

	s_nearestGoal = 0xFFFF;
	s_bestEffort  = 0xFFFF;
	s_bestEffortH = PATHFINDER_INFINITE;
	s_expansions  = 0;
	s_packedSrc   = packedSrc;

	if (unit == NULL || goals == NULL || goalCount == 0) return false;
	if (!Map_IsValidPosition(packedSrc)) return false;

	vmax = Pathfinder_MaxSpeed(unit);
	if (vmax == 0) return false;

	s_hStraight = Pathfinder_TicksForStep(vmax, false);
	s_hDiagonal = Pathfinder_TicksForStep(vmax, true);

	Pathfinder_BeginGeneration();

	s_goalMinX = 0x3F;
	s_goalMaxX = 0;
	s_goalMinY = 0x3F;
	s_goalMaxY = 0;

	for (i = 0; i < goalCount; i++) {
		uint16 x;
		uint16 y;

		if (!Map_IsValidPosition(goals[i])) continue;
		if (s_goalStamp[goals[i]] == s_generationCurrent) continue;

		s_goalStamp[goals[i]] = s_generationCurrent;
		remaining++;

		x = Tile_GetPackedX(goals[i]);
		y = Tile_GetPackedY(goals[i]);
		if (x < s_goalMinX) s_goalMinX = x;
		if (x > s_goalMaxX) s_goalMaxX = x;
		if (y < s_goalMinY) s_goalMinY = y;
		if (y > s_goalMaxY) s_goalMaxY = y;
	}

	if (remaining == 0) return false;

	Pathfinder_TouchTile(packedSrc);
	s_gScore[packedSrc] = 0;
	Pathfinder_HeapPush(packedSrc, Pathfinder_Heuristic(packedSrc));

	while (s_heapSize != 0 && s_expansions < PATHFINDER_EXPANSION_LIMIT) {
		uint16 current = Pathfinder_HeapPop();
		uint32 h;
		uint8 direction;

		s_closed[current] = 1;
		s_expansions++;

		/* The tile that got closest, for the caller that would rather set off
		 * towards an unreachable destination than stand still -- which is what
		 * Westwood's router did by returning a partial route, and what several
		 * layers above quietly depend on.  Ties go to the lower tile index so
		 * the answer does not depend on expansion order either. */
		h = Pathfinder_Heuristic(current);
		if (h < s_bestEffortH || (h == s_bestEffortH && current < s_bestEffort)) {
			s_bestEffortH = h;
			s_bestEffort  = current;
		}

		if (s_goalStamp[current] == s_generationCurrent) {
			if (!found) s_nearestGoal = current;
			found = true;
			remaining--;
			if (stopAtFirstGoal || remaining == 0) break;
		}

		for (direction = 0; direction < 8; direction++) {
			uint16 neighbour;
			uint32 cost;
			uint32 tentative;
			int16 nx = (int16)Tile_GetPackedX(current) + s_directionX[direction];
			int16 ny = (int16)Tile_GetPackedY(current) + s_directionY[direction];

			/* Explicit column and row arithmetic: s_mapDirection is a flat
			 * offset, so a step west off column zero would land on the east
			 * edge of the row above without ever leaving the array. */
			if (nx < 0 || nx > 63 || ny < 0 || ny > 63) continue;

			neighbour = (uint16)(current + s_mapDirection[direction]);

			Pathfinder_TouchTile(neighbour);
			if (s_closed[neighbour] != 0) continue;

			cost = Pathfinder_StepCost(unit, neighbour, direction);
			if (cost == PATHFINDER_INFINITE) continue;

			tentative = s_gScore[current] + cost;
			if (tentative >= s_gScore[neighbour]) continue;

			s_gScore[neighbour]  = tentative;
			s_cameFrom[neighbour] = direction;
			Pathfinder_HeapPush(neighbour, tentative + Pathfinder_Heuristic(neighbour));
		}
	}

	return found;
}

bool Pathfinder_GetTicks(uint16 packed, uint32 *ticks)
{
	if (packed >= PATHFINDER_TILE_COUNT) return false;
	if (s_generation[packed] != s_generationCurrent) return false;
	if (s_closed[packed] == 0) return false;
	if (s_gScore[packed] == PATHFINDER_INFINITE) return false;

	if (ticks != NULL) *ticks = s_gScore[packed];

	return true;
}

/**
 * The estimate the last search was using, for the tile given.
 *
 * Exposed so the self-test can hold it against independently computed true
 * distances rather than against the argument that it ought to be a lower bound.
 * Checking the ingredients is not enough: a mistake in the octile combination
 * itself would pass that and still return non-optimal routes in silence.
 */
uint32 Pathfinder_GetHeuristic(uint16 packed)
{
	if (packed >= PATHFINDER_TILE_COUNT) return 0;

	return Pathfinder_Heuristic(packed);
}

uint16 Pathfinder_GetBestEffortTile(void)
{
	return s_bestEffort;
}

/**
 * Write the route to a settled tile as direction bytes, terminated by 0xFF.
 *
 * Truncation is expected and costs nothing: Unit.route holds fourteen bytes,
 * the unit walks them and asks again, and because the next search starts from
 * where it then stands, every recomputation is optimal in its own right.  This
 * is why the buffer never had to grow -- which would have meant the savegame
 * format, for no gain.
 *
 * @return The number of direction bytes written, terminator excluded.
 */
uint16 Pathfinder_GetRoute(uint16 packedGoal, uint8 *buffer, uint16 bufferSize)
{
	uint16 length = 0;
	uint16 packed = packedGoal;
	uint16 written;
	uint16 i;

	if (buffer == NULL || bufferSize == 0) return 0;

	buffer[0] = 0xFF;

	if (!Pathfinder_GetTicks(packedGoal, NULL)) return 0;

	while (packed != s_packedSrc && length < PATHFINDER_TILE_COUNT) {
		uint8 direction = s_cameFrom[packed];

		if (direction > 7) return 0;

		s_unwind[length++] = direction;
		packed = (uint16)(packed - s_mapDirection[direction]);
	}

	if (packed != s_packedSrc) return 0;

	written = min(length, (uint16)(bufferSize - 1));

	for (i = 0; i < written; i++) {
		buffer[i] = s_unwind[length - 1 - i];
	}
	buffer[written] = 0xFF;

	return written;
}
