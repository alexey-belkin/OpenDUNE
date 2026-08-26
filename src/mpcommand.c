/** @file src/mpcommand.c The one way a player changes the world. */

#include <stdio.h>
#include <string.h>
#include "types.h"
#include "os/common.h"
#include "os/math.h"

#include "mpcommand.h"
#include "mpturn.h"

#include "opendune.h"
#include "pool/house.h"
#include "pool/structure.h"
#include "house.h"
#include "structure.h"
#include "timer.h"
#include "tools.h"
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

	/* Playing in turns: nothing happens now.  The command is stamped for a turn
	 * far enough ahead that every player's copy will have arrived by the time it
	 * runs, and MpTurn_Advance() executes it there.  This one line is what makes
	 * the whole interface lockstep-ready -- everything else already submits. */
	if (MpTurn_IsActive()) {
		MpTurn_Submit(cmd);
		return;
	}

	if (s_recording && s_recordCount < lengthof(s_record)) {
		s_record[s_recordCount].tick = g_timerGame;
		s_record[s_recordCount].cmd  = *cmd;
		s_recordCount++;
	}

	MpCommand_Execute(cmd);
}

/**
 * Put the structure a Construction Yard has finished down on the map.
 *
 * The command names the yard rather than the building, because clearing
 * linkedID is simulation state and has to happen on every client at the same
 * tick.  The GUI used to clear it when the player pressed "Place it" and put it
 * back if they cancelled -- local bookkeeping that would have desynced on the
 * first placement, since only one client's player presses anything.
 *
 * The two side effects are the reason this is not simply Structure_Place():
 * a Palace remembers where it went, and a Refinery comes with a harvester.  Both
 * lived in the viewport handler, and the harvester was created for
 * g_playerHouseID -- correct while only one house could ever place anything, and
 * a gift to the wrong player the moment two can.
 */
static void MpCommand_ExecuteStructurePlace(Structure *yard, uint16 packed)
{
	uint8 houseID = yard->o.houseID;
	Structure *s;
	uint16 type;
	House *h;

	/* Whichever of the yard's finished buildings is next, and the whole of the
	 * bookkeeping that goes with taking it: see Structure_Queue_PlaceReady().
	 * The count of spots already clicked for is released whether or not this
	 * one lands, so a refused spot does not strand the cursor. */
	s = Structure_Queue_PlaceReady(yard, packed, &type);
	Structure_Queue_PlaceRelease(yard);
	if (s == NULL) return;

	h = House_Get_ByIndex(houseID);
	if (h == NULL) return;

	if (type == STRUCTURE_PALACE) h->palacePosition = s->o.position;

	if (type == STRUCTURE_REFINERY && g_validateStrictIfZero == 0) {
		Unit *u;

		g_validateStrictIfZero++;
		u = Unit_CreateWrapper(houseID, UNIT_HARVESTER, Tools_Index_Encode(s->o.index, IT_STRUCTURE));
		g_validateStrictIfZero--;

		if (u == NULL) {
			h->harvestersIncoming++;
		} else {
			u->originEncoded = Tools_Index_Encode(s->o.index, IT_STRUCTURE);
		}
	}
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

		case MP_CMD_STRUCTURE_PLACE:
			if (cmd->object >= STRUCTURE_INDEX_MAX_HARD) break;
			s = Structure_Get_ByIndex(cmd->object);
			if (s == NULL || !s->o.flags.s.used) break;
			if (s->o.houseID != cmd->houseID) break;
			MpCommand_ExecuteStructurePlace(s, cmd->packed);
			break;

		case MP_CMD_STRUCTURE_REPAIR:
			if (cmd->object >= STRUCTURE_INDEX_MAX_HARD) break;
			s = Structure_Get_ByIndex(cmd->object);
			if (s == NULL || !s->o.flags.s.used) break;
			if (s->o.houseID != cmd->houseID) break;

			/* A toggle, not a state: both clients hold the same flags, so both
			 * resolve it the same way.  The widget the click handler used to
			 * pass in was the button's own highlight -- presentation, and the
			 * other client has no such button.  Both functions ignore a NULL
			 * one. */
			if (Structure_SetRepairingState(s, -1, NULL)) break;
			Structure_SetUpgradingState(s, -1, NULL);
			break;

		case MP_CMD_STRUCTURE_STARPORT:
			if (cmd->object >= STRUCTURE_INDEX_MAX_HARD) break;
			s = Structure_Get_ByIndex(cmd->object);
			if (s == NULL || !s->o.flags.s.used) break;
			if (s->o.houseID != cmd->houseID) break;
			Structure_StarportOrder(s, cmd->unit, cmd->count, cmd->value);
			break;

		case MP_CMD_STRUCTURE_QUEUE:
			if (cmd->object >= STRUCTURE_INDEX_MAX_HARD) break;
			s = Structure_Get_ByIndex(cmd->object);
			if (s == NULL || !s->o.flags.s.used) break;
			if (s->o.houseID != cmd->houseID) break;

			/* The queue is not saved state, and it still decides what the
			 * factory builds next -- so a queue filled on one client alone is a
			 * factory that produces different things on the two machines. */
			if (cmd->value != 0) {
				Structure_Queue_RemoveOrder(s);
			} else if (Structure_Queue_AddOrder(s)) {
				s->o.flags.s.onHold = false;
			}
			break;

		case MP_CMD_STRUCTURE_RALLY:
			if (cmd->object >= STRUCTURE_INDEX_MAX_HARD) break;
			s = Structure_Get_ByIndex(cmd->object);
			if (s == NULL || !s->o.flags.s.used) break;
			if (s->o.houseID != cmd->houseID) break;
			/* Not saved state, and still a desync: the rally point is what a
			 * new unit is ordered to drive to, so a factory pointed somewhere
			 * on one client builds units that go somewhere else. */
			Structure_SetRallyPoint(s, cmd->packed);
			break;

		case MP_CMD_STRUCTURE_SPECIAL:
			if (cmd->object >= STRUCTURE_INDEX_MAX_HARD) break;
			s = Structure_Get_ByIndex(cmd->object);
			if (s == NULL || !s->o.flags.s.used) break;
			if (s->o.houseID != cmd->houseID) break;
			Structure_ActivateSpecial(s);
			break;

		case MP_CMD_HOUSE_MISSILE:
			/* Aiming draws randoms and frees a unit, so it happens on both
			 * clients or neither. */
			Unit_LaunchHouseMissile(cmd->packed);
			break;

		case MP_CMD_STRUCTURE_HOLD:
			if (cmd->object >= STRUCTURE_INDEX_MAX_HARD) break;
			s = Structure_Get_ByIndex(cmd->object);
			if (s == NULL || !s->o.flags.s.used) break;
			if (s->o.houseID != cmd->houseID) break;

			if (cmd->value != 0) {
				s->o.flags.s.onHold = true;
			} else {
				s->o.flags.s.repairing = false;
				s->o.flags.s.onHold    = false;
				s->o.flags.s.upgrading = false;
			}
			break;

		default:
			break;
	}
}

/**
 * Write the recording out.
 *
 * Text rather than a packed struct, and deliberately: the point of a recording
 * is to be read when a replay disagrees, and a line per command diffs where a
 * binary blob only says "different".  The volume is nothing -- a long match is a
 * few hundred lines.
 */
bool MpCommand_SaveRecord(const char *filename, uint32 seed)
{
	FILE *fp;
	uint16 i;

	fp = fopen(filename, "w");
	if (fp == NULL) return false;

	fprintf(fp, "opendune-commands 1\n");
	fprintf(fp, "seed %u\n", (unsigned)seed);
	fprintf(fp, "count %u\n", (unsigned)s_recordCount);

	for (i = 0; i < s_recordCount; i++) {
		const MpCommand *cmd = &s_record[i].cmd;
		uint16 u;

		fprintf(fp, "cmd %u %u %u %u %u %u %u %u",
		        (unsigned)s_record[i].tick, (unsigned)cmd->type, (unsigned)cmd->houseID,
		        (unsigned)cmd->action, (unsigned)cmd->packed, (unsigned)cmd->object,
		        (unsigned)cmd->value, (unsigned)cmd->count);

		for (u = 0; u < cmd->count && u < MP_COMMAND_UNITS_MAX; u++) {
			fprintf(fp, " %u", (unsigned)cmd->unit[u]);
		}

		fprintf(fp, "\n");
	}

	fclose(fp);
	return true;
}

/**
 * Read a recording back, refusing one made on a different map.
 *
 * A recording only means anything against the match it was taken from, and
 * replaying it onto another seed would diverge for a reason that has nothing to
 * do with the command layer -- which is exactly the sort of false alarm a
 * desync hunt does not need.
 */
bool MpCommand_LoadRecord(const char *filename, uint32 seed)
{
	char header[64];
	unsigned version = 0;
	unsigned fileSeed = 0;
    unsigned count = 0;
	FILE *fp;
	uint16 i;

	fp = fopen(filename, "r");
	if (fp == NULL) return false;

	if (fscanf(fp, "%31s %u", header, &version) != 2 || strcmp(header, "opendune-commands") != 0 || version != 1) {
		fclose(fp);
		return false;
	}
	if (fscanf(fp, " seed %u", &fileSeed) != 1 || fileSeed != seed) {
		fclose(fp);
		return false;
	}
	if (fscanf(fp, " count %u", &count) != 1 || count > lengthof(s_record)) {
		fclose(fp);
		return false;
	}

	s_recordCount = 0;
	for (i = 0; i < (uint16)count; i++) {
		unsigned tick, type, house, action, packed, object, value, num;
		MpCommand *cmd = &s_record[i].cmd;
		uint16 u;

		if (fscanf(fp, " cmd %u %u %u %u %u %u %u %u",
		           &tick, &type, &house, &action, &packed, &object, &value, &num) != 8) {
			fclose(fp);
			return false;
		}
		if (num > MP_COMMAND_UNITS_MAX) {
			fclose(fp);
			return false;
		}

		memset(cmd, 0, sizeof(*cmd));
		s_record[i].tick = (uint32)tick;
		cmd->type    = (uint8)type;
		cmd->houseID = (uint8)house;
		cmd->action  = (uint8)action;
		cmd->packed  = (uint16)packed;
		cmd->object  = (uint16)object;
		cmd->value   = (uint16)value;
		cmd->count   = (uint8)num;

		for (u = 0; u < cmd->count; u++) {
			unsigned index;

			if (fscanf(fp, " %u", &index) != 1) {
				fclose(fp);
				return false;
			}
			cmd->unit[u] = (uint16)index;
		}

		s_recordCount++;
	}

	fclose(fp);
	return true;
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
