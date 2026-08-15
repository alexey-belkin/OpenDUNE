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

/**
 * Everything the metrics suite scores a doctrine on, for one House, for one
 * match.  Raw counts only -- the ratios and the pass marks are the suite's job,
 * because a target value is a judgement and this is measurement.
 */
typedef struct DoctrineMetrics {
	uint32 turretEntries;                                   /*!< Crossings into an envelope. */
	uint32 turretDwell;                                     /*!< Samples taken inside one, i.e. time spent. */
	uint32 turretDeathsLoose;                               /*!< Killed by a turret outside an assault. */
	uint32 turretDeathsAssault;                             /*!< Killed by a turret during one. */
	uint32 turretsKilled;                                   /*!< Enemy turrets destroyed, the other half of the trade. */
	uint32 harvesterLost;
	uint32 harvesterLostEarly;                              /*!< Before t100000. */
	uint32 harvesterKilled;                                 /*!< Enemy harvesters killed. */
	uint32 harvesterExposed;                                /*!< Samples with one within eight tiles of a gun. */
	uint32 harvesterSamples;
	uint32 cohesionOn;                                      /*!< Attackers on the wave, summed over assault samples. */
	uint32 cohesionAll;                                     /*!< Attackers in existence, over the same samples. */
	uint32 wavesLaunched;
	uint32 wavesAborted;
	uint32 wavesDeclined;
	uint32 firstAssault;                                    /*!< Tick, or 0 if there never was one. */
} DoctrineMetrics;

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
extern uint16 Doctrine_ThreatDistance(uint8 houseID, uint16 packed);
extern void Doctrine_HarvesterExposure(uint8 houseID, uint16 *near8, uint16 *worst);
extern uint16 Doctrine_DangerAt(uint8 houseID, uint16 packed);
extern bool Doctrine_IsTurretTarget(uint16 encoded);
extern bool Doctrine_MayEnterTurretZone(const struct Unit *u);
extern bool Doctrine_TargetIsCovered(uint8 houseID, uint16 encoded);
extern bool Doctrine_IsInTurretZone(const struct Unit *u);
extern bool Doctrine_IsLeaving(const struct Unit *u);
extern uint16 Doctrine_CoveringTurret(uint8 houseID, uint16 packed);
extern bool Doctrine_TurretExclusion(struct Unit *unit);
extern void Doctrine_RecordHit(struct Unit *victim, uint16 originEncoded);
extern void Doctrine_RecordDeath(uint8 houseID, uint16 unitIndex);
extern void Doctrine_RecordTurretKilled(uint8 killer);
extern void Doctrine_RecordHarvesterLoss(uint8 owner, uint8 killer);
extern bool Doctrine_GetProduction(uint8 houseID, char *buf, uint16 length);
extern bool Doctrine_GetTelemetry(uint8 houseID, char *buf, uint16 length);
extern bool Doctrine_GetSummary(uint8 houseID, char *buf, uint16 length);
extern void Doctrine_GetMetrics(uint8 houseID, DoctrineMetrics *out);

#endif /* DOCTRINE_H */
