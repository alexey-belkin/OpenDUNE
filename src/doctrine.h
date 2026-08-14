/** @file src/doctrine.h War doctrines: interchangeable AI battle strategies. */

#ifndef DOCTRINE_H
#define DOCTRINE_H

struct House;
struct Unit;

/**
 * A doctrine is everything the AI does with an army once it has one: how units
 * are grouped, when an attack leaves, where it goes and what it shoots.  The
 * economy layer is deliberately outside it -- that is what the economy search
 * tunes, and a battle strategy has to be comparable across the same income.
 *
 * They exist side by side so they can be played against each other:
 * "--doctrine=A,B" gives the first house one and the second house the other on
 * the same map with the same seed, which is the only honest way to tell whether
 * a change to the AI is an improvement or just a different way to lose.
 */
typedef enum DoctrineID {
	DOCTRINE_LEGACY  = 0,                                   /*!< A: engine teams, one wave gate, fixed army mix. */
	DOCTRINE_ECHELON = 1,                                   /*!< B: roles, line of departure, artillery suppression. */

	DOCTRINE_MAX     = 2,
	DOCTRINE_INVALID = 0xFF
} DoctrineID;

/** Roles doctrine B sorts an army into.  Membership is by unit type. */
typedef enum DoctrineRole {
	DOCTRINE_ROLE_ARTILLERY = 0,                            /*!< Outranges a turret: Launcher, Sonic Tank. */
	DOCTRINE_ROLE_ASSAULT   = 1,                            /*!< The mass: Tank, Siege Tank, Deviator. */
	DOCTRINE_ROLE_RAID      = 2,                            /*!< Fast and thin: Trike, Raider, Quad. */
	DOCTRINE_ROLE_GARRISON  = 3,                            /*!< Too slow to march: infantry, Devastator. */

	DOCTRINE_ROLE_MAX       = 4,
	DOCTRINE_ROLE_NONE      = 0xFF
} DoctrineRole;

extern DoctrineID Doctrine_ParseName(const char *name);
extern const char *Doctrine_GetName(DoctrineID id);
extern void Doctrine_SetForIndex(uint8 index, DoctrineID id);
extern DoctrineID Doctrine_GetForHouse(uint8 houseID);
extern bool Doctrine_ParseArgument(const char *arg);
extern void Doctrine_GetSelection(char *buf, uint16 length);

extern void Doctrine_Reset(void);
extern void Doctrine_HouseStart(uint8 houseID);
extern bool Doctrine_UsesEngineTeams(uint8 houseID);

extern void Doctrine_Tick(struct House *h);
extern bool Doctrine_WaveReady(uint8 houseID);
extern bool Doctrine_IsOnWave(const struct Unit *u);
extern uint16 Doctrine_PickUnit(const struct House *h, uint32 buildable);
extern bool Doctrine_AllowUnit(const struct House *h, uint16 unitType);
extern void Doctrine_ForgetUnit(uint16 unitIndex);
extern bool Doctrine_GetProduction(uint8 houseID, char *buf, uint16 length);
extern bool Doctrine_GetTelemetry(uint8 houseID, char *buf, uint16 length);
extern bool Doctrine_GetSummary(uint8 houseID, char *buf, uint16 length);

#endif /* DOCTRINE_H */
