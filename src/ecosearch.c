/** @file src/ecosearch.c Search for the fastest economy opening.
 *
 * The question this answers: given a Construction Yard and 1500 credits, what
 * sequence of buildings and what harvester/carryall mix gets spice out of the
 * ground fastest?  Nothing here decides the answer -- it runs the real game,
 * headless and as fast as the CPU allows, and scores what came out.
 *
 * A genome is one opening: an ordered build list plus the unit targets and the
 * Starport orders that go with it (SkirmishEconomyPlan).  Fitness is the gross
 * spice refined within a fixed number of game ticks, averaged over a fixed set
 * of maps so a plan cannot win by being lucky with one landscape.
 *
 * Combat is off in this mode (Skirmish_AI_AllowUnit), so the only thing a plan
 * can spend credits on is its own economy -- which is the point: the result is
 * the economic ceiling, and military spending gets balanced against it later.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "types.h"
#include "os/common.h"
#include "os/math.h"

#include "ecosearch.h"

#include "house.h"
#include "opendune.h"
#include "pool/house.h"
#include "pool/pool.h"
#include "pool/structure.h"
#include "pool/unit.h"
#include "skirmish.h"
#include "structure.h"
#include "team.h"
#include "timer.h"
#include "tile.h"
#include "tools.h"
#include "unit.h"

/**
 * The economic decisions, and only those.
 *
 * Everything else a base needs is not a choice: the Outpost and Light Factory
 * are tolls on the way to the Heavy Factory, and Windtraps are dictated by the
 * power the plan has already committed to.  EcoSearch_Compile() inserts those,
 * so the search never spends a generation rediscovering that a Heavy Factory
 * needs an Outpost, or that four spare Windtraps are four wasted Windtraps.
 * Barracks, WOR, turrets and the Palace are war expenses and cannot appear at
 * all -- combat is off in this mode.
 */
static const uint8 s_palette[] = {
	STRUCTURE_REFINERY,       /* Throughput, and a shorter trip for the harvesters. */
	STRUCTURE_HEAVY_VEHICLE,  /* Builds harvesters. */
	STRUCTURE_HIGH_TECH,      /* Builds carryalls. */
	STRUCTURE_STARPORT,       /* Buys either, at a price. */
	STRUCTURE_SILO            /* Raises the credit ceiling. */
};

#define ECO_GENE_MIN 2
#define ECO_GENE_MAX 10
/* The unit cap is 25 in a skirmish; leave the ceiling well above what any plan
 * has been able to pay for, so the search finds where harvesters stop paying
 * back rather than where this constant sits. */
#define ECO_HARVESTER_MAX 60
#define ECO_CARRYALL_MAX 4
#define ECO_STARPORT_MAX 8
/* Queue samples before another refinery is added.  A sample is one Construction
 * Yard decision, so this is patience measured in decisions, not ticks. */
#define ECO_REFINERY_WAIT_MAX 20
/* Ticks of driving with a full load before another carryall is justified. */
#define ECO_CARRYALL_WAIT_MAX 4800
#define ECO_POPULATION_MAX 64

/** The genome: the decisions, before prerequisites and power are filled in. */
typedef struct EcoGenome {
	uint8 gene[ECO_GENE_MAX];
	uint8 geneCount;
	uint8 harvesterTarget;
	uint8 carryallTarget;
	uint8 starportHarvesters;
	uint8 starportCarryalls;
	uint8 refineryWait;                                     /*!< Queue samples before another refinery; 0 = fixed plan. */
	uint16 carryallWait;                                    /*!< Ticks a loaded harvester may drive home before another carryall; 0 = fixed fleet. */
} EcoGenome;

typedef struct EcoIndividual {
	EcoGenome genome;
	uint32 fitness;
	bool   scored;
} EcoIndividual;

static EcoIndividual s_population[ECO_POPULATION_MAX];
static EcoIndividual s_next[ECO_POPULATION_MAX];

/** House the search plays.  The economy is the same for all three; see the notes. */
#define ECO_HOUSE HOUSE_ATREIDES

/* Per-plan progress, for looking at one opening rather than comparing many. */
static bool s_trace = false;

void EcoSearch_SetTrace(bool trace)
{
	s_trace = trace;
}

static void EcoSearch_Print(const char *str)
{
	fprintf(stderr, "%s\n", str);
	fflush(stderr);
}

/**
 * Turn a genome into a build list the game can run.
 *
 * Two things are inserted here rather than searched for:
 *  - prerequisites, because a Heavy Factory without an Outpost and a Light
 *    Factory is not a cheaper Heavy Factory, it is no Heavy Factory at all;
 *  - Windtraps, one whenever the power already committed would go into deficit,
 *    because a base short of power caps every structure at half hitpoints and a
 *    base with spare Windtraps has simply thrown away 300 credits each.
 */
static void EcoSearch_Compile(const EcoGenome *genome, SkirmishEconomyPlan *plan)
{
	uint32 built = 1 << STRUCTURE_CONSTRUCTION_YARD;
	int16 power = 0;
	uint16 i;

	memset(plan, 0, sizeof(*plan));

	plan->harvesterTarget    = genome->harvesterTarget;
	plan->carryallTarget     = genome->carryallTarget;
	plan->starportHarvesters = genome->starportHarvesters;
	plan->starportCarryalls  = genome->starportCarryalls;
	plan->refineryWait       = genome->refineryWait;
	plan->carryallWait       = genome->carryallWait;

	for (i = 0; i < genome->geneCount; i++) {
		/* Prerequisite chain first, then the gene itself.  One level of nesting
		 * covers the whole economy palette: nothing needs a prerequisite that
		 * itself needs one beyond the Outpost and the Light Factory. */
		static const uint8 order[3] = { STRUCTURE_OUTPOST, STRUCTURE_LIGHT_VEHICLE, 0xFF };
		uint16 step;

		for (step = 0; step < 3; step++) {
			const uint8 type = (step + 1 < 3) ? order[step] : genome->gene[i];
			const StructureInfo *si;

			if (step + 1 < 3) {
				/* Only pull in a toll the gene actually requires. */
				if ((g_table_structureInfo[genome->gene[i]].o.structuresRequired & (1 << type)) == 0) continue;
				if ((built & (1 << type)) != 0) continue;
			}

			si = &g_table_structureInfo[type];

			/* Power the structure will draw, paid for before it is queued. */
			while (power + si->powerUsage > 0 && plan->buildCount < SKIRMISH_PLAN_MAX) {
				plan->build[plan->buildCount++] = STRUCTURE_WINDTRAP;
				power += g_table_structureInfo[STRUCTURE_WINDTRAP].powerUsage;
				built |= 1 << STRUCTURE_WINDTRAP;
			}

			if (plan->buildCount == SKIRMISH_PLAN_MAX) return;

			plan->build[plan->buildCount++] = type;
			power += si->powerUsage;
			built |= 1 << type;
		}
	}
}

/**
 * Run one opening on one map and report the spice it refined.
 *
 * The main loop is paced by the GUI, so the subsystems are driven by hand here,
 * exactly as the skirmish self-test does it.
 */
static uint32 EcoSearch_Evaluate(const EcoGenome *genome, uint32 seed, uint32 ticks)
{
	SkirmishEconomyPlan plan;
	uint32 tick;

	EcoSearch_Compile(genome, &plan);

	/* Same seed in, same match out: the search compares openings, not luck. */
	Tools_RandomLCG_Seed((uint16)seed);

	if (!Skirmish_StartEconomy(ECO_HOUSE, seed, &plan)) return 0;

	/* Hand the clock over completely.  Left running, the SDL timer thread also
	 * advances g_timerGame, and how many of its ticks land between two of ours
	 * varies with machine load -- the same genome then scores differently on
	 * consecutive runs, which is fatal for a search that compares scores. */
	Timer_SetTimer(TIMER_GAME, false);

	for (tick = 0; tick < ticks; tick++) {
		g_timerGame++;

		GameLoop_Team();
		GameLoop_Unit();
		GameLoop_Structure();
		GameLoop_House();

		if (s_trace && (tick % 10000) == 0) {
			char summary[256];
			char line[320];

			if (Skirmish_GetSummary(0, summary, sizeof(summary))) {
				snprintf(line, sizeof(line), "  t%u %s spice-refined %u",
				         (unsigned)tick, summary, (unsigned)Skirmish_Economy_GetHarvested(ECO_HOUSE));
				EcoSearch_Print(line);
			}

			{
				PoolFindStruct find;
				uint16 oldValidate = g_validateStrictIfZero;

				g_validateStrictIfZero = 1;
				find.houseID = ECO_HOUSE;
				find.index   = 0xFFFF;
				find.type    = UNIT_HARVESTER;

				while (true) {
					const Unit *u = Unit_Find(&find);
					char state[32], detail[32];

					if (u == NULL) break;

					Unit_GetStatusText(u, state, detail, sizeof(state));
					snprintf(line, sizeof(line), "      harv#%u %s %s act%u move%04X dest%u,%u pos%u,%u hidden%u",
					         u->o.index, state, detail, u->actionID, u->targetMove,
					         u->currentDestination.x, u->currentDestination.y,
					         Tile_GetPackedX(Tile_PackTile(u->o.position)), Tile_GetPackedY(Tile_PackTile(u->o.position)),
					         u->o.flags.s.isNotOnMap);
					EcoSearch_Print(line);
				}

				find.houseID = ECO_HOUSE;
				find.index   = 0xFFFF;
				find.type    = STRUCTURE_REFINERY;

				while (true) {
					const Structure *s = Structure_Find(&find);

					if (s == NULL) break;

					snprintf(line, sizeof(line), "      ref#%u state%u linked%u var4=%04X hp%u notOnMap%u",
					         s->o.index, s->state, s->o.linkedID, s->o.script.variables[4],
					         s->o.hitpoints, s->o.flags.s.isNotOnMap);
					EcoSearch_Print(line);
				}
				g_validateStrictIfZero = oldValidate;
			}
		}
	}

	Timer_SetTimer(TIMER_GAME, true);

	return Skirmish_Economy_GetHarvested(ECO_HOUSE);
}

static uint32 EcoSearch_Fitness(EcoIndividual *individual, uint32 ticks, uint16 maps)
{
	uint32 total = 0;
	uint16 map;

	if (individual->scored) return individual->fitness;

	for (map = 0; map < maps; map++) {
		/* Fixed map set, so every generation is judged on the same ground. */
		total += EcoSearch_Evaluate(&individual->genome, 1000 + map * 7919, ticks);
	}

	individual->fitness = total / maps;
	individual->scored  = true;

	return individual->fitness;
}

static void EcoSearch_Describe(const EcoGenome *genome, char *buf, uint16 length)
{
	SkirmishEconomyPlan plan;
	uint16 used = 0;
	uint16 i;

	EcoSearch_Compile(genome, &plan);

	buf[0] = '\0';

	for (i = 0; i < plan.buildCount && used + 16 < length; i++) {
		int written = snprintf(buf + used, length - used, "%s%s",
		                       (i == 0) ? "" : " ", g_table_structureInfo[plan.build[i]].o.name);

		if (written <= 0) break;
		used += (uint16)written;
	}

	snprintf(buf + used, length - used, " | harv %u carry %u starport %uh/%uc refinery-on-queue %u carryall-on-trip %u",
	         genome->harvesterTarget, genome->carryallTarget,
	         genome->starportHarvesters, genome->starportCarryalls, genome->refineryWait, genome->carryallWait);
}

static void EcoSearch_Repair(EcoGenome *genome)
{
	bool hasRefinery = false;
	uint16 i;

	if (genome->geneCount < ECO_GENE_MIN) genome->geneCount = ECO_GENE_MIN;
	if (genome->geneCount > ECO_GENE_MAX) genome->geneCount = ECO_GENE_MAX;

	for (i = 0; i < genome->geneCount; i++) {
		if (genome->gene[i] >= STRUCTURE_MAX) genome->gene[i] = STRUCTURE_REFINERY;
		if (genome->gene[i] == STRUCTURE_REFINERY) hasRefinery = true;
	}

	/* Without a Refinery there is no economy to measure. */
	if (!hasRefinery) genome->gene[0] = STRUCTURE_REFINERY;

	if (genome->harvesterTarget < 1) genome->harvesterTarget = 1;
	if (genome->harvesterTarget > ECO_HARVESTER_MAX) genome->harvesterTarget = ECO_HARVESTER_MAX;
	if (genome->carryallTarget > ECO_CARRYALL_MAX) genome->carryallTarget = ECO_CARRYALL_MAX;
	if (genome->starportHarvesters > ECO_STARPORT_MAX) genome->starportHarvesters = ECO_STARPORT_MAX;
	if (genome->starportCarryalls > ECO_STARPORT_MAX) genome->starportCarryalls = ECO_STARPORT_MAX;
	if (genome->refineryWait > ECO_REFINERY_WAIT_MAX) genome->refineryWait = ECO_REFINERY_WAIT_MAX;
	if (genome->carryallWait > ECO_CARRYALL_WAIT_MAX) genome->carryallWait = ECO_CARRYALL_WAIT_MAX;
}

static void EcoSearch_Randomise(EcoGenome *genome)
{
	uint16 i;

	memset(genome, 0, sizeof(*genome));

	genome->geneCount = (uint8)Tools_RandomLCG_Range(ECO_GENE_MIN, ECO_GENE_MAX);

	for (i = 0; i < genome->geneCount; i++) {
		genome->gene[i] = s_palette[Tools_RandomLCG_Range(0, lengthof(s_palette) - 1)];
	}

	genome->harvesterTarget    = (uint8)Tools_RandomLCG_Range(1, ECO_HARVESTER_MAX);
	genome->carryallTarget     = (uint8)Tools_RandomLCG_Range(0, ECO_CARRYALL_MAX);
	genome->starportHarvesters = (uint8)Tools_RandomLCG_Range(0, ECO_STARPORT_MAX);
	genome->starportCarryalls  = (uint8)Tools_RandomLCG_Range(0, ECO_STARPORT_MAX);
	genome->refineryWait       = (uint8)Tools_RandomLCG_Range(0, ECO_REFINERY_WAIT_MAX);
	genome->carryallWait       = Tools_RandomLCG_Range(0, 8) * 600;

	EcoSearch_Repair(genome);
}

static void EcoSearch_Mutate(EcoGenome *genome)
{
	uint16 rolls = Tools_RandomLCG_Range(1, 3);
	uint16 i;

	for (i = 0; i < rolls; i++) {
		switch (Tools_RandomLCG_Range(0, 7)) {
			case 0: /* Replace one decision. */
				genome->gene[Tools_RandomLCG_Range(0, genome->geneCount - 1)] =
					s_palette[Tools_RandomLCG_Range(0, lengthof(s_palette) - 1)];
				break;

			case 1: { /* Swap two, which is how build order gets explored. */
				const uint16 a = Tools_RandomLCG_Range(0, genome->geneCount - 1);
				const uint16 b = Tools_RandomLCG_Range(0, genome->geneCount - 1);
				const uint8 tmp = genome->gene[a];

				genome->gene[a] = genome->gene[b];
				genome->gene[b] = tmp;
			} break;

			case 2: /* Grow or shrink the plan. */
				if (Tools_RandomLCG_Range(0, 1) == 0) {
					if (genome->geneCount < ECO_GENE_MAX) {
						genome->gene[genome->geneCount] = s_palette[Tools_RandomLCG_Range(0, lengthof(s_palette) - 1)];
						genome->geneCount++;
					}
				} else if (genome->geneCount > ECO_GENE_MIN) {
					genome->geneCount--;
				}
				break;

			case 3:
				genome->harvesterTarget = (uint8)Tools_RandomLCG_Range(1, ECO_HARVESTER_MAX);
				break;

			case 4:
				genome->carryallTarget = (uint8)Tools_RandomLCG_Range(0, ECO_CARRYALL_MAX);
				break;

			case 5:
				genome->starportHarvesters = (uint8)Tools_RandomLCG_Range(0, ECO_STARPORT_MAX);
				genome->starportCarryalls  = (uint8)Tools_RandomLCG_Range(0, ECO_STARPORT_MAX);
				break;

			case 6:
				genome->refineryWait = (uint8)Tools_RandomLCG_Range(0, ECO_REFINERY_WAIT_MAX);
				break;

			default:
				genome->carryallWait = Tools_RandomLCG_Range(0, 8) * 600;
				break;
		}
	}

	EcoSearch_Repair(genome);
}

/** Single point crossover on the decisions, coin flip on each scalar. */
static void EcoSearch_Cross(const EcoGenome *a, const EcoGenome *b, EcoGenome *out)
{
	uint16 cut = Tools_RandomLCG_Range(1, a->geneCount - 1);
	uint16 i;

	memset(out, 0, sizeof(*out));

	for (i = 0; i < cut; i++) out->gene[i] = a->gene[i];
	for (i = cut; i < b->geneCount && i < ECO_GENE_MAX; i++) out->gene[i] = b->gene[i];

	out->geneCount = (uint8)max(cut, min(b->geneCount, ECO_GENE_MAX));

	out->harvesterTarget    = (Tools_RandomLCG_Range(0, 1) == 0) ? a->harvesterTarget : b->harvesterTarget;
	out->carryallTarget     = (Tools_RandomLCG_Range(0, 1) == 0) ? a->carryallTarget : b->carryallTarget;
	out->starportHarvesters = (Tools_RandomLCG_Range(0, 1) == 0) ? a->starportHarvesters : b->starportHarvesters;
	out->starportCarryalls  = (Tools_RandomLCG_Range(0, 1) == 0) ? a->starportCarryalls : b->starportCarryalls;
	out->refineryWait       = (Tools_RandomLCG_Range(0, 1) == 0) ? a->refineryWait : b->refineryWait;
	out->carryallWait       = (Tools_RandomLCG_Range(0, 1) == 0) ? a->carryallWait : b->carryallWait;

	EcoSearch_Repair(out);
}

static uint16 EcoSearch_Tournament(uint16 population)
{
	const uint16 a = Tools_RandomLCG_Range(0, population - 1);
	const uint16 b = Tools_RandomLCG_Range(0, population - 1);

	return (s_population[a].fitness >= s_population[b].fitness) ? a : b;
}

/** Hand-written openings, so the search has a bar to clear. */
static void EcoSearch_Seed(uint16 index, EcoGenome *genome)
{
	static const uint8 classic[]      = { STRUCTURE_REFINERY, STRUCTURE_HEAVY_VEHICLE, STRUCTURE_SILO, STRUCTURE_HIGH_TECH };
	static const uint8 refineryRush[] = { STRUCTURE_REFINERY, STRUCTURE_REFINERY, STRUCTURE_REFINERY, STRUCTURE_SILO };
	static const uint8 starportRush[] = { STRUCTURE_REFINERY, STRUCTURE_STARPORT, STRUCTURE_REFINERY, STRUCTURE_SILO };

	memset(genome, 0, sizeof(*genome));

	switch (index) {
		case 0:
			memcpy(genome->gene, classic, sizeof(classic));
			genome->geneCount = lengthof(classic);
			genome->harvesterTarget = 3;
			genome->carryallTarget = 1;
			break;

		case 1:
			memcpy(genome->gene, refineryRush, sizeof(refineryRush));
			genome->geneCount = lengthof(refineryRush);
			genome->harvesterTarget = 6;
			break;

		default:
			memcpy(genome->gene, starportRush, sizeof(starportRush));
			genome->geneCount = lengthof(starportRush);
			genome->harvesterTarget = 4;
			genome->carryallTarget = 1;
			genome->starportHarvesters = 3;
			break;
	}

	EcoSearch_Repair(genome);
}

/**
 * Score the hand-written openings and print them.  Cheap sanity check that the
 * economy mode does what it says before spending an hour evolving anything.
 */
void EcoSearch_RunBaseline(uint32 ticks, uint16 maps)
{
	char line[512];
	char description[400];
	uint16 i;

	for (i = 0; i < 3; i++) {
		EcoIndividual individual;

		memset(&individual, 0, sizeof(individual));
		EcoSearch_Seed(i, &individual.genome);
		EcoSearch_Describe(&individual.genome, description, sizeof(description));

		snprintf(line, sizeof(line), "baseline %u: spice %u | %s",
		         i, (unsigned)EcoSearch_Fitness(&individual, ticks, maps), description);
		EcoSearch_Print(line);

		/* The state the last evaluated map ended in, which is where a plan that
		 * scores badly explains itself. */
		if (Skirmish_GetSummary(0, line, sizeof(line))) EcoSearch_Print(line);
		if (Skirmish_GetBuildOrder(0, line, sizeof(line))) EcoSearch_Print(line);
	}
}

/**
 * Build the plan behind one cell of the driver grid: N refineries, a Heavy
 * Factory, and a Hi-Tech factory when carryalls are asked for.  This is what
 * --economy-play hands to the game so a number from the table can be watched.
 */
void EcoSearch_MakePlan(uint16 refineries, uint16 harvesters, uint16 carryalls, uint16 refineryWait, uint16 carryallWait, SkirmishEconomyPlan *out)
{
	EcoGenome genome;
	uint16 i;

	memset(&genome, 0, sizeof(genome));

	if (refineries < 1) refineries = 1;
	if (refineries > ECO_GENE_MAX - 2) refineries = ECO_GENE_MAX - 2;

	/* First refinery, then the factory, then the rest of the refineries.  Putting
	 * all of them up front was measuring something else entirely: four refineries
	 * cost 1600 credits out of a 1500 credit start, so the Heavy Factory -- and
	 * with it every harvester after the free one -- arrived far too late.  The
	 * question here is what extra unload capacity is worth, not what delaying the
	 * factory costs. */
	genome.gene[0] = STRUCTURE_REFINERY;
	genome.gene[1] = STRUCTURE_HEAVY_VEHICLE;
	genome.geneCount = 2;

	for (i = 1; i < refineries; i++) genome.gene[genome.geneCount++] = STRUCTURE_REFINERY;

	if (carryalls != 0) genome.gene[genome.geneCount++] = STRUCTURE_HIGH_TECH;

	genome.harvesterTarget = (uint8)harvesters;
	genome.carryallTarget  = (uint8)carryalls;
	genome.refineryWait    = (uint8)refineryWait;
	genome.carryallWait    = carryallWait;

	EcoSearch_Repair(&genome);
	EcoSearch_Compile(&genome, out);
}

/**
 * Sweep the three economic drivers one at a time: refineries, harvesters and
 * carryalls.  The search says which opening wins; this says why.
 */
void EcoSearch_RunGrid(uint32 ticks, uint16 maps)
{
	static const uint8 harvesters[] = { 2, 4, 8, 16, 32 };

	char line[256];
	uint16 refineries;
	uint16 h;
	uint16 carryalls;

	static const uint8 waits[] = { 0, 2, 4, 8 };

	EcoSearch_Print("eco-grid: refineries x harvester target x carryalls -> spice refined");

	for (refineries = 1; refineries <= 4; refineries++) {
		for (h = 0; h < lengthof(harvesters); h++) {
			for (carryalls = 0; carryalls <= 2; carryalls += 2) {
				EcoIndividual individual;
				uint16 i;

				memset(&individual, 0, sizeof(individual));

				for (i = 0; i < refineries; i++) individual.genome.gene[i] = STRUCTURE_REFINERY;
				individual.genome.gene[refineries] = STRUCTURE_HEAVY_VEHICLE;
				individual.genome.geneCount = (uint8)refineries + 1;

				if (carryalls != 0) {
					individual.genome.gene[individual.genome.geneCount++] = STRUCTURE_HIGH_TECH;
				}

				individual.genome.harvesterTarget = harvesters[h];
				individual.genome.carryallTarget  = (uint8)carryalls;

						individual.genome.refineryWait = 0;

				snprintf(line, sizeof(line), "  refinery %u harvesters %2u carryall %u -> spice %u",
				         refineries, harvesters[h], carryalls,
				         (unsigned)EcoSearch_Fitness(&individual, ticks, maps));
				EcoSearch_Print(line);

				if (s_trace) {
					char load[256];

					if (Skirmish_GetRefineryLoad(load, sizeof(load))) {
						snprintf(line, sizeof(line), "      %s", load);
						EcoSearch_Print(line);
					}
				}
			}
		}
	}
}

/**
 * The adaptive rule against fixed refinery counts: start with one refinery and
 * let the queue decide when to add the next.
 */
void EcoSearch_RunQueueSweep(uint32 ticks, uint16 maps)
{
	static const uint8 waits[] = { 0, 2, 5, 8, 10, 12, 15, 20 };
	static const uint8 harvesters[] = { 8, 16, 32 };

	char line[256];
	uint16 w;
	uint16 h;

	EcoSearch_Print("eco-queue: one refinery to start, another whenever harvesters queue");

	for (h = 0; h < lengthof(harvesters); h++) {
		for (w = 0; w < lengthof(waits); w++) {
			EcoIndividual individual;

			memset(&individual, 0, sizeof(individual));
			individual.genome.gene[0] = STRUCTURE_REFINERY;
			individual.genome.gene[1] = STRUCTURE_HEAVY_VEHICLE;
			individual.genome.geneCount = 2;
			individual.genome.harvesterTarget = harvesters[h];
			individual.genome.refineryWait = waits[w];
			/* With the carryall rule on, because N is being calibrated for the
			 * economy as it actually runs now, not for the one without them. */
			individual.genome.carryallWait = 1200;

			snprintf(line, sizeof(line), "  harvesters %2u refinery-on-queue %2u -> spice %u",
			         harvesters[h], waits[w], (unsigned)EcoSearch_Fitness(&individual, ticks, maps));
			EcoSearch_Print(line);

			if (s_trace) {
				char load[256];

				if (Skirmish_GetRefineryLoad(load, sizeof(load))) {
					snprintf(line, sizeof(line), "      %s", load);
					EcoSearch_Print(line);
				}
			}
		}
	}
}

/**
 * How long is a harvester allowed to drive home before another carryall is worth
 * its 500 credit factory?  With the refinery rule fixed at five, so the queue is
 * somebody else's problem.
 */
void EcoSearch_RunCarryallSweep(uint32 ticks, uint16 maps)
{
	static const uint16 waits[] = { 0, 300, 600, 1200, 2400, 4800 };
	static const uint8 harvesters[] = { 8, 16 };

	char line[256];
	uint16 w;
	uint16 h;

	EcoSearch_Print("eco-carryall: carryalls grow when loaded harvesters spend too long on the road");

	for (h = 0; h < lengthof(harvesters); h++) {
		for (w = 0; w < lengthof(waits); w++) {
			EcoIndividual individual;

			memset(&individual, 0, sizeof(individual));
			individual.genome.gene[0] = STRUCTURE_REFINERY;
			individual.genome.gene[1] = STRUCTURE_HEAVY_VEHICLE;
			individual.genome.geneCount = 2;
			individual.genome.harvesterTarget = harvesters[h];
			individual.genome.refineryWait = 5;
			individual.genome.carryallWait = waits[w];

			snprintf(line, sizeof(line), "  harvesters %2u carryall-on-trip %4u -> spice %u",
			         harvesters[h], waits[w], (unsigned)EcoSearch_Fitness(&individual, ticks, maps));
			EcoSearch_Print(line);

			if (s_trace && Skirmish_GetSummary(0, line, sizeof(line))) {
				char summary[300];

				snprintf(summary, sizeof(summary), "      %s", line);
				EcoSearch_Print(summary);
			}
		}
	}
}

/**
 * Evolve an economy opening.
 * @param population Individuals per generation.
 * @param generations How many generations to run.
 * @param ticks Game ticks each evaluation simulates.
 * @param maps Maps each individual is averaged over.
 */
void EcoSearch_Run(uint16 population, uint16 generations, uint32 ticks, uint16 maps)
{
	char line[512];
	char description[400];
	EcoIndividual best;
	time_t started = time(NULL);
	uint16 generation;
	uint16 i;

	if (population < 4) population = 4;
	if (population > ECO_POPULATION_MAX) population = ECO_POPULATION_MAX;
	if (maps < 1) maps = 1;

	memset(&best, 0, sizeof(best));

	Tools_RandomLCG_Seed(1);

	for (i = 0; i < population; i++) {
		memset(&s_population[i], 0, sizeof(s_population[i]));

		if (i < 3) {
			EcoSearch_Seed(i, &s_population[i].genome);
		} else {
			EcoSearch_Randomise(&s_population[i].genome);
		}
	}

	snprintf(line, sizeof(line), "eco-search: %u individuals, %u generations, %u ticks, %u maps -- up to %u simulated matches",
	         population, generations, (unsigned)ticks, maps, (unsigned)population * generations * maps);
	EcoSearch_Print(line);

	for (generation = 0; generation < generations; generation++) {
		uint32 total = 0;
		uint16 bestIndex = 0;
		uint16 elite;
		long elapsed;
		long remaining;

		for (i = 0; i < population; i++) {
			const uint32 fitness = EcoSearch_Fitness(&s_population[i], ticks, maps);

			total += fitness;
			if (fitness > s_population[bestIndex].fitness) bestIndex = i;
		}

		if (s_population[bestIndex].fitness > best.fitness) best = s_population[bestIndex];

		/* Wall clock rather than a spinner: the run is long enough that the
		 * useful question is when it ends, not whether it is alive. */
		elapsed = (long)(time(NULL) - started);
		remaining = (generation + 1 < generations) ? elapsed * (generations - generation - 1) / (generation + 1) : 0;

		EcoSearch_Describe(&s_population[bestIndex].genome, description, sizeof(description));
		snprintf(line, sizeof(line), "gen %u/%u [%lds elapsed, ~%lds left]: best %u avg %u | %s",
		         generation + 1, generations, elapsed, remaining,
		         (unsigned)s_population[bestIndex].fitness,
		         (unsigned)(total / population), description);
		EcoSearch_Print(line);

		if (generation + 1 == generations) break;

		/* Elitism: the two best survive untouched, so a generation can never be
		 * worse than the one before it. */
		s_next[0] = best;
		s_next[1] = s_population[bestIndex];
		elite = 2;

		for (i = elite; i < population; i++) {
			const uint16 a = EcoSearch_Tournament(population);
			const uint16 b = EcoSearch_Tournament(population);

			memset(&s_next[i], 0, sizeof(s_next[i]));
			EcoSearch_Cross(&s_population[a].genome, &s_population[b].genome, &s_next[i].genome);
			EcoSearch_Mutate(&s_next[i].genome);
		}

		memcpy(s_population, s_next, sizeof(EcoIndividual) * population);
	}

	EcoSearch_Describe(&best.genome, description, sizeof(description));
	snprintf(line, sizeof(line), "eco-search best: spice %u | %s", (unsigned)best.fitness, description);
	EcoSearch_Print(line);

	/* The genome is a wish list: Skirmish_Plan_PickNext() reorders it around
	 * prerequisites, power and what can be paid for, so what actually went up is
	 * the answer worth reading. */
	EcoSearch_Evaluate(&best.genome, 1000, ticks);

	if (Skirmish_GetSummary(0, line, sizeof(line))) EcoSearch_Print(line);
	if (Skirmish_GetBuildOrder(0, line, sizeof(line))) {
		char built[560];

		snprintf(built, sizeof(built), "built: %s", line);
		EcoSearch_Print(built);
	}
}
