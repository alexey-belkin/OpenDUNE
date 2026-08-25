/* $Id$ */

/** @file src/pathfinder.h A* route search over the tile grid. */

#ifndef PATHFINDER_H
#define PATHFINDER_H

struct Unit;

extern void Pathfinder_Init(void);
extern void Pathfinder_SetEnabled(bool enabled);
extern bool Pathfinder_IsEnabled(void);

extern bool Pathfinder_Run(struct Unit *unit, uint16 packedSrc, const uint16 *goals, uint16 goalCount, bool stopAtFirstGoal);
extern bool Pathfinder_GetTicks(uint16 packed, uint32 *ticks);
extern uint16 Pathfinder_GetBestEffortTile(void);
extern uint16 Pathfinder_GetRoute(uint16 packedGoal, uint8 *buffer, uint16 bufferSize);

extern uint32 Pathfinder_GetExpansions(void);
extern uint32 Pathfinder_StepTicks(struct Unit *unit, uint16 packed, uint8 orient8);
extern uint32 Pathfinder_StepCost(struct Unit *unit, uint16 packed, uint8 orient8);
extern uint32 Pathfinder_Unreachable(void);
extern bool Pathfinder_CheckHeuristic(struct Unit *unit, uint16 *failedLandscape);
extern uint32 Pathfinder_GetHeuristic(uint16 packed);

#endif /* PATHFINDER_H */
