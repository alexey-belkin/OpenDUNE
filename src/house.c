/** @file src/house.c %House management routines. */

#include <stdio.h>
#include "os/common.h"
#include "types.h"
#include "os/math.h"
#include "os/strings.h"
#include "os/sleep.h"

#include "house.h"
#include "mpturn.h"
#include "match.h"

#include "audio/driver.h"
#include "audio/sound.h"
#include "gfx.h"
#include "inifile.h"
#include "gui/gui.h"
#include "gui/widget.h"
#include "map.h"
#include "opendune.h"
#include "pool/pool.h"
#include "pool/house.h"
#include "pool/structure.h"
#include "pool/unit.h"
#include "scenario.h"
#include "skirmish.h"
#include "string.h"
#include "structure.h"
#include "table/strings.h"
#include "tile.h"
#include "timer.h"
#include "tools.h"
#include "unit.h"
#include "wsa.h"


/* -----------------------------------------------------------------------------
 * The Starport as a shop.
 *
 * The original stocks it per scenario, prices it around the factory price and
 * delivers almost at once, which is close enough to free that a Starport is
 * simply a better factory.  This fork treats it as a trader: the goods come
 * from off-world, so they cost more and take time to arrive, and how much of
 * either is tuning rather than code.  Every default here is also a key under
 * the [opendune] heading of opendune.ini -- keep bin/opendune.ini.sample and
 * README.txt in step with the initialiser below.
 * -------------------------------------------------------------------------- */

typedef struct StarportConfig {
	uint16 restockTicks;                                    /*!< Game ticks between one freighter top-up and the next. */
	uint16 markup;                                          /*!< Mean price, as a percentage of the factory price. */
	uint16 markupOrdos;                                     /*!< The same for Ordos, who are the traders. */
	uint16 delivery;                                        /*!< Delivery time, as a percentage of the house table's. */
	uint16 deliveryOrdos;                                   /*!< The same for Ordos. */
	uint16 stockCredits;                                    /*!< A type's opening stock is what this buys of it. */
	uint16 stockCeiling;                                    /*!< Restock limit, as a percentage of the opening stock. */
	bool   specialUnits;                                    /*!< Whether a House's own units are for sale.  @see Starport_Sells() */
} StarportConfig;

static StarportConfig s_starport = {
	1800,
	130,
	100,
	300,
	150,
	1500,
	200,
	false
};

static uint16 Starport_ReadPercent(const char *key, uint16 defaultValue, uint16 low)
{
	int value = IniFile_GetInteger(key, defaultValue);

	if (value < (int)low) return low;
	if (value > 10000) return 10000;
	return (uint16)value;
}

void Starport_Init(void)
{
	int ticks = IniFile_GetInteger("starport_restock_ticks", s_starport.restockTicks);

	/* A period of zero would top the shelves up sixty times a second, which is
	 * not a fast shop but an infinite one. */
	if (ticks < 1) ticks = 1;
	if (ticks > 0xFFFF) ticks = 0xFFFF;
	s_starport.restockTicks = (uint16)ticks;

	/* The price is the factory price times (base + two dice of 0..6) / 10, so
	 * the mean is (base + 6) / 10 and the floor of the markup is 60%: below
	 * that the base would have to go negative and the spread would fold over
	 * itself. */
	s_starport.markup        = Starport_ReadPercent("starport_markup", s_starport.markup, 60);
	s_starport.markupOrdos   = Starport_ReadPercent("starport_markup_ordos", s_starport.markupOrdos, 60);
	s_starport.delivery      = Starport_ReadPercent("starport_delivery", s_starport.delivery, 0);
	s_starport.deliveryOrdos = Starport_ReadPercent("starport_delivery_ordos", s_starport.deliveryOrdos, 0);
	s_starport.stockCeiling  = Starport_ReadPercent("starport_stock_ceiling", s_starport.stockCeiling, 100);

	s_starport.specialUnits = (IniFile_GetInteger("starport_special_units", s_starport.specialUnits ? 1 : 0) != 0);

	ticks = IniFile_GetInteger("starport_stock_credits", s_starport.stockCredits);
	if (ticks < 0) ticks = 0;
	if (ticks > 0xFFFF) ticks = 0xFFFF;
	s_starport.stockCredits = (uint16)ticks;
}

uint16 Starport_RestockTicks(void)
{
	return s_starport.restockTicks;
}

/**
 * What the freighter is asking for one unit.
 *
 * @param houseID The buying house; Ordos deal at their own rate.
 * @param buildCredits The factory price.
 * @param roll Two dice of 0..6, drawn by the caller -- the interface generator
 *   when a window is asking, the game generator when the simulation is.  Which
 *   stream is used is the caller's business and it matters: a price the player
 *   is merely looking at must not move the world.
 * @return The price.  Uncapped: an off-world Devastator is allowed to cost
 *   what it costs.
 */
uint16 Starport_Price(uint8 houseID, uint16 buildCredits, uint16 roll)
{
	uint16 markup = (houseID == HOUSE_ORDOS) ? s_starport.markupOrdos : s_starport.markup;
	uint16 base   = (uint16)(markup / 10);
	uint16 tenth  = (uint16)(buildCredits / 10);

	base = (base > 6) ? (uint16)(base - 6) : 0;

	return (uint16)(tenth * (base + roll));
}

/**
 * How many of a type the freighter carries to begin with: what one fixed sum
 * buys of it at the factory price.  A cheap unit therefore arrives by the
 * dozen and an MCV one at a time, which is the shape of a cargo hold rather
 * than of a shopping list.  Never zero, or the type would be missing from the
 * window and could never restock into it.
 */
int16 Starport_InitialStock(uint16 buildCredits)
{
	uint16 stock;

	if (buildCredits == 0) return 0;

	stock = (uint16)(s_starport.stockCredits / buildCredits);

	return (int16)((stock < 1) ? 1 : min(stock, 127));
}

/** How far restocking may refill a type: a multiple of what it opened with. */
int16 Starport_StockCeiling(uint16 buildCredits)
{
	int16 opening = Starport_InitialStock(buildCredits);
	uint32 ceiling;

	if (opening <= 0) return 0;

	ceiling = ((uint32)opening * s_starport.stockCeiling) / 100;
	if (ceiling < (uint32)opening) ceiling = (uint32)opening;

	return (int16)min(ceiling, 127);
}

/**
 * Whether any factory in the game builds this type.
 *
 * The Ordos Raider Trike is a special case and not an exception: no table lists
 * it, because Structure_GetBuildObject() swaps a Light Factory's Trike for one
 * when Ordos own the factory.  It has a factory; it just has it by substitution.
 */
static bool Starport_FactoryBuilds(uint16 unitType)
{
	uint16 i, j;

	if (unitType == UNIT_RAIDER_TRIKE) return true;

	for (i = 0; i < STRUCTURE_MAX; i++) {
		for (j = 0; j < 8; j++) {
			if (g_table_structureInfo[i].buildableUnits[j] == unitType) return true;
		}
	}

	return false;
}

/**
 * Whether the freighter sells this type at all.
 *
 * The Starport used to sell whatever the buying house was allowed to field,
 * which let it stand in for buildings the house had never built: eight hundred
 * credits and a Starport bought a Devastator, and the House of IX -- the thing
 * that is supposed to cost -- need never go up.  The same for the Saboteur,
 * which no factory anywhere builds, because it is what the Ordos Palace does.
 *
 * So the shop is now the common roster: what a house could have built for
 * itself with an ordinary factory.  A House's own unit comes from that House's
 * own building.  `starport_special_units=1` sells them again.
 */
bool Starport_Sells(uint16 unitType)
{
	const UnitInfo *ui;

	if (unitType >= UNIT_MAX) return false;

	ui = &g_table_unitInfo[unitType];

	/* A build price of zero is what separates merchandise from the rest of the
	 * unit pool: the projectiles, the sandworm, the frigate and the Death Hand
	 * all sit in the same table and cost nothing. */
	if (ui->o.buildCredits == 0) return false;

	if (s_starport.specialUnits) return true;

	/* Anything that wants a building of its own is that building's to sell.
	 * Today that is the House of IX, four times over. */
	if (ui->o.structuresRequired != 0) return false;

	return Starport_FactoryBuilds(unitType);
}

/**
 * The shop rule, checked one type at a time.
 *
 * No map and no match: Starport_Sells() reads two static tables and one key, so
 * the test is exactly as wide as the rule is.  What it cannot reach is the
 * order handler, which needs a Starport standing -- that half is covered by
 * refusing the whole order before anything is charged, and by the window never
 * offering what this refuses.
 *
 * @return 1 when everything held, 0 on the first thing that did not.
 */
int Starport_RunRegressionTest(void)
{
	static const uint16 refused[] = {
		UNIT_DEVIATOR, UNIT_DEVASTATOR, UNIT_SONIC_TANK, UNIT_ORNITHOPTER, UNIT_SABOTEUR
	};
	static const uint16 sold[] = {
		UNIT_CARRYALL, UNIT_TANK, UNIT_SIEGE_TANK, UNIT_QUAD, UNIT_HARVESTER, UNIT_MCV,
		UNIT_TRIKE, UNIT_RAIDER_TRIKE, UNIT_SOLDIER, UNIT_INFANTRY, UNIT_TROOPER,
		UNIT_TROOPERS, UNIT_LAUNCHER
	};
	/* Priced at nothing, and therefore not merchandise at any setting. */
	static const uint16 never[] = {
		UNIT_MISSILE_HOUSE, UNIT_MISSILE_ROCKET, UNIT_SANDWORM, UNIT_FRIGATE
	};
	const bool configured = s_starport.specialUnits;
	int result = 1;
	uint16 i;

	s_starport.specialUnits = false;

	for (i = 0; i < lengthof(refused); i++) {
		if (Starport_Sells(refused[i])) {
			printf("starport: %s is still for sale\n", g_table_unitInfo[refused[i]].o.name);
			result = 0;
		}
	}

	for (i = 0; i < lengthof(sold); i++) {
		if (!Starport_Sells(sold[i])) {
			printf("starport: %s is no longer for sale\n", g_table_unitInfo[sold[i]].o.name);
			result = 0;
		}
	}

	for (i = 0; i < lengthof(never); i++) {
		if (Starport_Sells(never[i])) {
			printf("starport: %s is not merchandise\n", g_table_unitInfo[never[i]].o.name);
			result = 0;
		}
	}

	/* Every excluded type has to be obtainable some other way, or the rule has
	 * taken a unit out of the game rather than moved it. */
	for (i = 0; i < lengthof(refused); i++) {
		if (refused[i] == UNIT_SABOTEUR) continue;                  /* the Palace, which builds no unit list. */
		if (g_table_unitInfo[refused[i]].o.structuresRequired == 0 || !Starport_FactoryBuilds(refused[i])) {
			printf("starport: %s has nowhere else to come from\n", g_table_unitInfo[refused[i]].o.name);
			result = 0;
		}
	}

	/* And the key sells them again. */
	s_starport.specialUnits = true;
	for (i = 0; i < lengthof(refused); i++) {
		if (!Starport_Sells(refused[i])) {
			printf("starport_special_units=1 did not restore %s\n", g_table_unitInfo[refused[i]].o.name);
			result = 0;
		}
	}
	for (i = 0; i < lengthof(never); i++) {
		if (Starport_Sells(never[i])) result = 0;
	}

	s_starport.specialUnits = configured;

	return result;
}

/** How long the freighter takes, in Starport ticks. */
uint16 Starport_DeliveryTime(uint8 houseID)
{
	uint16 percent = (houseID == HOUSE_ORDOS) ? s_starport.deliveryOrdos : s_starport.delivery;
	uint32 time;

	if (houseID >= HOUSE_MAX) return 0;

	time = ((uint32)g_table_houseInfo[houseID].starportDeliveryTime * percent) / 100;

	return (uint16)min(time, 0xFFFF);
}

House *g_playerHouse = NULL;
HouseType g_playerHouseID = HOUSE_INVALID;
uint16 g_houseMissileCountdown = 0;
uint16 g_playerCreditsNoSilo = 0;
uint16 g_playerCredits = 0; /*!< Credits shown to player as 'current'. */
uint32 g_tickHousePowerMaintenance = 0;

static uint32 s_tickHouseHouse = 0;
static uint32 s_tickHouseStarport = 0;
static uint32 s_tickHouseReinforcement = 0;
static uint32 s_tickHouseMissileCountdown = 0;
static uint32 s_tickHouseStarportAvailability = 0;

static void House_EnsureHarvesterAvailable(uint8 houseID);

/**
 * Loop over all houses, preforming various of tasks.
 */
void GameLoop_House(void)
{
	PoolFindStruct find;
	House *h = NULL;
	bool tickHouse                = false;
	bool tickPowerMaintenance     = false;
	bool tickStarport             = false;
	bool tickReinforcement        = false;
	bool tickMissileCountdown     = false;
	bool tickStarportAvailability = false;

	if (g_debugScenario) return;

	if (s_tickHouseHouse <= g_timerGame) {
		tickHouse = true;
		s_tickHouseHouse = g_timerGame + 900;
	}

	if (g_tickHousePowerMaintenance <= g_timerGame) {
		tickPowerMaintenance = true;
		g_tickHousePowerMaintenance = g_timerGame + 10800;
	}

	if (s_tickHouseStarport <= g_timerGame) {
		tickStarport = true;
		s_tickHouseStarport = g_timerGame + 180;
	}

	if (s_tickHouseReinforcement <= g_timerGame) {
		tickReinforcement = true;
		s_tickHouseReinforcement = g_timerGame + (g_debugGame ? 60 : 600);
	}

	if (s_tickHouseMissileCountdown <= g_timerGame) {
		tickMissileCountdown = true;
		s_tickHouseMissileCountdown = g_timerGame + 60;
	}

	if (s_tickHouseStarportAvailability <= g_timerGame) {
		tickStarportAvailability = true;
		s_tickHouseStarportAvailability = g_timerGame + Starport_RestockTicks();
	}

	if (tickMissileCountdown && g_houseMissileCountdown != 0) {
		g_houseMissileCountdown--;
		Sound_Output_Feedback(g_houseMissileCountdown + 41);

		if (g_houseMissileCountdown == 0) Unit_LaunchHouseMissile(Map_FindLocationTile(4, g_playerHouseID));
	}

	if (tickStarportAvailability) {
		uint16 type;

		/* Pick a random unit to increase starport availability */
		type = Tools_RandomLCG_Range(0, UNIT_MAX - 1);

		/* Increase how many of this unit is available via starport by one.
		 *
		 * Zero means "the freighter does not carry this at all" and stays zero;
		 * selling out writes -1 instead, so a type that has been cleared out
		 * comes back on the next delivery rather than being gone for good.  The
		 * ceiling is per type now: a hold that opens with a dozen Trikes and one
		 * MCV should not refill to the same number of each. */
		if (g_starportAvailable[type] != 0 &&
		    g_starportAvailable[type] < Starport_StockCeiling(g_table_unitInfo[type].o.buildCredits)) {
			if (g_starportAvailable[type] == -1) {
				g_starportAvailable[type] = 1;
			} else {
				g_starportAvailable[type]++;
			}
		}
	}

	if (tickReinforcement) {
		Unit *nu = NULL;
		int i;

		for (i = 0; i < 16; i++) {
			uint16 locationID;
			bool deployed;
			Unit *u;

			if (g_scenario.reinforcement[i].unitID == UNIT_INDEX_INVALID) continue;
			if (g_scenario.reinforcement[i].timeLeft == 0) continue;
			if (--g_scenario.reinforcement[i].timeLeft != 0) continue;

			u = Unit_Get_ByIndex(g_scenario.reinforcement[i].unitID);

			locationID = g_scenario.reinforcement[i].locationID;
			deployed   = false;

			if (locationID >= 4) {
				if (nu == NULL) {
					nu = Unit_Create(UNIT_INDEX_INVALID, UNIT_CARRYALL, u->o.houseID, Tile_UnpackTile(Map_FindLocationTile(Tools_Random_256() & 3, u->o.houseID)), 100);

					if (nu != NULL) {
						nu->o.flags.s.byScenario = true;
						Unit_SetDestination(nu, Tools_Index_Encode(Map_FindLocationTile(locationID, u->o.houseID), IT_TILE));
					}
				}

				if (nu != NULL) {
					u->o.linkedID = nu->o.linkedID;
					nu->o.linkedID = (uint8)u->o.index;
					nu->o.flags.s.inTransport = true;
					g_scenario.reinforcement[i].unitID = UNIT_INDEX_INVALID;
					deployed = true;
				} else {
					/* Failed to create carry-all, try again in a short moment */
					g_scenario.reinforcement[i].timeLeft = 1;
				}
			} else {
				deployed = Unit_SetPosition(u, Tile_UnpackTile(Map_FindLocationTile(locationID, u->o.houseID)));
			}

			if (deployed && g_scenario.reinforcement[i].repeat != 0) {
				tile32 tile;
				tile.x = 0xFFFF;
				tile.y = 0xFFFF;

				g_validateStrictIfZero++;
				u = Unit_Create(UNIT_INDEX_INVALID, u->o.type, u->o.houseID, tile, 0);
				g_validateStrictIfZero--;

				if (u != NULL) {
					g_scenario.reinforcement[i].unitID = u->o.index;
					g_scenario.reinforcement[i].timeLeft = g_scenario.reinforcement[i].timeBetween;
				}
			}
		}
	}

	find.houseID = HOUSE_INVALID;
	find.index   = 0xFFFF;
	find.type    = 0xFFFF;

	while (true) {
		h = House_Find(&find);
		if (h == NULL) break;

		if (tickHouse) {
			/* ENHANCEMENT -- Originally this code was outside the house loop, which seems very odd.
			 *  This problem is considered to be so bad, that the original code has been removed. */
			/* In a match the grace is per house, kept with the base, because
			 * g_playerCreditsNoSilo is one global for one player and two people
			 * would be sharing it -- and which of them got it would depend on
			 * who was watching. */
			if (Match_IsActive()) {
				uint16 maxCredits = Skirmish_House_MaxCredits(h);

				if (h->credits > maxCredits) {
					h->credits = maxCredits;

					if (h->index == g_playerHouseID) {
						GUI_DisplayText(String_Get_ByIndex(STR_INSUFFICIENT_SPICE_STORAGE_AVAILABLE_SPICE_IS_LOST), 1);
					}
				}
			} else if (h->index != g_playerHouseID) {
				/* A skirmish AI starts with credits but no spice storage at all,
				 * so the plain storage clamp would wipe them before the first
				 * Refinery is paid for.  Give it the same grace as the human. */
				uint16 maxCredits = Skirmish_IsActive() ? Skirmish_House_MaxCredits(h) : h->creditsStorage;

				if (h->credits > maxCredits) {
					h->credits = maxCredits;
				}
			} else {
				uint16 maxCredits = max(h->creditsStorage, g_playerCreditsNoSilo);
				if (h->credits > maxCredits) {
					h->credits = maxCredits;

					GUI_DisplayText(String_Get_ByIndex(STR_INSUFFICIENT_SPICE_STORAGE_AVAILABLE_SPICE_IS_LOST), 1);
				}
			}

			if (h->index == g_playerHouseID && !Match_IsActive()) {
				if (h->creditsStorage > g_playerCreditsNoSilo) {
					g_playerCreditsNoSilo = 0;
				}

				if (g_playerCreditsNoSilo == 0 && g_campaignID > 1 && h->credits != 0) {
					if (h->creditsStorage != 0 && ((h->credits * 256 / h->creditsStorage) > 200)) {
						GUI_DisplayText(String_Get_ByIndex(STR_SPICE_STORAGE_CAPACITY_LOW_BUILD_SILOS), 0);
					}
				}

				if (h->credits < 100 && g_playerCreditsNoSilo != 0) {
					GUI_DisplayText(String_Get_ByIndex(STR_CREDITS_ARE_LOW_HARVEST_SPICE_FOR_MORE_CREDITS), 0);
				}
			}
		}

		if (tickHouse) House_EnsureHarvesterAvailable(h->index);

		Skirmish_Economy_Tick(h);

		if (tickStarport && h->starportLinkedID != UNIT_INDEX_INVALID) {
			Unit *u = NULL;

			h->starportTimeLeft--;
			if ((int16)h->starportTimeLeft < 0) h->starportTimeLeft = 0;

			if (h->starportTimeLeft == 0) {
				Structure *s;

				s = Structure_Get_ByIndex(g_structureIndex);
				if (s->o.type == STRUCTURE_STARPORT && s->o.houseID == h->index) {
					u = Unit_CreateWrapper((uint8)h->index, UNIT_FRIGATE, Tools_Index_Encode(s->o.index, IT_STRUCTURE));
				} else {
					PoolFindStruct find2;

					find2.houseID = h->index;
					find2.index   = 0xFFFF;
					find2.type    = STRUCTURE_STARPORT;

					while (true) {
						s = Structure_Find(&find2);
						if (s == NULL) break;
						if (s->o.linkedID != 0xFF) continue;

						u = Unit_CreateWrapper((uint8)h->index, UNIT_FRIGATE, Tools_Index_Encode(s->o.index, IT_STRUCTURE));
						break;
					}
				}

				if (u != NULL) {
					u->o.linkedID = (uint8)h->starportLinkedID;
					h->starportLinkedID = UNIT_INDEX_INVALID;
					u->o.flags.s.inTransport = true;

					Sound_Output_Feedback(38);
				}

				h->starportTimeLeft = (u != NULL) ? g_table_houseInfo[h->index].starportDeliveryTime : 1;
			}
		}

		if (tickHouse) {
			House_CalculatePowerAndCredit(h);
			Structure_CalculateHitpointsMax(h);

			if (h->timerUnitAttack != 0) h->timerUnitAttack--;
			if (h->timerSandwormAttack != 0) h->timerSandwormAttack--;
			if (h->timerStructureAttack != 0) h->timerStructureAttack--;
			if (h->harvestersIncoming > 0 && Unit_CreateWrapper((uint8)h->index, UNIT_HARVESTER, 0) != NULL) h->harvestersIncoming--;
		}

		if (tickPowerMaintenance) {
			uint16 powerMaintenanceCost = (h->powerUsage / 32) + 1;
			h->credits -= min(h->credits, powerMaintenanceCost);
		}
	}
}

/**
 * Convert the name of a house to the type value of that house, or
 *  HOUSE_INVALID if not found.
 */
uint8 House_StringToType(const char *name)
{
	uint8 index;
	if (name == NULL) return HOUSE_INVALID;

	for (index = 0; index < 6; index++) {
		if (strcasecmp(g_table_houseInfo[index].name, name) == 0) return index;
	}

	return HOUSE_INVALID;
}

/**
 * Gives a harvester to the given house if it has a refinery and no harvesters.
 *
 * @param houseID The index of the house to give a harvester to.
 */
static void House_EnsureHarvesterAvailable(uint8 houseID)
{
	PoolFindStruct find;
	Structure *s;

	find.houseID = houseID;
	find.type    = 0xFFFF;
	find.index   = 0xFFFF;

	while (true) {
		s = Structure_Find(&find);
		if (s == NULL) break;
		/* ENHANCEMENT -- Dune2 checked the wrong type to skip. LinkedID is a structure for a Construction Yard */
		if (!g_dune2_enhanced && s->o.type == STRUCTURE_HEAVY_VEHICLE) continue;
		if (g_dune2_enhanced && s->o.type == STRUCTURE_CONSTRUCTION_YARD) continue;
		if (s->o.linkedID == UNIT_INVALID) continue;
		if (Unit_Get_ByIndex(s->o.linkedID)->o.type == UNIT_HARVESTER) return;
	}

	find.houseID = houseID;
	find.type    = UNIT_CARRYALL;
	find.index   = 0xFFFF;

	while (true) {
		Unit *u;

		u = Unit_Find(&find);
		if (u == NULL) break;
		if (u->o.linkedID == UNIT_INVALID) continue;
		if (Unit_Get_ByIndex(u->o.linkedID)->o.type == UNIT_HARVESTER) return;
	}

	if (Unit_IsTypeOnMap(houseID, UNIT_HARVESTER)) return;

	find.houseID = houseID;
	find.type    = STRUCTURE_REFINERY;
	find.index   = 0xFFFF;

	s = Structure_Find(&find);
	if (s == NULL) return;

	if (Unit_CreateWrapper(houseID, UNIT_HARVESTER, Tools_Index_Encode(s->o.index, IT_STRUCTURE)) == NULL) return;

	if (houseID != g_playerHouseID) return;

	GUI_DisplayText(String_Get_ByIndex(STR_HARVESTER_IS_HEADING_TO_REFINERY), 0);
}

/**
 * Checks if two houses are allied.
 *
 * @param houseID1 The index of the first house.
 * @param houseID2 The index of the second house.
 * @return True if and only if the two houses are allies of eachother.
 */
/**
 * Put the module's schedulers back to the start of time.
 *
 * They hold absolute deadlines -- "next run at g_timerGame + delta" -- so a
 * match that starts with the clock at zero inherits deadlines from the last one
 * and simply does not run until the clock catches up.  Every match has to begin
 * from a known state, which matters for a rematch, for a reconnect, and for any
 * harness that plays twice in one process.  See mp.md.
 */
void House_ResetTicks(void)
{
	s_tickHouseHouse                = 0;
	s_tickHouseStarport             = 0;
	s_tickHouseReinforcement        = 0;
	s_tickHouseMissileCountdown     = 0;
	s_tickHouseStarportAvailability = 0;
	g_tickHousePowerMaintenance     = 0;
}

bool House_AreAllied(uint8 houseID1, uint8 houseID2)
{
	if (houseID1 == HOUSE_INVALID || houseID2 == HOUSE_INVALID) return false;

	if (houseID1 == houseID2) return true;

	if (houseID1 == HOUSE_FREMEN || houseID2 == HOUSE_FREMEN) {
		return (houseID1 == HOUSE_ATREIDES || houseID2 == HOUSE_ATREIDES);
	}

	/* In a match the slots are the whole world, and with two of them the matrix
	 * is one line: anyone who is not you is against you.  Written the old way it
	 * was the spectator who decided, and a skirmish is watched by one who owns
	 * nothing -- which made the two AIs allies of each other: no target scored
	 * above zero, no team ever moved, and both armies stood around. */
	if (Match_IsActive()) return !Match_AreEnemies(houseID1, houseID2);

	/* Outside a match Dune II only ever has one enemy, the player, so every
	 * other pair of houses is allied by definition. */
	return (houseID1 != g_playerHouseID && houseID2 != g_playerHouseID);
}

/**
 * Updates the radar state for the given house.
 * @param h The house.
 * @return True if and only if the radar has been activated.
 */
bool House_UpdateRadarState(House *h)
{
	void *wsa;
	uint16 frame;
	uint16 frameCount;
	bool activate;
	bool onScreen;

	if (h == NULL) return false;

	onScreen = (h->index == g_playerHouseID);

	/* Radar state is the house's own and it is saved with the house, so in a
	 * match both clients have to agree on every house's -- the decision below is
	 * made for anybody who asks, and only the animation belongs to whoever is
	 * watching.  Outside a match nobody but the player has a radar to update. */
	if (!Match_IsActive() && !onScreen) return false;

	/* The skirmish spectator owns no Outpost and produces no power, yet the
	 * whole point of watching is seeing the minimap.  Not in a match: there the
	 * convenience would set a *saved* flag on whichever house this client is
	 * watching, so the two clients would disagree about the world within a
	 * second of starting.  In a match nobody is a spectator anyway. */
	if (!Match_IsActive() && Skirmish_IsActive() && onScreen && !Match_IsHumanControlled(h->index)) {
		h->flags.radarActivated = true;
		return true;
	}

	wsa = NULL;

	activate = h->flags.radarActivated;

	if (h->flags.radarActivated) {
		/* Deactivate radar */
		if ((h->structuresBuilt & (1 << STRUCTURE_OUTPOST)) == 0 || h->powerProduction < h->powerUsage) activate = false;
	} else {
		/* Activate radar */
		if ((h->structuresBuilt & (1 << STRUCTURE_OUTPOST)) != 0 && h->powerProduction >= h->powerUsage) activate = true;
	}

	if (h->flags.radarActivated == activate) return false;

	/* Somebody else's radar: record it and go.  The two seconds of STATIC.WSA
	 * are for the person watching, and in a match they are for one of the two
	 * -- so it cannot be what decides the state.  See mp.md, section 6.
	 *
	 * A networked match takes the same exit for its own radar as well: the
	 * animation is a blocking loop of WSA frames with a Timer_Sleep(3) in it,
	 * and while it runs this client sends no packets, so the other one stalls
	 * for two seconds staring at a frozen battle. */
	if (!onScreen || MpTurn_IsActive()) {
		h->flags.radarActivated = activate;
		return activate;
	}

	wsa = WSA_LoadFile("STATIC.WSA", GFX_Screen_Get_ByIndex(SCREEN_1), GFX_Screen_GetSize_ByIndex(SCREEN_1), true);
	frameCount = WSA_GetFrameCount(wsa);

	g_textDisplayNeedsUpdate = true;

	GUI_Mouse_Hide_Safe();

	while (Driver_Voice_IsPlaying()) sleepIdle();

	Voice_Play(62);

	Sound_Output_Feedback(activate ? 28 : 29);

	frameCount = WSA_GetFrameCount(wsa);

	for (frame = 0; frame < frameCount; frame++) {
		WSA_DisplayFrame(wsa, activate ? frameCount - frame : frame, 256, 136, SCREEN_0);
		GUI_PaletteAnimate();

		Timer_Sleep(3);
	}

	h->flags.radarActivated = activate;

	WSA_Unload(wsa);

	g_viewport_forceRedraw = true;

	GUI_Mouse_Show_Safe();

	GUI_Widget_Viewport_RedrawMap(SCREEN_0);

	return activate;
}

/**
 * Update the CreditsStorage by walking over all structures and checking what
 *  they can hold.
 * @param houseID The house to check the storage for.
 */
void House_UpdateCreditsStorage(uint8 houseID)
{
	PoolFindStruct find;
	uint32 creditsStorage;

	uint16 oldValidateStrictIfZero = g_validateStrictIfZero;
	g_validateStrictIfZero = 0;

	find.houseID = houseID;
	find.index   = 0xFFFF;
	find.type    = 0xFFFF;

	creditsStorage = 0;
	while (true) {
		const StructureInfo *si;
		Structure *s;

		s = Structure_Find(&find);
		if (s == NULL) break;

		si = &g_table_structureInfo[s->o.type];
		creditsStorage += si->creditsStorage;
	}

	if (creditsStorage > 32000) creditsStorage = 32000;

	House_Get_ByIndex(houseID)->creditsStorage = creditsStorage;

	g_validateStrictIfZero = oldValidateStrictIfZero;
}

/**
 * Calculate the power usage and production, and the credits storage.
 *
 * @param h The house to calculate the numbers for.
 */
void House_CalculatePowerAndCredit(House *h)
{
	PoolFindStruct find;

	if (h == NULL) return;

	h->powerUsage      = 0;
	h->powerProduction = 0;
	h->creditsStorage  = 0;

	find.houseID = h->index;
	find.index   = 0xFFFF;
	find.type    = 0xFFFF;

	while (true) {
		const StructureInfo *si;
		Structure *s;

		s = Structure_Find(&find);
		if (s == NULL) break;
		/* ENHANCEMENT -- Only count structures that are placed on the map, not ones we are building. */
		if (g_dune2_enhanced && s->o.flags.s.isNotOnMap) continue;

		si = &g_table_structureInfo[s->o.type];

		h->creditsStorage += si->creditsStorage;

		/* Positive values means usage */
		if (si->powerUsage >= 0) {
			h->powerUsage += si->powerUsage;
			continue;
		}

		/* Negative value and full health means everything goes to production */
		if (s->o.hitpoints >= si->o.hitpoints) {
			h->powerProduction += -si->powerUsage;
			continue;
		}

		/* Negative value and partial health, calculate how much should go to production (capped at 50%) */
		/* ENHANCEMENT -- The 50% cap of Dune2 is silly and disagress with the GUI. If your hp is 10%, so should the production. */
		if (!g_dune2_enhanced && s->o.hitpoints <= si->o.hitpoints / 2) {
			h->powerProduction += (-si->powerUsage) / 2;
			continue;
		}
		h->powerProduction += (-si->powerUsage) * s->o.hitpoints / si->o.hitpoints;
	}

	/* Check if we are low on power */
	if (h->index == g_playerHouseID && h->powerUsage > h->powerProduction) {
		GUI_DisplayText(String_Get_ByIndex(STR_INSUFFICIENT_POWER_WINDTRAP_IS_NEEDED), 1);
	}

	/* If there are no buildings left, you lose your right on 'credits without storage' */
	if (h->structuresBuilt == 0 && g_validateStrictIfZero == 0) {
		if (Match_IsActive()) {
			Skirmish_House_LoseNoSilo((uint8)h->index);
		} else if (h->index == g_playerHouseID) {
			g_playerCreditsNoSilo = 0;
		}
	}
}

const char *House_GetWSAHouseFilename(uint8 houseID)
{
	static const char *houseWSAFileNames[3] = { "FHARK.WSA", "FARTR.WSA", "FORDOS.WSA" };

	if (houseID >= 3) return NULL;
	return houseWSAFileNames[houseID];
}
