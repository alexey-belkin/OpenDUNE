/** @file src/gui/lobby.h Setting up a match against another person. */

#ifndef GUI_LOBBY_H
#define GUI_LOBBY_H

enum {
	LOBBY_CODE_MAX  = 17,                                   /*!< Game code, terminator included. */
	LOBBY_RELAY_MAX = 64,                                   /*!< host[:port], terminator included. */
	LOBBY_ROOM_MAX  = 96                                    /*!< What is actually sent to the relay. */
};

/**
 * Everything the lobby settles before a match, and nothing else.
 *
 * There is no house field.  What both players must agree on is folded into the
 * room name instead -- see Lobby_BuildRoom() -- so two clients that disagree
 * about anything simply never meet, which is a failure a person can act on.  A
 * mismatch that let them meet would be a desync a few seconds later, which is
 * not.
 */
typedef struct LobbyChoice {
	char   relayHost[LOBBY_RELAY_MAX];                      /*!< Host only; the port is separate. */
	uint16 relayPort;
	char   room[LOBBY_ROOM_MAX];                            /*!< Room name for MpNet_Connect(). */
	uint8  slot;                                            /*!< 0 or 1; which player this is. */
	uint8  house[2];                                        /*!< HouseType per slot. */
	uint32 seed;                                            /*!< Map seed, derived from the code. */
} LobbyChoice;

extern bool GUI_Lobby_Show(LobbyChoice *out);
extern bool GUI_Lobby_Choose(const char *relay, const char *code, uint8 pair, uint8 slot, LobbyChoice *out);
extern int GUI_Lobby_RunSelfTest(void);

/* Resolved by GUI_String_Get_ByIndex() for the window's negative stringIDs. */
extern char *GUI_Lobby_GetLabel(int16 stringID);
extern char *GUI_Lobby_GetEntryTitle(void);

#endif /* GUI_LOBBY_H */
