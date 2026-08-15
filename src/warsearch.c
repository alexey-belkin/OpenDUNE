/** @file src/warsearch.c Search for the economy/army split that wins.
 *
 * The economy search asked how fast a house can refine spice with nothing
 * shooting at it, and answered it -- see [economy.md](economy.md).  This asks
 * the question that only exists once there is an enemy: of everything the
 * economy earns, how much should be spent on the army, and when?
 *
 * The difference matters for how a strategy is scored.  Refined spice is a
 * number a single house can maximise on its own; a split cannot be, because its
 * value depends entirely on what the other house is doing.  So nothing here
 * scores a plan in isolation.  Two strategies are put on one map and the result
 * is who was left standing -- which means the answer is not a number but a
 * matrix, and "is there a counter-strategy" is a question the matrix can
 * actually answer.
 *
 * A strategy is (share, lateShare, switchTick): what percentage of lifetime
 * income may go to war, what it becomes later, and when it changes.  The
 * economy underneath is the tuned one and is the same for everybody, so nothing
 * here is measuring economy quality by accident.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#if defined(_WIN32)
	#include <direct.h>
#endif
#include "types.h"
#include "os/common.h"
#include "os/math.h"

#include "warsearch.h"

#include "house.h"
#include "opendune.h"
#include "pool/house.h"
#include "pool/pool.h"
#include "doctrine.h"
#include "skirmish.h"
#include "structure.h"
#include "team.h"
#include "timer.h"
#include "tools.h"
#include "unit.h"

/** The shares the matrix plays against each other, in percent of income. */
static const uint8 s_shares[] = { 0, 15, 30, 45, 60, 75, 90 };

/**
 * The two Houses a match is played between.
 *
 * They cannot be the same House -- the engine keeps one House record per id --
 * so every pairing is played twice with the sides swapped, and House identity
 * cancels out of the result instead of being mistaken for a strategy.
 */
#define WAR_HOUSE_A HOUSE_ATREIDES
#define WAR_HOUSE_B HOUSE_HARKONNEN

/** How much better one side has to be doing to call it a win rather than a draw. */
#define WAR_MARGIN_PERCENT 20

static bool s_trace = false;

/* What the last duel actually spent, per side, so a share that never binds does
 * not get mistaken for a share that does not matter.  Filled by WarSearch_Match,
 * summed over a duel. */
static uint32 s_spentMilitary[SKIRMISH_PLAYER_MAX];
static uint32 s_income[SKIRMISH_PLAYER_MAX];
/* Matches that ended with a base wiped out rather than on the tick limit.  A
 * duel of nothing but value margins is answering "who was ahead", not "who won",
 * and the difference should be visible rather than assumed. */
static uint16 s_decisive;

void WarSearch_SetTrace(bool trace)
{
	s_trace = trace;
}

static void WarSearch_Print(const char *str)
{
	fprintf(stderr, "%s\n", str);
	fflush(stderr);
}

/**
 * "3/28 [41s elapsed, ~2m10s left]".
 *
 * A round robin is minutes of silence otherwise, and the useful question while
 * waiting is when it ends, not whether it is alive.  The estimate is linear in
 * duels completed, which is honest here: every duel is the same number of
 * matches, and matches that end early average out across the grid.
 */
static void WarSearch_Progress(char *buf, uint16 length, uint16 done, uint16 total, time_t started)
{
	const long elapsed = (long)(time(NULL) - started);
	const long left = (done != 0 && done < total) ? elapsed * (total - done) / done : 0;

	if (left >= 60) {
		snprintf(buf, length, "%u/%u [%lds elapsed, ~%ldm%02lds left]",
		         done, total, elapsed, left / 60, left % 60);
	} else {
		snprintf(buf, length, "%u/%u [%lds elapsed, ~%lds left]", done, total, elapsed, left);
	}
}

void WarSearch_MakePlan(uint16 share, uint16 shareLate, uint32 switchTick, SkirmishEconomyPlan *out)
{
	Skirmish_MakeDefaultPlan(out);

	if (share > 100) share = 100;
	if (shareLate > 100) shareLate = 100;

	out->militaryShare      = (uint8)share;
	out->militaryShareLate  = (uint8)shareLate;
	out->militarySwitchTick = switchTick;
}

/**
 * Play one match and say who won.
 *
 * The clock is taken away from the SDL timer thread for the duration, for the
 * same reason the economy search does it: with it running, how many of its ticks
 * land between two of ours varies with machine load, and the same pair of
 * strategies then produces different matches on consecutive runs.
 *
 * @return 1 when the first plan won, -1 when the second did, 0 for a draw.
 */
static int WarSearch_Match(const SkirmishEconomyPlan *planA, const SkirmishEconomyPlan *planB,
                           uint32 seed, uint32 ticks, uint32 *valueA, uint32 *valueB)
{
	uint32 tick;
	uint32 a, b;

	*valueA = 0;
	*valueB = 0;

	Tools_RandomLCG_Seed((uint16)seed);

	if (!Skirmish_StartWar(WAR_HOUSE_A, WAR_HOUSE_B, seed, planA, planB)) return 0;

	Timer_SetTimer(TIMER_GAME, false);

	for (tick = 0; tick < ticks; tick++) {
		g_timerGame++;

		GameLoop_Team();
		GameLoop_Unit();
		GameLoop_Structure();
		GameLoop_House();

		/* A match that is already decided is only burning CPU; checking is a
		 * structure pool walk, so do it on a coarse cadence. */
		if ((tick & 0x3FF) == 0) {
			if (Skirmish_War_IsDefeated(0) || Skirmish_War_IsDefeated(1)) break;
		}

		if (s_trace && (tick % 20000) == 0) {
			char summary[256];
			char line[300];
			uint8 i;

			for (i = 0; i < SKIRMISH_PLAYER_MAX; i++) {
				if (!Skirmish_GetSummary(i, summary, sizeof(summary))) continue;

				snprintf(line, sizeof(line), "  t%u %s", (unsigned)tick, summary);
				WarSearch_Print(line);
			}
		}
	}

	Timer_SetTimer(TIMER_GAME, true);

	a = Skirmish_War_GetValue(0);
	b = Skirmish_War_GetValue(1);

	*valueA = a;
	*valueB = b;

	s_spentMilitary[0] += Skirmish_War_GetSpent(0, true);
	s_spentMilitary[1] += Skirmish_War_GetSpent(1, true);
	s_income[0] += Skirmish_War_GetIncome(0);
	s_income[1] += Skirmish_War_GetIncome(1);

	/* Losing the last building ends it outright; otherwise the match ran out of
	 * ticks and whoever still holds decisively more of the map's value was
	 * winning.  Anything inside the margin is a draw, not a result. */
	if (Skirmish_War_IsDefeated(1) && !Skirmish_War_IsDefeated(0)) { s_decisive++; return 1; }
	if (Skirmish_War_IsDefeated(0) && !Skirmish_War_IsDefeated(1)) { s_decisive++; return -1; }

	if (a > b + b * WAR_MARGIN_PERCENT / 100) return 1;
	if (b > a + a * WAR_MARGIN_PERCENT / 100) return -1;

	return 0;
}

/**
 * Play a strategy against another over a set of maps, both sides of the table.
 * @return Points for the first strategy: 2 per win, 1 per draw.
 */
static uint16 WarSearch_Duel(const SkirmishEconomyPlan *planA, const SkirmishEconomyPlan *planB,
                             uint32 ticks, uint16 maps, uint32 *forA, uint32 *forB,
                             uint16 *realA, uint16 *realB)
{
	uint32 militaryA = 0, incomeA = 0;
	uint32 militaryB = 0, incomeB = 0;
	uint16 points = 0;
	uint16 map;

	*forA = 0;
	*forB = 0;
	s_decisive = 0;

	for (map = 0; map < maps; map++) {
		const uint32 seed = 1000 + map * 7919;
		uint32 valueA, valueB;
		int result;

		/* Once as the Atreides base, once as the Harkonnen one.  Without this a
		 * result is half strategy and half House: different units, different
		 * damage bonuses and a different corner of the map. */
		memset(s_spentMilitary, 0, sizeof(s_spentMilitary));
		memset(s_income, 0, sizeof(s_income));

		result = WarSearch_Match(planA, planB, seed, ticks, &valueA, &valueB);
		points += (result > 0) ? 2 : ((result == 0) ? 1 : 0);
		*forA += valueA;
		*forB += valueB;
		militaryA += s_spentMilitary[0]; incomeA += s_income[0];
		militaryB += s_spentMilitary[1]; incomeB += s_income[1];

		memset(s_spentMilitary, 0, sizeof(s_spentMilitary));
		memset(s_income, 0, sizeof(s_income));

		result = WarSearch_Match(planB, planA, seed, ticks, &valueB, &valueA);
		points += (result < 0) ? 2 : ((result == 0) ? 1 : 0);
		*forA += valueA;
		*forB += valueB;
		militaryA += s_spentMilitary[1]; incomeA += s_income[1];
		militaryB += s_spentMilitary[0]; incomeB += s_income[0];
	}

	/* What the plan asked for is a ceiling; whether a house got anywhere near it
	 * is a different question, because its factories are slow and its unit cap is
	 * finite.  Measuring the realised share against income is the only way to tell
	 * "this share does not matter" from "this share was never reached". */
	*realA = (uint16)((incomeA != 0) ? militaryA * 100 / incomeA : 0);
	*realB = (uint16)((incomeB != 0) ? militaryB * 100 / incomeB : 0);

	return points;
}

/**
 * Play one match and print a sampled row per house per step, as CSV.
 *
 * This is the only entry point here that is not comparing anything: it exists to
 * make the shape of a single match legible -- when the refineries go up, when the
 * army appears, where the damage is actually being done -- rather than to score
 * it.  Everything is a level except damage and spice, which are running totals so
 * that a rate is an exact difference between two samples rather than whatever the
 * sampler happened to catch.
 */
/** Where recorded matches land, relative to the working directory. */
#define WAR_TELEMETRY_DIR "telemetry"

static void WarSearch_MakeDirectory(const char *path)
{
#if defined(_WIN32)
	_mkdir(path);
#else
	mkdir(path, 0755);
#endif
}

void WarSearch_RunTelemetry(uint8 houseA, uint8 houseB, uint32 ticks, uint16 step, uint16 shareA, uint16 shareB, uint32 switchTick, uint32 seed)
{
	SkirmishEconomyPlan planA, planB;
	char path[256];
	char line[256];
	char row[320];
	const char *winner;
	FILE *fp;
	time_t stamp = time(NULL);
	uint32 valueA, valueB;
	uint32 tick;
	uint8 i;

	if (step == 0) step = 5000;

	WarSearch_MakePlan(shareA, 90, switchTick, &planA);
	WarSearch_MakePlan(shareB, 90, switchTick, &planB);

	Tools_RandomLCG_Seed((uint16)seed);

	if (!Skirmish_StartWar(houseA, houseB, seed, &planA, &planB)) {
		WarSearch_Print("war-telemetry: could not start a match");
		return;
	}

	WarSearch_MakeDirectory(WAR_TELEMETRY_DIR);
	snprintf(path, sizeof(path), "%s/match-%s-%s-%uv%u-s%u-%lu.csv",
	         WAR_TELEMETRY_DIR,
	         g_table_houseInfo[houseA].name, g_table_houseInfo[houseB].name,
	         shareA, shareB, (unsigned)seed, (unsigned long)stamp);

	fp = fopen(path, "w");
	if (fp == NULL) {
		snprintf(row, sizeof(row), "war-telemetry: cannot write %s", path);
		WarSearch_Print(row);
		return;
	}

	/* The metadata a reader needs to tell one recording from another, in comment
	 * lines so the rest of the file is still plain CSV. */
	fprintf(fp, "# recorded %s", ctime(&stamp));
	fprintf(fp, "# houses %s,%s\n", g_table_houseInfo[houseA].name, g_table_houseInfo[houseB].name);
	fprintf(fp, "# shares %u,%u\n", shareA, shareB);
	fprintf(fp, "# shareLate 90,90\n");
	fprintf(fp, "# switchTick %u\n", (unsigned)switchTick);
	fprintf(fp, "# seed %u\n", (unsigned)seed);
	fprintf(fp, "# ticks %u\n", (unsigned)ticks);
	fprintf(fp, "# step %u\n", (unsigned)step);
	{
		char doctrine[32];

		Doctrine_GetSelection(doctrine, sizeof(doctrine));
		fprintf(fp, "# doctrine %s\n", doctrine);
	}
	fprintf(fp, "tick,house,refineries,combatStructures,harvesters,combatUnits,combatHitpoints,damageTaken,spiceRefined,credits,powerSurplus,shotsTurret,shotsStructure,shotsUnit,shotsBypass,idleAttackers,idleOnWave,stalledHarvesters,freeRefineries,wavePhase,waveUnits,waveAtLD,waveColumn,wavesLaunched,wavesAborted,wavesDeclined,inAssault,inMuster,idleGarrison,assaultSpread,builtArt,builtAss,builtRaid,builtGar,pickArt,pickAss,pickRaid,pickGar,vetoArt,vetoAss,vetoRaid,vetoGar,turretZone,turretEntries,harvLost,harvKilled,harvLostEarly,harvNear8,harvWorst,turretKillsLoose,turretKillsAssault,turretsKilled\n");

	Timer_SetTimer(TIMER_GAME, false);

	for (tick = 0; tick <= ticks; tick++) {
		if ((tick % step) == 0) {
			for (i = 0; i < SKIRMISH_PLAYER_MAX; i++) {
				if (!Skirmish_GetTelemetry(i, line, sizeof(line))) continue;

				snprintf(row, sizeof(row), "%u,%s", (unsigned)tick, line);
				WarSearch_Print(row);
				fprintf(fp, "%s\n", row);
			}
		}

		g_timerGame++;

		GameLoop_Team();
		GameLoop_Unit();
		GameLoop_Structure();
		GameLoop_House();
	}

	Timer_SetTimer(TIMER_GAME, true);

	valueA = Skirmish_War_GetValue(0);
	valueB = Skirmish_War_GetValue(1);

	if (Skirmish_War_IsDefeated(1) && !Skirmish_War_IsDefeated(0)) {
		winner = g_table_houseInfo[houseA].name;
	} else if (Skirmish_War_IsDefeated(0) && !Skirmish_War_IsDefeated(1)) {
		winner = g_table_houseInfo[houseB].name;
	} else if (valueA > valueB + valueB * WAR_MARGIN_PERCENT / 100) {
		winner = g_table_houseInfo[houseA].name;
	} else if (valueB > valueA + valueA * WAR_MARGIN_PERCENT / 100) {
		winner = g_table_houseInfo[houseB].name;
	} else {
		winner = "draw";
	}

	/* Trailing rather than leading: the verdict is not known until the match has
	 * been played, and rewinding the file to patch a header in would cost more
	 * than a reader costs to skip to the end. */
	fprintf(fp, "# winner %s\n", winner);
	fprintf(fp, "# value %u,%u\n", (unsigned)valueA, (unsigned)valueB);
	fprintf(fp, "# wipeout %u,%u\n",
	        Skirmish_War_IsDefeated(0) ? 1u : 0u, Skirmish_War_IsDefeated(1) ? 1u : 0u);
	fclose(fp);

	snprintf(row, sizeof(row), "war-telemetry: %s -- winner %s (%u vs %u)", path, winner, (unsigned)valueA, (unsigned)valueB);
	WarSearch_Print(row);
}

/**
 * Every share against every other share.
 *
 * The single most useful output here is not the winner but the shape: if one
 * column beats every other, the balance is a constant and the search is over; if
 * the wins go round in a circle, there is no best split and what a house should
 * spend depends on what its enemy is spending.
 */
void WarSearch_RunMatrix(uint32 ticks, uint16 maps)
{
	const uint16 total = (uint16)(lengthof(s_shares) * (lengthof(s_shares) - 1) / 2);
	uint16 points[lengthof(s_shares)];
	char line[512];
	char progress[64];
	time_t started = time(NULL);
	uint16 done = 0;
	uint16 i, j;

	if (maps < 1) maps = 1;

	memset(points, 0, sizeof(points));

	snprintf(line, sizeof(line), "war-matrix: %u shares round robin, %u ticks, %u maps -- %u matches",
	         (unsigned)lengthof(s_shares), (unsigned)ticks, maps,
	         (unsigned)(lengthof(s_shares) * (lengthof(s_shares) - 1) * maps));
	WarSearch_Print(line);

	for (i = 0; i < lengthof(s_shares); i++) {
		for (j = i + 1; j < lengthof(s_shares); j++) {
			SkirmishEconomyPlan planA, planB;
			uint32 valueA, valueB;
			uint16 realA, realB;
			uint16 scored;

			WarSearch_MakePlan(s_shares[i], s_shares[i], 0, &planA);
			WarSearch_MakePlan(s_shares[j], s_shares[j], 0, &planB);

			scored = WarSearch_Duel(&planA, &planB, ticks, maps, &valueA, &valueB, &realA, &realB);

			points[i] += scored;
			points[j] += (uint16)(maps * 4) - scored;

			WarSearch_Progress(progress, sizeof(progress), ++done, total, started);
			snprintf(line, sizeof(line), "  mil %2u%% vs %2u%%: %u-%u points, value %u-%u, spent %u%%/%u%%, %u/%u by wipeout  %s",
			         s_shares[i], s_shares[j], scored, (unsigned)(maps * 4) - scored,
			         (unsigned)valueA, (unsigned)valueB, realA, realB,
			         s_decisive, (unsigned)(maps * 2), progress);
			WarSearch_Print(line);
		}
	}

	WarSearch_Print("war-matrix standings (2 points a win, 1 a draw):");

	for (i = 0; i < lengthof(s_shares); i++) {
		snprintf(line, sizeof(line), "  mil %2u%% -> %u points of %u",
		         s_shares[i], points[i], (unsigned)((lengthof(s_shares) - 1) * maps * 4));
		WarSearch_Print(line);
	}
}

/**
 * Does the split want to change over the match?
 *
 * Every schedule here spends the same total share on average; what differs is
 * when.  If timing does not matter, they all land on the same score and the
 * balance really is one number.
 */
void WarSearch_RunTiming(uint32 ticks, uint16 maps)
{
	static const struct {
		const char *name;
		uint16 share;
		uint16 shareLate;
		uint32 switchTick;
	} schedules[] = {
		{ "flat 45",        45, 45,      0 },
		{ "arm t10k",        0, 90,  10000 },
		{ "arm t20k",        0, 90,  20000 },
		{ "arm t30k",        0, 90,  30000 },
		{ "arm t40k",        0, 90,  40000 },
		{ "arm t55k",        0, 90,  55000 },
		{ "arm t75k",        0, 90,  75000 },
		{ "arm t100k",       0, 90, 100000 }
	};

	const uint16 total = (uint16)(lengthof(schedules) * (lengthof(schedules) - 1) / 2);
	uint16 points[lengthof(schedules)];
	char line[512];
	char progress[64];
	time_t started = time(NULL);
	uint16 done = 0;
	uint16 i, j;

	if (maps < 1) maps = 1;

	memset(points, 0, sizeof(points));

	snprintf(line, sizeof(line), "war-timing: %u schedules, %u duels of %u matches each",
	         (unsigned)lengthof(schedules), total, maps * 2);
	WarSearch_Print(line);

	for (i = 0; i < lengthof(schedules); i++) {
		for (j = i + 1; j < lengthof(schedules); j++) {
			SkirmishEconomyPlan planA, planB;
			uint32 valueA, valueB;
			uint16 realA, realB;
			uint16 scored;

			WarSearch_MakePlan(schedules[i].share, schedules[i].shareLate, schedules[i].switchTick, &planA);
			WarSearch_MakePlan(schedules[j].share, schedules[j].shareLate, schedules[j].switchTick, &planB);

			scored = WarSearch_Duel(&planA, &planB, ticks, maps, &valueA, &valueB, &realA, &realB);

			points[i] += scored;
			points[j] += (uint16)(maps * 4) - scored;

			WarSearch_Progress(progress, sizeof(progress), ++done, total, started);
			snprintf(line, sizeof(line), "  %-15s vs %-15s: %u-%u points, value %u-%u, spent %u%%/%u%%, %u/%u by wipeout  %s",
			         schedules[i].name, schedules[j].name, scored, (unsigned)(maps * 4) - scored,
			         (unsigned)valueA, (unsigned)valueB, realA, realB,
			         s_decisive, (unsigned)(maps * 2), progress);
			WarSearch_Print(line);
		}
	}

	WarSearch_Print("war-timing standings:");

	for (i = 0; i < lengthof(schedules); i++) {
		snprintf(line, sizeof(line), "  %-15s (%u%% -> %u%% at t%u) -> %u points of %u",
		         schedules[i].name, schedules[i].share, schedules[i].shareLate,
		         (unsigned)schedules[i].switchTick, points[i],
		         (unsigned)((lengthof(schedules) - 1) * maps * 4));
		WarSearch_Print(line);
	}
}

/**
 * Hunt for a counter to whatever currently wins.
 *
 * Start from the matrix leader, then repeatedly look for the share that beats
 * the current champion by the widest margin.  If the ladder settles on one
 * strategy the balance is transitive; if it cycles, the cycle is the answer --
 * and it is the shape a rock-paper-scissors metagame makes.
 */
void WarSearch_RunLadder(uint32 ticks, uint16 maps)
{
	static const uint8 challengers[] = { 0, 10, 20, 30, 40, 50, 60, 70, 80, 90 };

	const uint16 total = (uint16)(5 * (lengthof(challengers) - 1));
	char line[512];
	char progress[64];
	time_t started = time(NULL);
	uint16 done = 0;
	uint16 champion = 45;
	uint16 round;

	if (maps < 1) maps = 1;

	WarSearch_Print("war-ladder: each round, the share that best beats the current champion");

	for (round = 0; round < 5; round++) {
		SkirmishEconomyPlan planChampion;
		uint16 bestShare = champion;
		uint16 bestPoints = 0;
		uint16 i;

		WarSearch_MakePlan(champion, champion, 0, &planChampion);

		for (i = 0; i < lengthof(challengers); i++) {
			SkirmishEconomyPlan planChallenger;
			uint32 valueA, valueB;
			uint16 realA, realB;
			uint16 scored;

			if (challengers[i] == champion) continue;

			WarSearch_MakePlan(challengers[i], challengers[i], 0, &planChallenger);

			scored = WarSearch_Duel(&planChallenger, &planChampion, ticks, maps, &valueA, &valueB, &realA, &realB);

			WarSearch_Progress(progress, sizeof(progress), ++done, total, started);
			snprintf(line, sizeof(line), "  round %u: %2u%% vs champion %2u%% -> %u-%u (spent %u%%/%u%%)  %s",
			         round + 1, challengers[i], champion, scored, (unsigned)(maps * 4) - scored, realA, realB, progress);
			WarSearch_Print(line);

			if (scored > bestPoints) {
				bestPoints = scored;
				bestShare  = challengers[i];
			}
		}

		/* Beating the champion means more than half the points on offer. */
		if (bestPoints <= (uint16)(maps * 2)) {
			snprintf(line, sizeof(line), "war-ladder: %u%% holds -- nothing beat it, the balance is a constant", champion);
			WarSearch_Print(line);
			return;
		}

		snprintf(line, sizeof(line), "war-ladder: %u%% dethroned by %u%% (%u points)", champion, bestShare, bestPoints);
		WarSearch_Print(line);

		champion = bestShare;
	}

	snprintf(line, sizeof(line), "war-ladder: still moving after 5 rounds, last champion %u%% -- no stable answer", champion);
	WarSearch_Print(line);
}
