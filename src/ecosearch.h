/** @file src/ecosearch.h Economy opening search definitions. */

#ifndef ECOSEARCH_H
#define ECOSEARCH_H

#include "skirmish.h"

extern void EcoSearch_SetTrace(bool trace);
extern void EcoSearch_Run(uint16 population, uint16 generations, uint32 ticks, uint16 maps);
extern void EcoSearch_RunBaseline(uint32 ticks, uint16 maps);
extern void EcoSearch_RunGrid(uint32 ticks, uint16 maps);
extern void EcoSearch_RunQueueSweep(uint32 ticks, uint16 maps);
extern void EcoSearch_RunCarryallSweep(uint32 ticks, uint16 maps);
extern void EcoSearch_MakePlan(uint16 refineries, uint16 harvesters, uint16 carryalls, uint16 refineryWait, uint16 carryallWait, SkirmishEconomyPlan *out);

#endif /* ECOSEARCH_H */
