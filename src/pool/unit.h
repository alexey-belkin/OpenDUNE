/** @file src/pool/unit.h %Unit pool definitions. */

#ifndef POOL_UNIT_H
#define POOL_UNIT_H

enum {
	/* Raised from the original 102.  The pool is partitioned by unit type in
	 * g_table_unitInfo (indexStart/indexEnd), and the original split left 80
	 * slots for every ground unit on the map -- both houses, harvesters
	 * included -- and four for every projectile in flight anywhere.  Two AIs
	 * running a full economy hit both ceilings well before they run out of
	 * credits, so extra income stopped converting into strength.
	 *
	 * Savegames store a uint16 index and are loaded by index without a range
	 * check, so a bigger pool still reads every existing save; only the new
	 * high slots are unreachable to the original game. */
	UNIT_INDEX_MAX = 242,                                   /*!< The highest possible index for any Unit. */

	UNIT_INDEX_INVALID = 0xFFFF
};

struct PoolFindStruct;

extern struct Unit *g_unitFindArray[UNIT_INDEX_MAX];
extern uint16 g_unitFindCount;

extern struct Unit *Unit_Get_ByIndex(uint16 index);
extern struct Unit *Unit_Find(struct PoolFindStruct *find);

extern void Unit_Init(void);
extern void Unit_Recount(void);
extern struct Unit *Unit_Allocate(uint16 index, uint8 type, uint8 houseID);
extern void Unit_Free(struct Unit *u);

#endif /* POOL_UNIT_H */
