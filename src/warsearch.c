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

	/* Off before the match is built: see WarSearch_RunMetrics() for what the
	 * timer thread does to a match that starts while it is running. */
	Timer_SetTimer(TIMER_GAME, false);

	Tools_RandomLCG_Seed((uint16)seed);

	if (!Skirmish_StartWar(WAR_HOUSE_A, WAR_HOUSE_B, seed, planA, planB)) {
		Timer_SetTimer(TIMER_GAME, true);
		return 0;
	}

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

/* -------------------------------------------------------------------------- */
/* The metrics suite                                                           */
/* -------------------------------------------------------------------------- */

/**
 * A fixed battery of matches, scored against fixed targets.
 *
 * Everything else in this file compares two strategies and answers "which".
 * This answers "is it still working", which is a different question and needs a
 * different shape: the same maps every time, a named list of numbers, and a
 * target beside each one.  It exists because every behavioural fix in this fork
 * so far has broken something that had already been fixed -- the turret fence
 * stopped units leaving, the flank route stopped them fighting, a rule written
 * for one doctrine leaked into the other -- and each was found by eye, matches
 * later, rather than by the run that caused it.
 *
 * Both sides are measured, not just the one under test.  The leak that put a
 * doctrine B rule into doctrine A was invisible precisely because nobody was
 * looking at A's numbers, and a baseline that drifts is not a baseline.
 *
 * Each map is played twice with the doctrines swapped between Houses, so House
 * identity and the corner of the map cancel out instead of being read as a
 * result.
 */
typedef struct MetricTotals {
	DoctrineMetrics d;
	uint32 spice;
	uint32 matches;
	uint32 assaultMatches;                                  /*!< Matches with at least one assault. */
	uint32 assaultTickSum;
	uint32 wipeouts;                                        /*!< Times this side lost its last building. */
	uint32 points;                                          /*!< 2 per win, 1 per draw. */
} MetricTotals;

/** Which way a metric is good, and where the line is. */
typedef enum MetricSense { METRIC_LOWER, METRIC_HIGHER } MetricSense;

static void WarSearch_MetricAdd(MetricTotals *t, uint8 houseID)
{
	DoctrineMetrics m;

	Doctrine_GetMetrics(houseID, &m);

	t->d.turretEntries       += m.turretEntries;
	t->d.turretDwell         += m.turretDwell;
	t->d.turretDeathsLoose   += m.turretDeathsLoose;
	t->d.turretDeathsAssault += m.turretDeathsAssault;
	t->d.turretsKilled       += m.turretsKilled;
	t->d.harvesterLost       += m.harvesterLost;
	t->d.harvesterLostEarly  += m.harvesterLostEarly;
	t->d.harvesterKilled     += m.harvesterKilled;
	t->d.harvesterExposed    += m.harvesterExposed;
	t->d.harvesterSamples    += m.harvesterSamples;
	t->d.cohesionOn          += m.cohesionOn;
	t->d.cohesionAll         += m.cohesionAll;
	t->d.wavesLaunched       += m.wavesLaunched;
	t->d.wavesAborted        += m.wavesAborted;
	t->d.wavesDeclined       += m.wavesDeclined;

	if (m.firstAssault != 0) {
		t->assaultMatches++;
		t->assaultTickSum += m.firstAssault;
	}

	t->spice += Skirmish_Economy_GetHarvested(houseID);
	t->matches++;
}

/** Per match, rounded, with an empty battery reading zero rather than dividing by it. */
static uint32 WarSearch_PerMatch(uint32 total, uint32 matches)
{
	if (matches == 0) return 0;
	return (total + matches / 2) / matches;
}

static uint32 WarSearch_Percent(uint32 part, uint32 whole)
{
	if (whole == 0) return 0;
	return part * 100 / whole;
}

/**
 * One row: name, both sides' readings, the gate, the goal, the verdict.
 *
 * Two numbers rather than one, because they answer different questions and
 * collapsing them makes the suite useless in one direction or the other.  The
 * goal is where the behaviour should end up and several of them are nowhere near
 * met; the gate is where it stands now with room for noise, and breaking one is
 * a regression -- something that worked this morning does not any more.  A suite
 * that only carried goals would print FAIL for ever and be ignored; one that only
 * carried gates would quietly bless whatever it happened to measure first.
 */
static void WarSearch_MetricRow(const char *name, uint32 test, uint32 base,
                                uint32 gate, uint32 goal, MetricSense sense,
                                uint16 *failures, uint16 *offGoal)
{
	const bool held = (sense == METRIC_LOWER) ? (test <= gate) : (test >= gate);
	const bool met  = (sense == METRIC_LOWER) ? (test <= goal) : (test >= goal);
	const char *op = (sense == METRIC_LOWER) ? "<=" : ">=";
	char line[200];

	if (!held) (*failures)++;
	if (!met) (*offGoal)++;

	snprintf(line, sizeof(line), "  %-22s %10u %10u   %s%-8u %s%-8u %s",
	         name, (unsigned)test, (unsigned)base,
	         op, (unsigned)gate, op, (unsigned)goal,
	         !held ? "REGRESSION" : (met ? "on goal" : "off goal"));
	WarSearch_Print(line);
}

/**
 * Play the battery and print the scorecard.
 *
 * @param houseA The House the tested doctrine starts as; it plays the other one
 *               on the return leg of every map.
 */
void WarSearch_RunMetrics(uint8 houseA, uint8 houseB, uint32 ticks, uint16 maps)
{
	MetricTotals test, base;
	SkirmishEconomyPlan plan;
	char line[256];
	char doctrine[32];
	uint16 failures = 0;
	uint16 offGoal = 0;
	uint16 map;
	uint32 value;

	if (maps < 1) maps = 1;
	if (ticks == 0) ticks = 200000;

	memset(&test, 0, sizeof(test));
	memset(&base, 0, sizeof(base));

	/* The tuned opening, the same for both sides.  A doctrine is a way of using
	 * an army, and measuring it on top of two different economies would be
	 * measuring the economies. */
	WarSearch_MakePlan(0, 90, 30000, &plan);

	Doctrine_GetSelection(doctrine, sizeof(doctrine));
	snprintf(line, sizeof(line), "doctrine metrics: %s, %u maps x2 legs, %u ticks",
	         doctrine, maps, (unsigned)ticks);
	WarSearch_Print(line);

	for (map = 0; map < maps; map++) {
		const uint32 seed = 1000 + map * 7919;
		uint8 leg;

		for (leg = 0; leg < 2; leg++) {
			const uint8 first  = (leg == 0) ? houseA : houseB;
			const uint8 second = (leg == 0) ? houseB : houseA;
			uint32 tick;

			/* The clock goes off before the match is built, not after.
			 *
			 * Starting a war takes real time -- a map is generated and two bases
			 * are placed -- and with the timer thread still running, however many
			 * of its ticks land inside that window is a property of machine load.
			 * The match then begins at a different absolute tick every run, and
			 * everything the engine gates on tick parity falls differently.  Two
			 * runs of this suite on the same binary disagreed by a third on spice
			 * and by half on time spent under turrets, which is more than most of
			 * the changes it is meant to be measuring. */
			Timer_SetTimer(TIMER_GAME, false);

			Tools_RandomLCG_Seed((uint16)seed);
			if (!Skirmish_StartWar(first, second, seed, &plan, &plan)) {
				Timer_SetTimer(TIMER_GAME, true);
				continue;
			}

			for (tick = 0; tick < ticks; tick++) {
				g_timerGame++;

				GameLoop_Team();
				GameLoop_Unit();
				GameLoop_Structure();
				GameLoop_House();

				if ((tick & 0x3FF) == 0
					&& (Skirmish_War_IsDefeated(0) || Skirmish_War_IsDefeated(1))) break;
			}

			Timer_SetTimer(TIMER_GAME, true);

			/* Base index 0 always carries the tested doctrine -- that is what
			 * swapping the Houses between legs is for. */
			WarSearch_MetricAdd(&test, Skirmish_GetBaseHouse(0));
			WarSearch_MetricAdd(&base, Skirmish_GetBaseHouse(1));

			if (Skirmish_War_IsDefeated(0)) test.wipeouts++;
			if (Skirmish_War_IsDefeated(1)) base.wipeouts++;

			{
				const uint32 a = Skirmish_War_GetValue(0);
				const uint32 b = Skirmish_War_GetValue(1);

				if (Skirmish_War_IsDefeated(1) && !Skirmish_War_IsDefeated(0)) {
					test.points += 2;
				} else if (Skirmish_War_IsDefeated(0) && !Skirmish_War_IsDefeated(1)) {
					base.points += 2;
				} else if (a > b + b * WAR_MARGIN_PERCENT / 100) {
					test.points += 2;
				} else if (b > a + a * WAR_MARGIN_PERCENT / 100) {
					base.points += 2;
				} else {
					test.points++;
					base.points++;
				}
			}

			snprintf(line, sizeof(line), "  map %u leg %u: seed %u done at t%u", map + 1, leg + 1, (unsigned)seed, (unsigned)tick);
			WarSearch_Print(line);
		}
	}

	WarSearch_Print("");
	snprintf(line, sizeof(line), "  %-22s %10s %10s   %-10s %-10s %s",
	         "metric", "tested", "baseline", "gate", "goal", "verdict");
	WarSearch_Print(line);

	/* Discipline: the turret rule, which is the one that keeps regressing.
	 * Entries counts decisions, dwell counts what those decisions cost -- a fence
	 * that turns units at the line and a fence they walk through and sit behind
	 * produce the same entry count and wildly different dwell. */
	WarSearch_MetricRow("turret.entries/match", WarSearch_PerMatch(test.d.turretEntries, test.matches),
	                    WarSearch_PerMatch(base.d.turretEntries, base.matches), 24, 8, METRIC_LOWER, &failures, &offGoal);
	WarSearch_MetricRow("turret.dwell/match", WarSearch_PerMatch(test.d.turretDwell, test.matches),
	                    WarSearch_PerMatch(base.d.turretDwell, base.matches), 13000, 400, METRIC_LOWER, &failures, &offGoal);
	WarSearch_MetricRow("turret.deaths.loose", test.d.turretDeathsLoose,
	                    base.d.turretDeathsLoose, 0, 0, METRIC_LOWER, &failures, &offGoal);

	/* Died to a turret during an assault, and the turret died too: the trade the
	 * doctrine is willing to make.  No assault deaths at all reads as 999, which
	 * passes -- nothing was spent, so there is nothing to have bought. */
	value = (test.d.turretDeathsAssault == 0) ? 999
	      : test.d.turretsKilled * 100 / test.d.turretDeathsAssault;
	WarSearch_MetricRow("turret.trade %", value,
	                    (base.d.turretDeathsAssault == 0) ? 999
	                    : base.d.turretsKilled * 100 / base.d.turretDeathsAssault,
	                    100, 100, METRIC_HIGHER, &failures, &offGoal);

	/* Money likes quiet.  The loss count is the outcome and the exposure is the
	 * cause; both are here because the first is what matters and the second is
	 * what moves first. */
	WarSearch_MetricRow("harv.lost.early", test.d.harvesterLostEarly,
	                    base.d.harvesterLostEarly, 24, 0, METRIC_LOWER, &failures, &offGoal);
	WarSearch_MetricRow("harv.lost/match", WarSearch_PerMatch(test.d.harvesterLost, test.matches),
	                    WarSearch_PerMatch(base.d.harvesterLost, base.matches), 8, 0, METRIC_LOWER, &failures, &offGoal);
	WarSearch_MetricRow("harv.exposure %", WarSearch_Percent(test.d.harvesterExposed, test.d.harvesterSamples),
	                    WarSearch_Percent(base.d.harvesterExposed, base.d.harvesterSamples), 20, 5, METRIC_LOWER, &failures, &offGoal);
	WarSearch_MetricRow("harv.killed/match", WarSearch_PerMatch(test.d.harvesterKilled, test.matches),
	                    WarSearch_PerMatch(base.d.harvesterKilled, base.matches), 6, 10, METRIC_HIGHER, &failures, &offGoal);

	/* Waves: that they happen, that they are not shuttled back and forth, and
	 * that when one goes in the army goes with it. */
	WarSearch_MetricRow("wave.launched/match", WarSearch_PerMatch(test.d.wavesLaunched, test.matches),
	                    WarSearch_PerMatch(base.d.wavesLaunched, base.matches), 2, 3, METRIC_HIGHER, &failures, &offGoal);
	WarSearch_MetricRow("wave.declined/match", WarSearch_PerMatch(test.d.wavesDeclined, test.matches),
	                    WarSearch_PerMatch(base.d.wavesDeclined, base.matches), 60, 20, METRIC_LOWER, &failures, &offGoal);
	WarSearch_MetricRow("wave.cohesion %", WarSearch_Percent(test.d.cohesionOn, test.d.cohesionAll),
	                    WarSearch_Percent(base.d.cohesionOn, base.d.cohesionAll), 70, 85, METRIC_HIGHER, &failures, &offGoal);
	WarSearch_MetricRow("wave.first.tick", WarSearch_PerMatch(test.assaultTickSum, test.assaultMatches),
	                    WarSearch_PerMatch(base.assaultTickSum, base.assaultMatches), 130000, 90000, METRIC_LOWER, &failures, &offGoal);
	WarSearch_MetricRow("wave.matches %", WarSearch_Percent(test.assaultMatches, test.matches),
	                    WarSearch_Percent(base.assaultMatches, base.matches), 40, 90, METRIC_HIGHER, &failures, &offGoal);

	/* And whether any of it won anything.
	 *
	 * The gates here and on the harvester rows were re-taken on 13 Sep 2026
	 * against the AI as it is shipped -- concrete paid for in time
	 * (skirmish_ai_paving) and guards that answer for their ground
	 * (skirmish_ai_guard), both on for both houses -- rather than against the
	 * faster, passive AI the suite was first calibrated on.  Doctrine B is
	 * scored, and under that AI it loses to A more often than not: the
	 * numbers are what they are, and a gate that pretends otherwise reads
	 * FAIL for ever and is ignored.  See metrics.md, "Re-baselined". */
	WarSearch_MetricRow("econ.spice/match", WarSearch_PerMatch(test.spice, test.matches),
	                    WarSearch_PerMatch(base.spice, base.matches), 45000, 65000, METRIC_HIGHER, &failures, &offGoal);
	WarSearch_MetricRow("result.points %", WarSearch_Percent(test.points, test.matches * 2),
	                    WarSearch_Percent(base.points, base.matches * 2), 30, 75, METRIC_HIGHER, &failures, &offGoal);
	WarSearch_MetricRow("result.wipeouts %", WarSearch_Percent(test.wipeouts, test.matches),
	                    WarSearch_Percent(base.wipeouts, base.matches), 60, 10, METRIC_LOWER, &failures, &offGoal);

	WarSearch_Print("");
	snprintf(line, sizeof(line), "doctrine metrics: %s -- %u regressions, %u of 16 still short of goal",
	         (failures == 0) ? "PASS" : "FAIL", failures, offGoal);
	WarSearch_Print(line);
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
	fprintf(fp, "tick,house,refineries,combatStructures,harvesters,combatUnits,combatHitpoints,damageTaken,spiceRefined,credits,powerSurplus,shotsTurret,shotsStructure,shotsUnit,shotsBypass,idleAttackers,idleOnWave,stalledHarvesters,freeRefineries,wavePhase,waveUnits,waveAtLD,waveColumn,wavesLaunched,wavesAborted,wavesDeclined,inAssault,inMuster,idleGarrison,assaultSpread,firstAssault,raiders,raidersHunting,atkStrength,defStrength,picket,builtArt,builtAss,builtRaid,builtGar,pickArt,pickAss,pickRaid,pickGar,vetoArt,vetoAss,vetoRaid,vetoGar,turretZone,turretEntries,harvLost,harvKilled,harvLostEarly,harvNear8,harvWorst,turretKillsLoose,turretKillsAssault,turretsKilled\n");

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
