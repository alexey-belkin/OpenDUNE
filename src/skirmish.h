/** @file src/skirmish.h %Skirmish (AI vs AI) definitions. */

#ifndef SKIRMISH_H
#define SKIRMISH_H

struct House;
struct Structure;

/** Number of houses taking part in a skirmish. */
#define SKIRMISH_PLAYER_MAX 2

/** Longest base plan a skirmish house can be given.  Most of the room above the
 *  two dozen buildings of an economy is the defence line, which is many cheap
 *  entries rather than a few expensive ones. */
#define SKIRMISH_PLAN_MAX 64

/**
 * An economy opening under test: what to build, in which order, and how many
 * harvesters and carryalls to keep.  This is the genome the economy search
 * evolves -- see [src/ecosearch.c](src/ecosearch.c).
 *
 * In a war it is only half a strategy: the military fields below say how much of
 * the income this economy is allowed to spend on fighting.
 */
typedef struct SkirmishEconomyPlan {
	uint8  build[SKIRMISH_PLAN_MAX];                        /*!< Ordered StructureTypes. */
	uint8  buildCount;
	uint8  harvesterTarget;                                 /*!< Harvesters the Heavy Factory builds towards. */
	uint8  carryallTarget;                                  /*!< Carryalls the Hi-Tech factory builds towards. */
	uint8  starportHarvesters;                              /*!< Harvesters to order from the Starport over the match. */
	uint8  starportCarryalls;                               /*!< Carryalls to order from the Starport over the match. */
	uint8  refineryWait;                                    /*!< Consecutive queue samples before another Refinery is added; 0 disables it. */
	uint16 carryallWait;                                    /*!< Ticks a loaded harvester may spend driving home before another Carryall is built; 0 disables it. */

	uint8  militaryShare;                                   /*!< Percent of lifetime income the house may spend on war, before the switch. */
	uint8  militaryShareLate;                               /*!< Percent after the switch, so a strategy can boom then arm, or the reverse. */
	uint32 militarySwitchTick;                              /*!< Game ticks into the match when the late share takes over. */
} SkirmishEconomyPlan;

extern void Skirmish_SetController(uint8 slot, uint8 controller);
extern void Skirmish_SetViewpoint(uint8 slot);
extern void Skirmish_Rules_Init(void);
extern bool Skirmish_Rules_BaseRock(void);
extern void Skirmish_Rules_SetBaseRock(bool carved);
extern bool Skirmish_Rules_AiPaving(void);
extern void Skirmish_Rules_SetAiPaving(bool timed);
extern bool Skirmish_Rules_AiGuard(void);
extern void Skirmish_Rules_SetAiGuard(bool active);
extern bool Skirmish_IsActive(void);
extern bool Skirmish_IsEconomyMode(void);
extern void Skirmish_Reset(void);
extern bool Skirmish_Start(uint8 houseID1, uint8 houseID2);
extern bool Skirmish_StartEconomy(uint8 houseID, uint32 seed, const SkirmishEconomyPlan *plan);
extern bool Skirmish_StartWar(uint8 houseID1, uint8 houseID2, uint32 seed, const SkirmishEconomyPlan *plan1, const SkirmishEconomyPlan *plan2);
extern void Skirmish_MakeDefaultPlan(SkirmishEconomyPlan *plan);

extern void Skirmish_War_Charge(const struct Structure *s, uint16 credits);
extern uint32 Skirmish_War_GetValue(uint8 index);
extern uint32 Skirmish_War_GetSpent(uint8 index, bool military);
extern uint32 Skirmish_War_GetIncome(uint8 index);
extern bool Skirmish_War_IsDefeated(uint8 index);

extern uint16 Skirmish_Plan_PickNext(struct House *h);
extern uint16 Skirmish_Plan_TakePosition(struct House *h, uint8 structureType);
extern uint16 Skirmish_Plan_PavingTiles(struct House *h, uint8 structureType);
extern uint16 Skirmish_Plan_List(const struct House *h, uint8 *types, uint16 max);
extern uint16 Skirmish_Plan_History(const struct House *h, uint8 *types, uint16 max);
extern uint16 Skirmish_House_MaxCredits(const struct House *h);
extern void Skirmish_House_LoseNoSilo(uint8 houseID);
extern void Skirmish_Economy_Tick(struct House *h);
extern bool Skirmish_AI_WantsHarvester(const struct House *h);
extern bool Skirmish_AI_FactoryWantsHarvester(const struct House *h, const struct Structure *s);
extern bool Skirmish_AI_WantsCarryall(const struct House *h);
extern bool Skirmish_AI_AllowUnit(const struct House *h, uint16 unitType);
extern uint16 Skirmish_AI_PickUnit(const struct House *h, uint32 buildable);
extern void Skirmish_RecordBuilt(uint8 houseID, uint16 unitType);
extern bool Skirmish_AI_WaveReady(uint8 houseID);
extern void Skirmish_RecordShot(uint8 houseID, uint16 target);
extern bool Skirmish_AI_StarportOrder(struct House *h, struct Structure *s);

extern void Skirmish_Economy_AddHarvested(uint8 houseID, uint16 credits, uint16 structureIndex);
extern uint32 Skirmish_Economy_GetHarvested(uint8 houseID);

extern bool Skirmish_GetSummary(uint8 index, char *buf, uint16 length);
extern bool Skirmish_GetBuildOrder(uint8 index, char *buf, uint16 length);
extern bool Skirmish_GetTeams(uint8 index, char *buf, uint16 length);
extern uint16 Skirmish_GetMapSpice(void);
extern uint16 Skirmish_GetBaseOrigin(uint8 index);
extern uint8 Skirmish_GetBaseHouse(uint8 index);
extern uint8 Skirmish_GetOpponent(uint8 houseID);
extern uint16 Skirmish_GetBaseRally(uint8 houseID);
extern uint16 Skirmish_GetTurretDistance(uint8 houseID, uint16 packed);
extern bool Skirmish_GetBaseRect(uint8 houseID, uint16 *x, uint16 *y, uint16 *width, uint16 *height);
extern uint16 Skirmish_GetUnitsBuilt(uint8 houseID, uint16 unitType);
extern uint16 Skirmish_CountUnitsOfType(uint8 houseID, uint16 unitType);
extern uint16 Skirmish_CountCombatUnits(uint8 houseID);
extern bool Skirmish_LegacyWaveReady(uint8 houseID, uint16 waveSize);
extern void Skirmish_RecordKill(uint8 houseID, bool crushed);
extern void Skirmish_RecordDamage(uint8 houseID, uint16 damage);
extern bool Skirmish_GetTelemetry(uint8 index, char *buf, uint16 length);
extern bool Skirmish_GetCasualties(char *buf, uint16 length);
extern bool Skirmish_GetBystanders(char *buf, uint16 length);
extern bool Skirmish_GetRefineryLoad(char *buf, uint16 length);
extern void Skirmish_DrawStatusOverlay(void);

#endif /* SKIRMISH_H */
