/** @file src/gui/lobby.c Setting up a match against another person.
 *
 * Everything a networked match needs used to come off the command line
 * (mp.md, "What is still missing": a lobby).  This is that lobby, and its one
 * design decision is worth stating before the code:
 *
 * **Whatever the two players must agree on goes into the room name.**  The
 * relay puts two clients together only when they ask for the same room, so a
 * disagreement about the map, the houses, the build or the ini means they never
 * meet -- and "the other player never joined" is something a person can act on.
 * The alternative is to let them meet and disagree, which is a desync a few
 * seconds later and looks like the game being broken.
 *
 * That is why there is no separate handshake here.  The room name *is* the
 * handshake, and the relay enforces it without knowing that it does.
 */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "types.h"
#include "../os/sleep.h"
#include "../os/strings.h"

#include "lobby.h"
#include "gui.h"
#include "widget.h"
#include "../file.h"
#include "../gfx.h"
#include "../house.h"
#include "../inifile.h"
#include "../input/input.h"
#include "../mpsync.h"
#include "../rev.h"
#include "../sprites.h"
#include "../string.h"
#include "../structure.h"
#include "../table/strings.h"
#include "../unit.h"

/** The public relay, and the port the one in tools/relay listens on. */
#define LOBBY_RELAY_DEFAULT "146.103.110.160"
#define LOBBY_PORT_DEFAULT  31337

/**
 * The six ordered pairs of the three houses.
 *
 * Ordered rather than combinations, because slot 1 takes the first and slot 2
 * the second: "Atreides against Harkonnen" and "Harkonnen against Atreides" are
 * the same match seen from two chairs, and the row has to be able to say which
 * chair this player is in.
 */
static const uint8 s_pairs[6][2] = {
	{ HOUSE_ATREIDES,  HOUSE_HARKONNEN },
	{ HOUSE_ATREIDES,  HOUSE_ORDOS     },
	{ HOUSE_ORDOS,     HOUSE_HARKONNEN },
	{ HOUSE_HARKONNEN, HOUSE_ATREIDES  },
	{ HOUSE_ORDOS,     HOUSE_ATREIDES  },
	{ HOUSE_HARKONNEN, HOUSE_ORDOS     }
};

static char s_code[LOBBY_CODE_MAX] = "";
static char s_relay[LOBBY_RELAY_MAX] = "";
static uint8 s_pair = 0;
static uint8 s_slot = 0;

/** Row labels, filled in by Lobby_Labels() and read back by the widget draw. */
static char s_label[5][64];
static char s_entryTitle[64] = "";

/**
 * Fold one table entry's common half into the digest, without its pointers.
 *
 * ObjectInfo carries `name` and `wsa`, which are addresses in *this* process
 * and have nothing to do with the balance.  Hashing them raw made the digest
 * different on every launch -- the loader puts the image somewhere else each
 * time -- so two copies of one build asked the relay for different rooms and
 * never met each other.  The bug could not be seen from inside one process,
 * which is exactly what the first version of the self-test compared.
 *
 * Everything else in a static table is the same bytes in every run of the same
 * binary, padding included, so the three spans around the pointers can be taken
 * as they lie.
 */
static uint32 Lobby_HashObject(uint32 crc, const ObjectInfo *o)
{
	const uint8 *base = (const uint8 *)o;

	crc = MpSync_Crc32(crc, base, (uint32)offsetof(ObjectInfo, name));
	crc = MpSync_Crc32(crc, base + offsetof(ObjectInfo, stringID_full),
	                   (uint32)(offsetof(ObjectInfo, wsa) - offsetof(ObjectInfo, stringID_full)));
	crc = MpSync_Crc32(crc, base + offsetof(ObjectInfo, flags),
	                   (uint32)(sizeof(ObjectInfo) - offsetof(ObjectInfo, flags)));

	return crc;
}

/**
 * A digest of everything about this build that changes what the simulation
 * does.
 *
 * The two tables are the whole of it: the balance module and the unit tuning
 * both work by patching them, so hashing the tables catches every ini key that
 * matters and ignores every one that does not -- whitespace, comments, a key
 * written out at its default value.  The revision is in there because two
 * different builds of the same tables can still differ anywhere else.
 */
static uint32 Lobby_ConfigHash(void)
{
	uint32 crc;
	uint16 i;

	crc = MpSync_Crc32(0, g_opendune_revision, (uint32)strlen(g_opendune_revision));

	for (i = 0; i < UNIT_MAX; i++) {
		const uint8 *entry = (const uint8 *)&g_table_unitInfo[i];

		crc = Lobby_HashObject(crc, &g_table_unitInfo[i].o);
		crc = MpSync_Crc32(crc, entry + offsetof(UnitInfo, indexStart),
		                   (uint32)(sizeof(UnitInfo) - offsetof(UnitInfo, indexStart)));
	}

	for (i = 0; i < STRUCTURE_MAX; i++) {
		const uint8 *entry = (const uint8 *)&g_table_structureInfo[i];

		crc = Lobby_HashObject(crc, &g_table_structureInfo[i].o);
		crc = MpSync_Crc32(crc, entry + offsetof(StructureInfo, enterFilter),
		                   (uint32)(sizeof(StructureInfo) - offsetof(StructureInfo, enterFilter)));
	}

	return crc;
}

/** The map, from the game code.  Both players type the code, so both get it. */
static uint32 Lobby_Seed(const char *code)
{
	/* Never zero: a seed of zero is a legal map but an easy value to arrive at
	 * by accident, and it would make an empty code silently mean something. */
	return MpSync_Crc32(0, code, (uint32)strlen(code)) | 1;
}

/**
 * The name sent to the relay: the code, the houses, and the build digest.
 *
 * The slot is deliberately absent -- the two players are in the same room, they
 * just take different seats in it.
 */
static void Lobby_BuildRoom(char *dest, uint16 length, const char *code, uint8 pair)
{
	snprintf(dest, length, "%s.%u.%08x", code, (unsigned)pair, (unsigned)Lobby_ConfigHash());
}

static const char *Lobby_HouseName(uint8 houseID)
{
	switch (houseID) {
		case HOUSE_ATREIDES:  return "ATREIDES";
		case HOUSE_ORDOS:     return "ORDOS";
		case HOUSE_HARKONNEN: return "HARKONNEN";
		default:              return "?";
	}
}

/**
 * Rebuild the five value rows.
 *
 * They are rebuilt in one place and read from one place, because a lobby whose
 * rows can disagree with each other is a lobby that will eventually start a
 * match nobody asked for.
 */
static void Lobby_Labels(void)
{
	snprintf(s_label[0], sizeof(s_label[0]), "RELAY  %s", (s_relay[0] != '\0') ? s_relay : "(NOT SET)");
	snprintf(s_label[1], sizeof(s_label[1]), "GAME CODE  %s", (s_code[0] != '\0') ? s_code : "(TYPE ONE)");
	snprintf(s_label[2], sizeof(s_label[2]), "PLAYER  %u OF 2", (unsigned)(s_slot + 1));
	snprintf(s_label[3], sizeof(s_label[3]), "HOUSE  YOU %s, THEM %s",
	         Lobby_HouseName(s_pairs[s_pair][s_slot]),
	         Lobby_HouseName(s_pairs[s_pair][s_slot ^ 1]));

	if (s_code[0] != '\0') {
		snprintf(s_label[4], sizeof(s_label[4]), "MAP  #%u (FROM THE CODE)", (unsigned)(Lobby_Seed(s_code) % 100000));
	} else {
		snprintf(s_label[4], sizeof(s_label[4]), "MAP  (FROM THE CODE)");
	}
}

/** Resolve the negative stringIDs the lobby window uses.  See gui.c. */
char *GUI_Lobby_GetLabel(int16 stringID)
{
	int16 row = (int16)(-20 - stringID);

	if (row < 0 || row >= 5) return NULL;

	return s_label[row];
}

char *GUI_Lobby_GetEntryTitle(void)
{
	return s_entryTitle;
}

/**
 * Type into one of the two text rows.
 *
 * A dialog of its own rather than editing in place: the row is a text button
 * drawn by the window, and an edit box needs a widget rectangle it owns.
 */
static void Lobby_Edit(const char *title, char *value, uint16 maxLength)
{
	WindowDesc *desc = &g_lobbyEntryWindowDesc;
	char buffer[LOBBY_RELAY_MAX];
	bool loop;
	bool paint = true;

	snprintf(buffer, sizeof(buffer), "%s", value);
	snprintf(s_entryTitle, sizeof(s_entryTitle), "%s", title);

	GUI_Window_BackupScreen(desc);
	GUI_Window_Create(desc);

	GFX_Screen_SetActive(SCREEN_0);
	Widget_SetCurrentWidget(15);

	GUI_Mouse_Hide_Safe();
	GUI_DrawBorder((g_curWidgetXBase << 3) - 1, g_curWidgetYBase - 1, (g_curWidgetWidth << 3) + 2, g_curWidgetHeight + 2, 4, false);
	GUI_Mouse_Show_Safe();

	for (loop = true; loop; sleepIdle()) {
		uint16 key;

		GUI_DrawText_Wrapper(NULL, 0, 0, 232, 235, 0x22);

		key = GUI_EditBox(buffer, (uint16)(maxLength - 1), 15, g_widgetLinkedListTail, NULL, paint);
		paint = false;

		if ((key & 0x8000) == 0) continue;

		GUI_Widget_MakeNormal(GUI_Widget_Get_ByIndex(g_widgetLinkedListTail, key & 0x7FFF), false);

		switch (key & 0x7FFF) {
			case 0x1E: /* RETURN / OK */
				snprintf(value, maxLength, "%s", buffer);
				loop = false;
				break;

			case 0x1F: /* ESCAPE / Cancel */
				loop = false;
				break;

			default: break;
		}
	}

	GUI_Window_RestoreScreen(desc);
}

/** Split "host" or "host:port" into the two things MpNet_Connect() wants. */
static void Lobby_SplitRelay(const char *text, char *host, uint16 hostLength, uint16 *port)
{
	const char *colon = strrchr(text, ':');

	*port = LOBBY_PORT_DEFAULT;

	if (colon == NULL) {
		snprintf(host, hostLength, "%s", text);
		return;
	}

	snprintf(host, hostLength, "%.*s", (int)(colon - text), text);
	if (colon[1] != '\0') *port = (uint16)atoi(colon + 1);
	if (*port == 0) *port = LOBBY_PORT_DEFAULT;
}

/**
 * The parts of the lobby that decide a match, without the screen.
 *
 * What is worth testing here is not the drawing but the rule the whole design
 * rests on: two clients that made the same choices must produce the same room
 * string, and any difference at all must produce a different one.  If that ever
 * stops being true the lobby stops protecting anybody, quietly.
 *
 * @return 1 when everything held, 0 on the first thing that did not.
 */
int GUI_Lobby_RunSelfTest(void)
{
	char roomA[LOBBY_ROOM_MAX];
	char roomB[LOBBY_ROOM_MAX];
	char host[LOBBY_RELAY_MAX];
	uint16 port;

	/* Same code, same houses -- the two players have to land in one room. */
	Lobby_BuildRoom(roomA, sizeof(roomA), "dune42", 0);
	Lobby_BuildRoom(roomB, sizeof(roomB), "dune42", 0);
	if (strcmp(roomA, roomB) != 0) return 0;

	/* Different houses, or a different code, and they must not. */
	Lobby_BuildRoom(roomB, sizeof(roomB), "dune42", 1);
	if (strcmp(roomA, roomB) == 0) return 0;

	Lobby_BuildRoom(roomB, sizeof(roomB), "dune43", 0);
	if (strcmp(roomA, roomB) == 0) return 0;

	/* The map follows the code and nothing else, and is never zero. */
	if (Lobby_Seed("dune42") != Lobby_Seed("dune42")) return 0;
	if (Lobby_Seed("dune42") == Lobby_Seed("dune43")) return 0;
	if (Lobby_Seed("") == 0) return 0;

	/* The build digest has to be stable within one process... */
	if (Lobby_ConfigHash() != Lobby_ConfigHash()) return 0;

	/* ...and that is nowhere near enough, which is how the first version of
	 * this shipped broken.  The digest hashed the tables raw, pointers and all,
	 * so it was stable here and different on every launch -- two copies of one
	 * build asked for different rooms and each waited for somebody who was in
	 * the other one.  What has to hold is that the digest does not depend on
	 * where anything happens to live, so move a pointer and demand it does not
	 * budge. */
	{
		const char *savedName = g_table_unitInfo[UNIT_TANK].o.name;
		const char *savedWsa  = g_table_unitInfo[UNIT_TANK].o.wsa;
		char nameCopy[64];
		char wsaCopy[64];
		uint32 before = Lobby_ConfigHash();
		uint32 after;

		snprintf(nameCopy, sizeof(nameCopy), "%s", (savedName != NULL) ? savedName : "");
		snprintf(wsaCopy, sizeof(wsaCopy), "%s", (savedWsa != NULL) ? savedWsa : "");

		g_table_unitInfo[UNIT_TANK].o.name = nameCopy;
		if (savedWsa != NULL) g_table_unitInfo[UNIT_TANK].o.wsa = wsaCopy;

		after = Lobby_ConfigHash();

		g_table_unitInfo[UNIT_TANK].o.name = savedName;
		g_table_unitInfo[UNIT_TANK].o.wsa  = savedWsa;

		if (after != before) return 0;
	}

	/* The other half of the same claim: a real balance difference must always
	 * change it, or the digest protects nobody. */
	{
		uint16 savedDamage = g_table_unitInfo[UNIT_TANK].damage;
		uint16 savedPower  = (uint16)g_table_structureInfo[STRUCTURE_WINDTRAP].powerUsage;
		uint32 before = Lobby_ConfigHash();
		bool ok;

		g_table_unitInfo[UNIT_TANK].damage = (uint16)(savedDamage + 1);
		ok = (Lobby_ConfigHash() != before);
		g_table_unitInfo[UNIT_TANK].damage = savedDamage;

		if (ok) {
			g_table_structureInfo[STRUCTURE_WINDTRAP].powerUsage = (int16)(savedPower + 1);
			ok = (Lobby_ConfigHash() != before);
			g_table_structureInfo[STRUCTURE_WINDTRAP].powerUsage = (int16)savedPower;
		}

		if (!ok) return 0;
		if (Lobby_ConfigHash() != before) return 0;
	}

	/* host, host:port, and the nonsense in between. */
	Lobby_SplitRelay("example.net", host, sizeof(host), &port);
	if (strcmp(host, "example.net") != 0 || port != LOBBY_PORT_DEFAULT) return 0;

	Lobby_SplitRelay("example.net:1234", host, sizeof(host), &port);
	if (strcmp(host, "example.net") != 0 || port != 1234) return 0;

	Lobby_SplitRelay("example.net:", host, sizeof(host), &port);
	if (strcmp(host, "example.net") != 0 || port != LOBBY_PORT_DEFAULT) return 0;

	Lobby_SplitRelay("example.net:0", host, sizeof(host), &port);
	if (port != LOBBY_PORT_DEFAULT) return 0;

	return 1;
}

/**
 * Turn a set of choices into a match, without a screen.
 *
 * The BEGIN row calls this and so does --lobby-play, which is the point: the
 * flag has to exercise the same arithmetic a person's click does, or it guards
 * nothing.
 *
 * @return False when the choices are not enough to start a match.
 */
bool GUI_Lobby_Choose(const char *relay, const char *code, uint8 pair, uint8 slot, LobbyChoice *out)
{
	if (relay == NULL || code == NULL || out == NULL) return false;
	if (relay[0] == '\0' || code[0] == '\0') return false;
	if (pair >= 6 || slot >= 2) return false;

	Lobby_SplitRelay(relay, out->relayHost, sizeof(out->relayHost), &out->relayPort);
	Lobby_BuildRoom(out->room, sizeof(out->room), code, pair);
	out->slot     = slot;
	out->house[0] = s_pairs[pair][0];
	out->house[1] = s_pairs[pair][1];
	out->seed     = Lobby_Seed(code);

	return true;
}

/**
 * Ask for a match, and say whether one was agreed.
 *
 * @param out Filled in when this returns true.
 * @return True to start a match, false when the player backed out.
 */
bool GUI_Lobby_Show(LobbyChoice *out)
{
	WindowDesc *desc = &g_lobbyWindowDesc;
	bool loop;
	bool ret = false;

	if (s_relay[0] == '\0') {
		char fromIni[LOBBY_RELAY_MAX];

		/* Whatever was used last time, if the player wrote it down.  Nobody
		 * wants to type an address twice. */
		if (IniFile_GetString("mp_relay", LOBBY_RELAY_DEFAULT, fromIni, sizeof(fromIni)) != NULL) {
			snprintf(s_relay, sizeof(s_relay), "%s", fromIni);
		} else {
			snprintf(s_relay, sizeof(s_relay), "%s", LOBBY_RELAY_DEFAULT);
		}
	}

	Lobby_Labels();

	GUI_Window_BackupScreen(desc);
	GUI_Window_Create(desc);

	for (loop = true; loop; sleepIdle()) {
		uint16 key = GUI_Widget_HandleEvents(g_widgetLinkedListTail);
		bool redraw = false;

		if ((key & 0x8000) == 0) {
			GUI_PaletteAnimate();
			continue;
		}

		GUI_Widget_MakeNormal(GUI_Widget_Get_ByIndex(g_widgetLinkedListTail, key & 0x7FFF), false);

		switch ((key & 0x7FFF) - 0x1E) {
			case 0: /* RELAY */
				GUI_Window_RestoreScreen(desc);
				Lobby_Edit("RELAY ADDRESS, HOST OR HOST:PORT", s_relay, sizeof(s_relay));
				redraw = true;
				break;

			case 1: /* GAME CODE */
				GUI_Window_RestoreScreen(desc);
				Lobby_Edit("GAME CODE -- BOTH PLAYERS TYPE THE SAME ONE", s_code, sizeof(s_code));
				redraw = true;
				break;

			case 2: /* PLAYER */
				s_slot ^= 1;
				redraw = true;
				break;

			case 3: /* HOUSE */
				s_pair = (uint8)((s_pair + 1) % 6);
				redraw = true;
				break;

			case 4: /* MAP -- derived, so the row only re-rolls by changing the code */
				break;

			case 5: /* BEGIN */
				if (!GUI_Lobby_Choose(s_relay, s_code, s_pair, s_slot, out)) break;

				loop = false;
				ret = true;
				break;

			case 6: /* CANCEL */
				loop = false;
				break;

			default: break;
		}

		if (redraw && loop) {
			Lobby_Labels();
			GUI_Window_BackupScreen(desc);
			GUI_Window_Create(desc);
		}
	}

	GUI_Window_RestoreScreen(desc);

	return ret;
}
