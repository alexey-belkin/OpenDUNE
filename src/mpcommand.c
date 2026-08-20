/** @file src/mpcommand.c The one way a player changes the world. */

#include <stdio.h>
#include <string.h>
#include "types.h"
#include "os/common.h"
#include "os/math.h"

#include "mpcommand.h"

#include "pool/structure.h"
#include "structure.h"
#include "timer.h"
#include "unit.h"

typedef struct MpRecord {
	uint32 tick;
	MpCommand cmd;
} MpRecord;

/* Enough for a long match: a person issues a few orders a second at most, and
 * the replay test needs to hold the whole run. */
static MpRecord s_record[4096];
static uint16 s_recordCount = 0;
static bool s_recording = false;

void MpCommand_Init(MpCommand *cmd, MpCommandType type, uint8 houseID)
{
	memset(cmd, 0, sizeof(*cmd));
	cmd->type    = (uint8)type;
	cmd->houseID = houseID;
	cmd->action  = ACTION_INVALID;
	cmd->packed  = 0xFFFF;
	cmd->object  = 0xFFFF;
	cmd->value   = 0xFFFF;
}

/**
 * The choke point.  Everything a player does to the world goes through here and
 * nothing reaches the simulation any other way.
 *
 * Locally there is nothing between submitting a command and doing it.  In a
 * networked match this is where it is stamped with turn T+D and handed to the
 * relay instead, and Execute() runs later, when every player's packet for that
 * turn has arrived.  Keeping the two apart now is the whole point: it is what
 * makes a replay possible, and a replay is how the choke point is proved
 * complete.  See mp.md.
 */
void MpCommand_Submit(const MpCommand *cmd)
{
	if (cmd == NULL || cmd->type == MP_CMD_NONE) return;

	if (s_recording && s_recordCount < lengthof(s_record)) {
		s_record[s_recordCount].tick = g_timerGame;
		s_record[s_recordCount].cmd  = *cmd;
		s_recordCount++;
	}

	MpCommand_Execute(cmd);
}

/**
 * Apply a command to the simulation.
 *
 * Nothing here may read the local selection, the camera or anything else the
 * other client has its own copy of: a command carries everything it needs.
 */
void MpCommand_Execute(const MpCommand *cmd)
{
	Structure *s;

	if (cmd == NULL) return;

	switch (cmd->type) {
		case MP_CMD_UNIT_ORDER:
			UnitSelection_ApplyOrderToList(cmd->unit, cmd->count, (ActionType)cmd->action, cmd->packed);
			break;

		case MP_CMD_UNIT_DEFAULT_ORDER:
			UnitSelection_ApplyDefaultOrderToList(cmd->unit, cmd->count, cmd->houseID, cmd->packed);
			break;

		case MP_CMD_UNIT_ACTION:
			UnitSelection_ApplyActionToList(cmd->unit, cmd->count, (ActionType)cmd->action);
			break;

		case MP_CMD_UNIT_HUNT:
			UnitSelection_ApplyHuntToList(cmd->unit, cmd->count);
			break;

		case MP_CMD_UNIT_AIR_TRANSIT:
			UnitSelection_ApplyAirTransitToList(cmd->unit, cmd->count, cmd->packed);
			break;

		case MP_CMD_STRUCTURE_BUILD:
			if (cmd->object >= STRUCTURE_INDEX_MAX_HARD) break;
			s = Structure_Get_ByIndex(cmd->object);
			if (s == NULL || !s->o.flags.s.used) break;
			Structure_BuildObject(s, cmd->value);
			break;

		default:
			break;
	}
}

void MpCommand_RecordBegin(void)
{
	s_recordCount = 0;
	s_recording = true;
}

void MpCommand_RecordEnd(void)
{
	s_recording = false;
}

uint16 MpCommand_GetRecordCount(void)
{
	return s_recordCount;
}

const MpCommand *MpCommand_GetRecorded(uint16 index, uint32 *tick)
{
	if (index >= s_recordCount) return NULL;
	if (tick != NULL) *tick = s_record[index].tick;

	return &s_record[index].cmd;
}
