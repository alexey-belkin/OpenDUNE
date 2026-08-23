/** @file src/mpcommand.h The one way a player changes the world. */

#ifndef MPCOMMAND_H
#define MPCOMMAND_H

#include "unit.h"

enum {
	MP_COMMAND_UNITS_MAX = UNIT_SELECTION_MAX               /*!< A command names its recipients; a selection is the largest set there is. */
};

/**
 * Every player action that reaches the simulation.
 *
 * Selection, camera, control groups and the minimap are deliberately absent:
 * they are properties of how somebody is holding the mouse, they change no state
 * that is saved, and in a match they must stay local.
 */
typedef enum MpCommandType {
	MP_CMD_NONE = 0,
	MP_CMD_UNIT_ORDER,                                      /*!< Targeted group order: action at a tile. */
	MP_CMD_UNIT_DEFAULT_ORDER,                              /*!< Right click: each unit picks its own. */
	MP_CMD_UNIT_ACTION,                                     /*!< Untargeted order, applied at once. */
	MP_CMD_UNIT_HUNT,                                       /*!< Manual hunt. */
	MP_CMD_UNIT_AIR_TRANSIT,                                /*!< Ask for a lift to a tile. */
	MP_CMD_STRUCTURE_BUILD,                                 /*!< Start, change or cancel what a factory is making. */
	MP_CMD_STRUCTURE_PLACE,                                 /*!< Put a finished structure on the map. */
	MP_CMD_STRUCTURE_HOLD,                                  /*!< Hold or resume production; value 1 holds, 0 resumes. */
	MP_CMD_STRUCTURE_REPAIR,                                /*!< The repair/upgrade button: a toggle, resolved where it runs. */
	MP_CMD_STRUCTURE_STARPORT                               /*!< A Starport order: what, how many, and what it cost. */
} MpCommandType;

/**
 * A command names its recipients rather than referring to "the selection",
 * because the selection is local and the other client has one of its own.
 */
typedef struct MpCommand {
	uint8  type;                                            /*!< MpCommandType. */
	uint8  houseID;                                         /*!< Who issued it. */
	uint8  action;                                          /*!< ActionType, where the type uses one. */
	uint8  count;                                           /*!< Recipients in unit[]. */
	uint16 packed;                                          /*!< Target tile, where the type uses one. */
	uint16 object;                                          /*!< Structure index, where the type uses one. */
	uint16 value;                                           /*!< Payload: the object type a factory should build, or a flag. */
	uint16 unit[MP_COMMAND_UNITS_MAX];                      /*!< Recipient unit indices, or a Starport order's (type << 8 | amount) entries. */
} MpCommand;

extern void MpCommand_Init(MpCommand *cmd, MpCommandType type, uint8 houseID);
extern void MpCommand_Submit(const MpCommand *cmd);
extern void MpCommand_Execute(const MpCommand *cmd);

extern bool MpCommand_SaveRecord(const char *filename, uint32 seed);
extern bool MpCommand_LoadRecord(const char *filename, uint32 seed);

extern void MpCommand_RecordBegin(void);
extern void MpCommand_RecordEnd(void);
extern uint16 MpCommand_GetRecordCount(void);
extern const MpCommand *MpCommand_GetRecorded(uint16 index, uint32 *tick);

#endif /* MPCOMMAND_H */
