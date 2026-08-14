/** @file src/warsearch.h Economy versus army balance search definitions. */

#ifndef WARSEARCH_H
#define WARSEARCH_H

#include "skirmish.h"

extern void WarSearch_SetTrace(bool trace);
extern void WarSearch_MakePlan(uint16 share, uint16 shareLate, uint32 switchTick, SkirmishEconomyPlan *out);
extern void WarSearch_RunMatrix(uint32 ticks, uint16 maps);
extern void WarSearch_RunTiming(uint32 ticks, uint16 maps);
extern void WarSearch_RunLadder(uint32 ticks, uint16 maps);

#endif /* WARSEARCH_H */
