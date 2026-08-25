/** @file src/opendune.c Gameloop and other main routines. */

#if defined(_WIN32)
	#if defined(_MSC_VER)
		#define _CRTDBG_MAP_ALLOC
		#include <stdlib.h>
		#include <crtdbg.h>
	#endif /* _MSC_VER */
	#include <io.h>
	#include <windows.h>
#endif /* _WIN32 */
#ifdef TOS
#include <mint/sysbind.h>
#include <mint/osbind.h>
#include <mint/ostruct.h>
#endif /* TOS */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#if defined(WITH_SDL) || defined(WITH_SDL2)
#include <SDL.h>
#ifdef _WIN32
#undef main
#endif
#endif /* WITH_SDL(2) */
#include "types.h"
#include "os/common.h"
#include "os/error.h"
#include "os/math.h"
#include "os/strings.h"
#include "os/sleep.h"

#include "opendune.h"
#include "pathfinder.h"

#include "animation.h"
#include "audio/driver.h"
#include "audio/sound.h"
#include "config.h"
#include "crashlog/crashlog.h"
#include "cutscene.h"
#include "ecosearch.h"
#include "explosion.h"
#include "file.h"
#include "gfx.h"
#include "gui/font.h"
#include "gui/gui.h"
#include "gui/mentat.h"
#include "gui/security.h"
#include "gui/lobby.h"
#include "gui/widget.h"
#include "house.h"
#include "ini.h"
#include "inifile.h"
#include "input/input.h"
#include "input/mouse.h"
#include "load.h"
#include "map.h"
#include "match.h"
#include "mpcommand.h"
#include "mpnet.h"
#include "mpturn.h"
#include "mpsync.h"
#include "pool/pool.h"
#include "pool/house.h"
#include "pool/unit.h"
#include "pool/structure.h"
#include "pool/team.h"
#include "rev.h"
#include "scenario.h"
#include "doctrine.h"
#include "skirmish.h"
#include "sprites.h"
#include "string.h"
#include "structure.h"
#include "table/strings.h"
#include "team.h"
#include "tile.h"
#include "timer.h"
#include "tools.h"
#include "unit.h"
#include "warsearch.h"
#include "video/video.h"

const char *window_caption = "OpenDUNE - v0.9";

bool g_dune2_enhanced = true; /*!< If false, the game acts exactly like the original Dune2, including bugs. */
bool g_starPortEnforceUnitLimit = false;	/*!< If true, one cannot circumvent unit cap using starport */
bool g_unpackSHPonLoad = true;	/*!< If true, Format80 encoded sprites from SHP files will be decoded on load. set to false to save memory */

uint32 g_hintsShown1 = 0;          /*!< A bit-array to indicate which hints has been show already (0-31). */
uint32 g_hintsShown2 = 0;          /*!< A bit-array to indicate which hints has been show already (32-63). */
GameMode g_gameMode = GM_MENU;
uint16 g_campaignID = 0;
uint16 g_scenarioID = 1;
uint16 g_activeAction = 0xFFFF;      /*!< Action the controlled unit will do. */
uint32 g_tickScenarioStart = 0;      /*!< The tick the scenario started in. */
static uint32 s_tickGameTimeout = 0; /*!< The tick the game will timeout. */

bool   g_debugGame = false;        /*!< When true, you can control the AI. */
bool   g_debugScenario = false;    /*!< When true, you can review the scenario. There is no fog. The game is not running (no unit-movement, no structure-building, etc). You can click on individual tiles. */
bool   g_debugSkipDialogs = false; /*!< When non-zero, you immediately go to house selection, and skip all intros. */

void *g_readBuffer = NULL;
uint32 g_readBufferSize = 0;

static bool  s_debugForceWin = false; /*!< When true, you immediately win the level. */

static uint8 s_enableLog = 0; /*!< 0 = off, 1 = record game, 2 = playback game (stored in 'dune.log'). */
static bool s_selectionSelfTest = false;
static int s_selectionSelfTestResult = -1;
static bool s_lobbySelfTest = false;

/* --lobby-play: take the lobby's road into a match without a person clicking
 * through it.  The room, the seed and the houses are computed by the lobby's
 * own code, and the handoff to GM_SKIRMISH is the same one BEGIN takes, which
 * is the half that no self-test could reach. */
static bool s_lobbyPlay = false;
static char s_lobbyPlayRelay[128] = "";
static char s_lobbyPlayCode[32] = "";
static uint8 s_lobbyPlayPair = 0;
static uint8 s_lobbyPlaySlot = 0;
static bool s_lobbyPlayEntered = false;
static int s_lobbySelfTestResult = -1;
static bool s_moveRulesSelfTest = false;
static int s_moveRulesSelfTestResult = -1;
static bool s_buildRulesSelfTest = false;
static int s_buildRulesSelfTestResult = -1;
static bool s_pathfinderSelfTest = false;
static int s_pathfinderSelfTestResult = -1;
static int s_pathfinderOverride = -1;
static bool s_combatBalanceSelfTest = false;
static int s_combatBalanceSelfTestResult = -1;
/* Extra simulation passes per loop iteration, on top of whatever the Game
 * Controls speed setting already does.  Powers of two only: the '[' and ']'
 * keys halve and double it. */
static uint16 s_gameSpeedFactor = 1;
#define GAME_SPEED_FACTOR_MAX 8

static bool s_ecoSearch = false;
static bool s_ecoBaseline = false;
static bool s_ecoGrid = false;
static bool s_ecoQueue = false;
static bool s_ecoCarryall = false;
static bool s_ecoPlay = false;
static uint16 s_ecoPlayRefineries = 1;
static uint16 s_ecoPlayHarvesters = 16;
static uint16 s_ecoPlayCarryalls = 0;
static uint16 s_ecoPlayWait = 5;
static uint16 s_ecoPlayCarryallWait = 1200;
static uint32 s_ecoPlaySeed = 1000;
static uint16 s_ecoPopulation = 12;
static uint16 s_ecoGenerations = 8;
static uint32 s_ecoTicks = 40000;
static uint16 s_ecoMaps = 2;

static bool s_warMatrix = false;
static bool s_warTiming = false;
static bool s_warLadder = false;
static bool s_warMetrics = false;
static bool s_warPlay = false;
static bool s_warTelemetry = false;
static uint16 s_warTelemetryStep = 5000;
static uint32 s_warSwitchTick = 30000;
static uint32 s_warTicks = 150000;
static uint16 s_warMaps = 2;
static uint16 s_warPlayShare[SKIRMISH_PLAYER_MAX] = { 45, 45 };
static uint32 s_warPlaySeed = 1000;

static bool s_skirmishSelfTest = false;
static bool s_skirmishDirect = false;
static uint32 s_skirmishSelfTestTicks = 30000;

static bool s_mpChecksum = false;
static uint32 s_mpChecksumTicks = 60000;
static uint32 s_mpChecksumStep = 10000;
static uint32 s_mpChecksumSeed = 1000;

#define MP_REPLAY_SAMPLES_MAX 256

static bool s_mpReplay = false;
static uint32 s_mpReplayTicks = 40000;
static uint32 s_mpReplayStep = 4000;
static uint32 s_mpReplaySeed = 1000;
static char s_mpRecordFile[256] = "";
static char s_mpPlayFile[256] = "";
static uint16 s_mpReplayUnplayed = 0;
static bool s_mpViewpoint = false;
static bool s_mpModal = false;
static uint32 s_mpModalEvery = 5000;
static uint32 s_mpModalHold = 1200;
static uint32 s_mpModalDumpTick = 0;
static bool s_mpTurnLoop = false;
static uint8 s_mpTurnSlot = 0;
static uint16 s_mpTurnLength = MP_TURN_LENGTH_DEFAULT;
static uint8 s_mpTurnDelay = MP_TURN_DELAY_DEFAULT;
static char s_mpNetDirectory[256] = "";
static uint32 s_mpNetWaitMs = 30000;
static uint32 s_mpNetLagMs = 0;
static bool s_mpRealtime = false;
static char s_mpRelayHost[128] = "";
static uint16 s_mpRelayPort = 31337;
static char s_mpRelayRoom[64] = "opendune";
static uint32 s_mpLiveSeed = 1000;
static uint32 s_mpLiveStart = 0;
static uint32 s_mpLiveSteps = 0;
static uint32 s_mpLiveStalledMs = 0;
static uint32 s_mpLiveNextSample = 0;
static uint32 s_mpLiveSampleStep = 300;
/* Off by default until the squad stops desyncing: units created inside a live
 * match start their bytecode in different places on the two clients, and a
 * match that breaks at tick 200 is worse than one with nothing to click. */
static uint16 s_mpLiveUnits = 0;
static uint8 s_mpViewpointOverride = 0;
static bool s_mpLiveDesyncSeen = false;
static uint32 s_mpLiveDumpTick = 0xFFFFFFFF;   /*!< Zero means the starting position. */
static bool s_mpDesyncDump = false;            /*!< Keep a rolling dump of recent turns. */

static void PrintToConsole(const char *str);

uint16 g_validateStrictIfZero = 0; /*!< 0 = strict validation, basically: no-cheat-mode. */
bool g_running = true; /*!< true if game needs to keep running; false to stop the game. */
uint16 g_selectionType = 0;
uint16 g_selectionTypeNew = 0;
bool g_viewport_forceRedraw = false; /*!< Force a full redraw of the screen. */
bool g_viewport_fadein = false; /*!< Fade in the screen. */

int16 g_musicInBattle = 0; /*!< 0 = no battle, 1 = fight is going on, -1 = music of fight is going on is active. */

/**
 * Check if a level is finished, based on the values in WinFlags.
 *
 * @return True if and only if the level has come to an end.
 */
static bool GameLoop_IsLevelFinished(void)
{
	bool finish = false;

	if (s_debugForceWin) return true;

	/* You have to play at least 7200 ticks before you can win the game */
	if (g_timerGame - g_tickScenarioStart < 7200) return false;

	/* Check for structure counts hitting zero */
	if ((g_scenario.winFlags & 0x3) != 0) {
		PoolFindStruct find;
		uint16 countStructureEnemy = 0;
		uint16 countStructureFriendly = 0;

		find.houseID = HOUSE_INVALID;
		find.type    = 0xFFFF;
		find.index   = 0xFFFF;

		/* Calculate how many structures are left on the map */
		while (true) {
			Structure *s;

			s = Structure_Find(&find);
			if (s == NULL) break;

			if (s->o.type == STRUCTURE_SLAB_1x1 || s->o.type == STRUCTURE_SLAB_2x2 || s->o.type == STRUCTURE_WALL) continue;
			if (s->o.type == STRUCTURE_TURRET) continue;
			if (s->o.type == STRUCTURE_ROCKET_TURRET) continue;

			if (s->o.houseID == g_playerHouseID) {
				countStructureFriendly++;
			} else {
				countStructureEnemy++;
			}
		}

		if ((g_scenario.winFlags & 0x1) != 0 && countStructureEnemy == 0) {
			finish = true;
		}
		if ((g_scenario.winFlags & 0x2) != 0 && countStructureFriendly == 0) {
			finish = true;
		}
	}

	/* Check for reaching spice quota */
	if ((g_scenario.winFlags & 0x4) != 0 && g_playerCredits != 0xFFFF) {
		if (g_playerCredits >= g_playerHouse->creditsQuota) {
			finish = true;
		}
	}

	/* Check for reaching timeout */
	if ((g_scenario.winFlags & 0x8) != 0) {
		/* XXX -- This code was with '<' instead of '>=', which makes
		 *  no sense. As it is unused, who knows what the intentions
		 *  were. This at least makes it sensible. */
		if (g_timerGame >= s_tickGameTimeout) {
			finish = true;
		}
	}

	return finish;
}

/**
 * Check if a level is won, based on the values in LoseFlags.
 *
 * @return True if and only if the level has been won by the human.
 */
static bool GameLoop_IsLevelWon(void)
{
	bool win = false;

	if (s_debugForceWin) return true;

	/* Check for structure counts hitting zero */
	if ((g_scenario.loseFlags & 0x3) != 0) {
		PoolFindStruct find;
		uint16 countStructureEnemy = 0;
		uint16 countStructureFriendly = 0;

		find.houseID = HOUSE_INVALID;
		find.type    = 0xFFFF;
		find.index   = 0xFFFF;

		/* Calculate how many structures are left on the map */
		while (true) {
			Structure *s;

			s = Structure_Find(&find);
			if (s == NULL) break;

			if (s->o.type == STRUCTURE_SLAB_1x1 || s->o.type == STRUCTURE_SLAB_2x2 || s->o.type == STRUCTURE_WALL) continue;
			if (s->o.type == STRUCTURE_TURRET) continue;
			if (s->o.type == STRUCTURE_ROCKET_TURRET) continue;

			if (s->o.houseID == g_playerHouseID) {
				countStructureFriendly++;
			} else {
				countStructureEnemy++;
			}
		}

		win = true;
		if ((g_scenario.loseFlags & 0x1) != 0) {
			win = win && (countStructureEnemy == 0);
		}
		if ((g_scenario.loseFlags & 0x2) != 0) {
			win = win && (countStructureFriendly != 0);
		}
	}

	/* Check for reaching spice quota */
	if (!win && (g_scenario.loseFlags & 0x4) != 0 && g_playerCredits != 0xFFFF) {
		win = (g_playerCredits >= g_playerHouse->creditsQuota);
	}

	/* Check for reaching timeout */
	if (!win && (g_scenario.loseFlags & 0x8) != 0) {
		win = (g_timerGame < s_tickGameTimeout);
	}

	return win;
}

void GameLoop_Uninit(void)
{
	while (g_widgetLinkedListHead != NULL) {
		Widget *w = g_widgetLinkedListHead;
		g_widgetLinkedListHead = w->next;

		free(w);
	}

	Script_ClearInfo(g_scriptStructure);
	Script_ClearInfo(g_scriptTeam);

	free(g_readBuffer); g_readBuffer = NULL;

	free(g_palette1); g_palette1 = NULL;
	free(g_palette2); g_palette2 = NULL;
	free(g_paletteMapping1); g_paletteMapping1 = NULL;
	free(g_paletteMapping2); g_paletteMapping2 = NULL;
}

/**
 * Checks if the level comes to an end. If so, it shows all end-level stuff,
 *  and prepares for the next level.
 */
static void GameLoop_LevelEnd(void)
{
	static uint32 levelEndTimer = 0;

	if (levelEndTimer >= g_timerGame && !s_debugForceWin) return;

	if (GameLoop_IsLevelFinished()) {
		Music_Play(0);

		g_cursorSpriteID = 0;

		Sprites_SetMouseSprite(0, 0, g_sprites[0]);

		Sound_Output_Feedback(0xFFFE);

		GUI_ChangeSelectionType(SELECTIONTYPE_MENTAT);

		if (GameLoop_IsLevelWon()) {
			Sound_Output_Feedback(40);

			GUI_DisplayModalMessage(String_Get_ByIndex(STR_YOU_HAVE_SUCCESSFULLY_COMPLETED_YOUR_MISSION), 0xFFFF);

			GUI_Mentat_ShowWin();

			Sprites_UnloadTiles();

			g_campaignID++;

			GUI_EndStats_Show(g_scenario.killedAllied, g_scenario.killedEnemy, g_scenario.destroyedAllied, g_scenario.destroyedEnemy, g_scenario.harvestedAllied, g_scenario.harvestedEnemy, g_scenario.score, g_playerHouseID);

			if (g_campaignID == 9) {
				GUI_Mouse_Hide_Safe();

				GUI_SetPaletteAnimated(g_palette2, 15);
				GUI_ClearScreen(SCREEN_0);
				GameLoop_GameEndAnimation();
				PrepareEnd();
				exit(0);
			}

			GUI_Mouse_Hide_Safe();
			GameLoop_LevelEndAnimation();
			GUI_Mouse_Show_Safe();

			File_ReadBlockFile("IBM.PAL", g_palette1, 256 * 3);

			g_scenarioID = GUI_StrategicMap_Show(g_campaignID, true);

			GUI_SetPaletteAnimated(g_palette2, 15);

			if (g_campaignID == 1 || g_campaignID == 7) {
				if (!GUI_Security_Show()) {
					PrepareEnd();
					exit(0);
				}
			}
		} else {
			Sound_Output_Feedback(41);

			GUI_DisplayModalMessage(String_Get_ByIndex(STR_YOU_HAVE_FAILED_YOUR_MISSION), 0xFFFF);

			GUI_Mentat_ShowLose();

			Sprites_UnloadTiles();

			g_scenarioID = GUI_StrategicMap_Show(g_campaignID, false);
		}

		g_playerHouse->flags.doneFullScaleAttack = false;

		Sprites_LoadTiles();

		g_gameMode = GM_RESTART;
		s_debugForceWin = false;
	}

	levelEndTimer = g_timerGame + 300;
}

static void GameLoop_DrawMenu(const char **strings)
{
	WidgetProperties *props;
	uint16 left;
	uint16 top;
	uint8 i;

	props = &g_widgetProperties[21];
	top = g_curWidgetYBase + props->yBase;
	left = (g_curWidgetXBase + props->xBase) << 3;

	GUI_Mouse_Hide_Safe();

	for (i = 0; i < props->height; i++) {
		uint16 pos = top + g_fontCurrent->height * i;

		if (i == props->fgColourBlink) {
			GUI_DrawText_Wrapper(strings[i], left, pos, props->fgColourSelected, 0, 0x22);
		} else {
			GUI_DrawText_Wrapper(strings[i], left, pos, props->fgColourNormal, 0, 0x22);
		}
	}

	GUI_Mouse_Show_Safe();

	Input_History_Clear();
}

static void GameLoop_DrawText2(const char *string, uint16 left, uint16 top, uint8 fgColourNormal, uint8 fgColourSelected, uint8 bgColour)
{
	uint8 i;

	for (i = 0; i < 3; i++) {
		GUI_Mouse_Hide_Safe();

		GUI_DrawText_Wrapper(string, left, top, fgColourSelected, bgColour, 0x22);
		Timer_Sleep(2);

		GUI_DrawText_Wrapper(string, left, top, fgColourNormal, bgColour, 0x22);
		GUI_Mouse_Show_Safe();
		Timer_Sleep(2);
	}
}

static bool GameLoop_IsInRange(uint16 x, uint16 y, uint16 minX, uint16 minY, uint16 maxX, uint16 maxY)
{
	return x >= minX && x <= maxX && y >= minY && y <= maxY;
}

static uint16 GameLoop_HandleEvents(const char **strings)
{
	uint8 last;
	uint16 result;
	uint16 key;
	uint16 top;
	uint16 left;
	uint16 minX;
	uint16 maxX;
	uint16 minY;
	uint16 maxY;
	uint16 lineHeight;
	uint8 fgColourNormal;
	uint8 fgColourSelected;
	uint8 old;
	WidgetProperties *props;
	uint8 current;

	props = &g_widgetProperties[21];

	last = props->height - 1;
	old = props->fgColourBlink % (last + 1);
	current = old;

	result = 0xFFFF;

	top = g_curWidgetYBase + props->yBase;
	left = (g_curWidgetXBase + props->xBase) << 3;

	lineHeight = g_fontCurrent->height;

	minX = (g_curWidgetXBase << 3) + (g_fontCurrent->maxWidth * props->xBase);
	minY = g_curWidgetYBase + props->yBase;
	maxX = minX + (g_fontCurrent->maxWidth * props->width) - 1;
	maxY = minY + (props->height * lineHeight) - 1;

	fgColourNormal = props->fgColourNormal;
	fgColourSelected = props->fgColourSelected;

	key = 0;
	if (Input_IsInputAvailable() != 0) {
		key = Input_Wait() & 0x8FF;
	}

	if (g_mouseDisabled == 0) {
		uint16 y = g_mouseY;

		if (GameLoop_IsInRange(g_mouseX, y, minX, minY, maxX, maxY)) {
			current = (y - minY) / lineHeight;
		}
	}

	switch (key) {
		case 0x60: /* NUMPAD 8 / ARROW UP */
			if (current-- == 0) current = last;
			break;

		case 0x62: /* NUMPAD 2 / ARROW DOWN */
			if (current++ == last) current = 0;
			break;

		case 0x5B: /* NUMPAD 7 / HOME */
		case 0x65: /* NUMPAD 9 / PAGE UP */
			current = 0;
			break;

		case 0x5D: /* NUMPAD 1 / END */
		case 0x67: /* NUMPAD 3 / PAGE DOWN */
			current = last;
			break;

		case 0x41: /* MOUSE LEFT BUTTON */
		case 0x42: /* MOUSE RIGHT BUTTON */
			if (GameLoop_IsInRange(g_mouseClickX, g_mouseClickY, minX, minY, maxX, maxY)) {
				current = (g_mouseClickY - minY) / lineHeight;
				result = current;
			}
			break;

		case 0x2B: /* NUMPAD 5 / RETURN */
		case 0x3D: /* SPACE */
		case 0x61:
			result = current;
			break;

		default: {
			uint8 i;

			for (i = 0; i < props->height; i++) {
				char c1;
				char c2;

				if (strings[i] == NULL) continue;
				c1 = toupper(*strings[i]);
				c2 = toupper(Input_Keyboard_HandleKeys(key & 0xFF));

				if (c1 == c2) {
					result = i;
					current = i;
					break;
				}
			}
		} break;
	}

	if (current != old) {
		GUI_Mouse_Hide_Safe();
		GUI_DrawText_Wrapper(strings[old], left, top + (old * lineHeight), fgColourNormal, 0, 0x22);
		GUI_DrawText_Wrapper(strings[current], left, top + (current * lineHeight), fgColourSelected, 0, 0x22);
		GUI_Mouse_Show_Safe();
	}

	props->fgColourBlink = current;

	if (result == 0xFFFF) return 0xFFFF;

	GUI_Mouse_Hide_Safe();
	GameLoop_DrawText2(strings[result], left, top + (current * lineHeight), fgColourNormal, fgColourSelected, 0);
	GUI_Mouse_Show_Safe();

	return result;
}

static void Window_WidgetClick_Create(void)
{
	WidgetInfo *wi;

	for (wi = g_table_gameWidgetInfo; wi->index >= 0; wi++) {
		Widget *w;

		w = GUI_Widget_Allocate(wi->index, wi->shortcut, wi->offsetX, wi->offsetY, wi->spriteID, wi->stringID);

		if (wi->spriteID < 0) {
			w->width  = wi->width;
			w->height = wi->height;
		}

		w->clickProc = wi->clickProc;
		w->flags.requiresClick = (wi->flags & 0x0001) ? true : false;
		w->flags.notused1 = (wi->flags & 0x0002) ? true : false;
		w->flags.clickAsHover = (wi->flags & 0x0004) ? true : false;
		w->flags.invisible = (wi->flags & 0x0008) ? true : false;
		w->flags.greyWhenInvisible = (wi->flags & 0x0010) ? true : false;
		w->flags.noClickCascade = (wi->flags & 0x0020) ? true : false;
		w->flags.loseSelect = (wi->flags & 0x0040) ? true : false;
		w->flags.notused2 = (wi->flags & 0x0080) ? true : false;
		w->flags.buttonFilterLeft = (wi->flags >> 8) & 0x0f;
		w->flags.buttonFilterRight = (wi->flags >> 12) & 0x0f;

		g_widgetLinkedListHead = GUI_Widget_Insert(g_widgetLinkedListHead, w);
	}
}

static void ReadProfileIni(const char *filename)
{
	char *source;
	char *key;
	char *keys;
	char buffer[120];
	uint16 locsi;

	if (filename == NULL) return;
	if (!File_Exists(filename)) return;

	source = GFX_Screen_Get_ByIndex(SCREEN_1);

	memset(source, 0, 32000);
	File_ReadBlockFile(filename, source, GFX_Screen_GetSize_ByIndex(SCREEN_1));

	keys = source + strlen(source) + 5000;
	*keys = '\0';

	Ini_GetString("construct", NULL, keys, keys, 2000, source);

	for (key = keys; *key != '\0'; key += strlen(key) + 1) {
		ObjectInfo *oi = NULL;
		uint16 count;
		uint8 type;
		uint16 buildCredits;
		uint16 buildTime;
		uint16 fogUncoverRadius;
		uint16 availableCampaign;
		uint16 sortPriority;
		uint16 priorityBuild;
		uint16 priorityTarget;
		uint16 hitpoints;

		type = Unit_StringToType(key);
		if (type != UNIT_INVALID) {
			oi = &g_table_unitInfo[type].o;
		} else {
			type = Structure_StringToType(key);
			if (type != STRUCTURE_INVALID) oi = &g_table_structureInfo[type].o;
		}

		if (oi == NULL) continue;

		Ini_GetString("construct", key, buffer, buffer, 120, source);
		count = sscanf(buffer, "%hu,%hu,%hu,%hu,%hu,%hu,%hu,%hu", &buildCredits, &buildTime, &hitpoints, &fogUncoverRadius, &availableCampaign, &priorityBuild, &priorityTarget, &sortPriority);
		oi->buildCredits      = buildCredits;
		oi->buildTime         = buildTime;
		oi->hitpoints         = hitpoints;
		oi->fogUncoverRadius  = fogUncoverRadius;
		oi->availableCampaign = availableCampaign;
		oi->priorityBuild     = priorityBuild;
		oi->priorityTarget    = priorityTarget;
		if (count <= 7) continue;
		oi->sortPriority = (uint8)sortPriority;
	}

	if (g_debugGame) {
		for (locsi = 0; locsi < UNIT_MAX; locsi++) {
			ObjectInfo *oi = &g_table_unitInfo[locsi].o;

			sprintf(buffer, "%*s%4d,%4d,%4d,%4d,%4d,%4d,%4d,%4d",
				15 - (int)strlen(oi->name), "", oi->buildCredits, oi->buildTime, oi->hitpoints, oi->fogUncoverRadius,
				oi->availableCampaign, oi->priorityBuild, oi->priorityTarget, oi->sortPriority);

			Ini_SetString("construct", oi->name, buffer, source);
		}

		for (locsi = 0; locsi < STRUCTURE_MAX; locsi++) {
			ObjectInfo *oi = &g_table_structureInfo[locsi].o;

			sprintf(buffer, "%*s%4d,%4d,%4d,%4d,%4d,%4d,%4d,%4d",
				15 - (int)strlen(oi->name), "", oi->buildCredits, oi->buildTime, oi->hitpoints, oi->fogUncoverRadius,
				oi->availableCampaign, oi->priorityBuild, oi->priorityTarget, oi->sortPriority);

			Ini_SetString("construct", oi->name, buffer, source);
		}
	}

	*keys = '\0';

	Ini_GetString("combat", NULL, keys, keys, 2000, source);

	for (key = keys; *key != '\0'; key += strlen(key) + 1) {
		uint16 damage;
		uint16 movingSpeedFactor;
		uint16 fireDelay;
		uint16 fireDistance;

		Ini_GetString("combat", key, buffer, buffer, 120, source);
		String_Trim(buffer);
		if (sscanf(buffer, "%hu,%hu,%hu,%hu", &fireDistance, &damage, &fireDelay, &movingSpeedFactor) < 4) continue;

		for (locsi = 0; locsi < UNIT_MAX; locsi++) {
			UnitInfo *ui = &g_table_unitInfo[locsi];

			if (strcasecmp(ui->o.name, key) != 0) continue;

			ui->damage            = damage;
			ui->movingSpeedFactor = movingSpeedFactor;
			ui->fireDelay         = fireDelay;
			ui->fireDistance      = fireDistance;
			break;
		}
	}

	if (!g_debugGame) return;

	for (locsi = 0; locsi < UNIT_MAX; locsi++) {
		const UnitInfo *ui = &g_table_unitInfo[locsi];

		sprintf(buffer, "%*s%4d,%4d,%4d,%4d", 15 - (int)strlen(ui->o.name), "", ui->fireDistance, ui->damage, ui->fireDelay, ui->movingSpeedFactor);
		Ini_SetString("combat", ui->o.name, buffer, source);
	}
}

/** Base the spectator camera last jumped to, cycled by Tab. */
static uint8 s_skirmishCameraBase = 0;

/** Houses the two skirmish AIs play, set from the command line. */
static uint8 s_skirmishHouse[SKIRMISH_PLAYER_MAX] = { HOUSE_ATREIDES, HOUSE_HARKONNEN };

/**
 * Read the House pair of "--skirmish=atreides,harkonnen".  Anything the parser
 * does not like leaves the default pair in place rather than starting a match
 * the caller did not ask for.
 * @param arg The part after the '='.
 */
static void GameLoop_SkirmishParseHouses(const char *arg)
{
	char names[64];
	char *split;
	uint8 first, second;

	strncpy(names, arg, sizeof(names) - 1);
	names[sizeof(names) - 1] = '\0';

	split = strchr(names, ',');
	if (split == NULL) {
		Warning("--skirmish expects two House names, e.g. --skirmish=ordos,harkonnen\n");
		return;
	}
	*split = '\0';

	first  = House_StringToType(names);
	second = House_StringToType(split + 1);

	if (first == HOUSE_INVALID || second == HOUSE_INVALID) {
		Warning("--skirmish: unknown House name in '%s'\n", arg);
		return;
	}

	/* Everything in the engine is keyed on the House index, so the two AIs
	 * cannot be the same House. */
	if (first == second) {
		Warning("--skirmish: both players cannot be the same House\n");
		return;
	}

	s_skirmishHouse[0] = first;
	s_skirmishHouse[1] = second;
}

/**
 * Intro menu.
 */
/**
 * Not a string table entry: the menu wants a row the original game never had.
 *
 * Picked well above every real STR_ id and below 0xFFFF, which the event loop
 * uses for "nothing was clicked".  The row is terminated by STR_NULL like all
 * the others, so the sizing loop still finds the end.
 */
#define STR_LOBBY_MENU 0x7000

/* Defined with the rest of the match plumbing, further down. */
static void MpGame_TakeLobbyChoice(const LobbyChoice *choice);

static void GameLoop_GameIntroAnimationMenu(void)
{
	/* Seven columns, not six.  The list is sized by its first STR_NULL, so a
	 * row that fills every column has no terminator and the whole menu vanishes
	 * -- which is what a sixth entry did to the profile that has both a savegame
	 * and a Hall of Fame. */
	static const uint16 mainMenuStrings[][7] = {
		{STR_PLAY_A_GAME, STR_LOBBY_MENU, STR_REPLAY_INTRODUCTION, STR_EXIT_GAME, STR_NULL,         STR_NULL,         STR_NULL}, /* Neither HOF nor save. */
		{STR_PLAY_A_GAME, STR_LOBBY_MENU, STR_REPLAY_INTRODUCTION, STR_LOAD_GAME, STR_EXIT_GAME,    STR_NULL,         STR_NULL}, /* Has a save game. */
		{STR_PLAY_A_GAME, STR_LOBBY_MENU, STR_REPLAY_INTRODUCTION, STR_EXIT_GAME, STR_HALL_OF_FAME, STR_NULL,         STR_NULL}, /* Has a HOF. */
		{STR_PLAY_A_GAME, STR_LOBBY_MENU, STR_REPLAY_INTRODUCTION, STR_LOAD_GAME, STR_EXIT_GAME,    STR_HALL_OF_FAME, STR_NULL}  /* Has a HOF and a save game. */
	};
	static char lobbyMenuLabel[] = "PLAY SOMEBODY";

	bool loadGame = false;
	static bool drawMenu = true;
	static uint16 stringID = STR_REPLAY_INTRODUCTION;
	uint16 maxWidth;
	static bool hasSave = false;
	static bool hasFame = false;
	static const char *strings[7];
	static uint16 index = 0xFFFF;

	/* Back at the menu means no match is running, whatever the last one left
	 * behind.  Without this, cancelling out of a networked game and then
	 * starting a campaign would still find MpGame_IsLive() true.  The command
	 * line never comes through here -- --mp-relay goes straight to GM_SKIRMISH
	 * -- so it is not cleared out from under it. */
	s_mpRelayHost[0] = '\0';

	/* Nothing to draw for somebody who is leaving. */
	if (!g_running) return;

	/* Arrive as though PLAY SOMEBODY had just been chosen, so the flag exercises
	 * the handoff and not a shortcut past it.  Once only: a second pass would
	 * be the retry loop this flag was written to catch. */
	if (s_lobbyPlay && !s_lobbyPlayEntered) {
		stringID = STR_LOBBY_MENU;
		s_lobbyPlayEntered = true;
	}

	if (index == 0xFFFF) {
		hasSave = File_Exists_Personal("_save000.dat");
		hasFame = File_Exists_Personal("SAVEFAME.DAT");
		index = (hasFame ? 2 : 0) + (hasSave ? 1 : 0);
	}

	if (!g_canSkipIntro) {
		if (hasSave) g_canSkipIntro = true;
	}

	switch (stringID) {
		case STR_REPLAY_INTRODUCTION:
			Music_Play(0);

			free(g_readBuffer);
			g_readBufferSize = (g_enableVoices == 0) ? 12000 : 28000;
			g_readBuffer = calloc(1, g_readBufferSize);

			GUI_Mouse_Hide_Safe();

			Driver_Music_FadeOut();

			GameLoop_GameIntroAnimation();

			Sound_Output_Feedback(0xFFFE);

			File_ReadBlockFile("IBM.PAL", g_palette1, 256 * 3);

			if (!g_canSkipIntro) {
				File_Create_Personal("ONETIME.DAT");
				g_canSkipIntro = true;
			}

			Music_Play(0);

			free(g_readBuffer);
			g_readBufferSize = (g_enableVoices == 0) ? 12000 : 20000;
			g_readBuffer = calloc(1, g_readBufferSize);

			GUI_Mouse_Show_Safe();

			Music_Play(28);

			drawMenu = true;
			break;

		case STR_EXIT_GAME:
			g_running = false;
			return;

		case STR_HALL_OF_FAME:
			GUI_HallOfFame_Show(0xFFFF);

			GFX_SetPalette(g_palette2);

			hasFame = File_Exists_Personal("SAVEFAME.DAT");
			drawMenu = true;
			break;

		case STR_LOBBY_MENU: {
			LobbyChoice choice;
			bool agreed;

			agreed = s_lobbyPlay
			       ? GUI_Lobby_Choose(s_lobbyPlayRelay, s_lobbyPlayCode, s_lobbyPlayPair, s_lobbyPlaySlot, &choice)
			       : GUI_Lobby_Show(&choice);

			if (agreed) {
				MpGame_TakeLobbyChoice(&choice);
				g_gameMode = GM_SKIRMISH;

				/* Coming back here means the match ended or never began, and
				 * this switch runs on whatever was chosen last.  Left as it
				 * was, the lobby reopened itself the instant the match failed
				 * -- over a black screen, because GM_SKIRMISH had already
				 * cleared it -- and did so again thirty seconds later, for
				 * ever.  Land in the menu instead, with the lobby's rows still
				 * filled in for a second try. */
				stringID = STR_NULL;
				drawMenu = true;
				return;
			}

			drawMenu = true;
			break;
		}

		case STR_LOAD_GAME:
			GUI_Mouse_Hide_Safe();
			GUI_SetPaletteAnimated(g_palette2, 30);
			GUI_ClearScreen(SCREEN_0);
			GUI_Mouse_Show_Safe();

			GFX_SetPalette(g_palette1);

			if (GUI_Widget_SaveLoad_Click(false)) {
				loadGame = true;
				if (g_gameMode == GM_RESTART) break;
				g_gameMode = GM_NORMAL;
			} else {
				GFX_SetPalette(g_palette2);

				drawMenu = true;
			}
			break;

		default: break;
	}

	if (drawMenu) {
		char buildLabel[32];
		uint16 i;

		g_widgetProperties[21].height = 0;

		for (i = 0; i < 7; i++) {
			strings[i] = NULL;

			if (mainMenuStrings[index][i] == 0) {
				if (g_widgetProperties[21].height == 0) g_widgetProperties[21].height = i;
				continue;
			}

			strings[i] = (mainMenuStrings[index][i] == STR_LOBBY_MENU)
			           ? lobbyMenuLabel
			           : String_Get_ByIndex(mainMenuStrings[index][i]);
		}

		GUI_DrawText_Wrapper(NULL, 0, 0, 0, 0, 0x22);

		maxWidth = 0;

		for (i = 0; i < g_widgetProperties[21].height; i++) {
			if (Font_GetStringWidth(strings[i]) <= maxWidth) continue;
			maxWidth = Font_GetStringWidth(strings[i]);
		}

		maxWidth += 7;

		g_widgetProperties[21].width  = maxWidth >> 3;
		g_widgetProperties[13].width  = g_widgetProperties[21].width + 2;
		g_widgetProperties[13].xBase  = 19 - (maxWidth >> 4);
		g_widgetProperties[13].yBase  = 160 - ((g_widgetProperties[21].height * g_fontCurrent->height) >> 1);
		g_widgetProperties[13].height = (g_widgetProperties[21].height * g_fontCurrent->height) + 11;

		if (File_Exists("TITLE.CPS")) Sprites_LoadImage("TITLE.CPS", SCREEN_1, NULL);
		else Sprites_LoadImage(String_GenerateFilename("TITLE"), SCREEN_1, NULL);

		GUI_Mouse_Hide_Safe();

		GUI_ClearScreen(SCREEN_0);

		GUI_Screen_Copy(0, 0, 0, 0, SCREEN_WIDTH / 8, SCREEN_HEIGHT, SCREEN_1, SCREEN_0);

		GUI_SetPaletteAnimated(g_palette1, 30);

		/* The revision alone cannot tell two builds of the same dirty tree apart,
		 * which is exactly the case while developing.  rev.c is regenerated on
		 * every build, so its compile time is a reliable build stamp. */
		snprintf(buildLabel, sizeof(buildLabel), "BUILD %s %s", g_opendune_revision, g_opendune_build_date + 12);
		GUI_DrawText_Wrapper(buildLabel, 1, 192, 133, 0, 0x31, 0x39);
		GUI_DrawText_Wrapper("V1.07", 319, 192, 133, 0, 0x231, 0x39);
		GUI_DrawText_Wrapper(NULL, 0, 0, 0, 0, 0x22);

		Widget_SetCurrentWidget(13);

		GUI_Widget_DrawBorder(13, 2, 1);

		GameLoop_DrawMenu(strings);

		GUI_Mouse_Show_Safe();

		drawMenu = false;
	}

	if (loadGame) return;

	stringID = GameLoop_HandleEvents(strings);

	if (stringID != 0xFFFF) stringID = mainMenuStrings[index][stringID];

	GUI_PaletteAnimate();

	if (stringID == STR_PLAY_A_GAME) g_gameMode = GM_PICKHOUSE;
}

/**
 * The speed the simulation is actually running at, as a multiplier of the
 * original game's pace.  This is what the indicator shows.
 */
uint16 GameLoop_GetSpeedFactor(void)
{
	return s_gameSpeedFactor * ((g_gameConfig.gameSpeed == GAME_SPEED_FAST) ? 2 : 1);
}

/** Halve (direction < 0) or double (direction > 0) the game speed. */
static void GameLoop_StepSpeed(int direction)
{
	uint16 old = s_gameSpeedFactor;

	if (direction < 0) {
		s_gameSpeedFactor = max(1, s_gameSpeedFactor / 2);
	} else {
		s_gameSpeedFactor = min(GAME_SPEED_FACTOR_MAX, s_gameSpeedFactor * 2);
	}

	if (s_gameSpeedFactor == old) return;

	/* The indicator sits over the map, which is only repainted where it is
	 * dirty; without this the previous factor stays on screen. */
	g_viewport_forceRedraw = true;
}

static void InGame_Numpad_Move(uint16 key)
{
	if (key == 0) return;

	switch (key) {
		case 0x0010: /* TAB */
			Map_SelectNext(true);
			return;

		case 0x0110: /* SHIFT TAB */
			Map_SelectNext(false);
			return;

		case 0x005C: /* NUMPAD 4 / ARROW LEFT */
		case 0x045C:
		case 0x055C:
			Map_MoveDirection(6);
			return;

		case 0x0066: /* NUMPAD 6 / ARROW RIGHT */
		case 0x0466:
		case 0x0566:
			Map_MoveDirection(2);
			return;

		case 0x0060: /* NUMPAD 8 / ARROW UP */
		case 0x0460:
		case 0x0560:
			Map_MoveDirection(0);
			return;

		case 0x0062: /* NUMPAD 2 / ARROW DOWN */
		case 0x0462:
		case 0x0562:
			Map_MoveDirection(4);
			return;

		case 0x005B: /* NUMPAD 7 / HOME */
		case 0x045B:
		case 0x055B:
			Map_MoveDirection(7);
			return;

		case 0x005D: /* NUMPAD 1 / END */
		case 0x045D:
		case 0x055D:
			Map_MoveDirection(5);
			return;

		case 0x0065: /* NUMPAD 9 / PAGE UP */
		case 0x0465:
		case 0x0565:
			Map_MoveDirection(1);
			return;

		case 0x0067: /* NUMPAD 3 / PAGE DOWN */
		case 0x0467:
		case 0x0567:
			Map_MoveDirection(3);
			return;

		default: return;
	}
}

/* Keyboard commands must not depend on which variant of the action panel is
 * visible.  The group panel deliberately has no widget shortcuts, while the
 * legacy single-unit panel owns them; routing here gives both the same target
 * transaction and therefore the same persistent selection semantics. */
static bool InGame_BeginSelectedAction(ActionType action)
{
	if (g_selectionType != SELECTIONTYPE_UNIT || g_unitSelectionCount == 0) return false;
	if (!UnitSelection_BeginAction(action)) return false;

	g_unitActive = g_unitSelected;
	g_activeAction = action;
	GUI_ChangeSelectionType(SELECTIONTYPE_TARGET);
	return true;
}

/**
 * Main game loop.
 */
/**
 * Start a match the harness owns: same seed, same clock, every run.
 *
 * The clock is taken before the match is set up, not after.  Skirmish_Start*()
 * ends by taking g_tickScenarioStart from g_timerGame and the INFO chunk stores
 * the difference, so a 60 Hz tick landing in between is one stray tick of
 * elapsed time and a different checksum -- about one run in four.  This is the
 * network stepper of mp.md in embryo: the simulation clock is a function of
 * steps taken, not of how long the machine took to take them.
 */
static bool MpHarness_StartMatch(uint32 seed)
{
	SkirmishEconomyPlan planA, planB;

	WarSearch_MakePlan(s_warPlayShare[0], s_warPlayShare[0], 0, &planA);
	WarSearch_MakePlan(s_warPlayShare[1], s_warPlayShare[1], 0, &planB);

	Timer_SetTimer(TIMER_GAME, false);
	Timer_SetTimer(TIMER_GUI, false);
	Timer_ResetGame();
	Timer_ResetGUI();

	/* The animation clock stops following the wall here rather than at
	 * MpTurn_Begin(), which is several hundred milliseconds of setup later.
	 * Those milliseconds are how long *this* machine took, so leaving them in
	 * gave the two clients different clocks before the first turn -- see the
	 * note on s_animClockClaimed in timer.c. */
	Timer_ClaimAnimClock(true);

	return Skirmish_StartWar(s_skirmishHouse[0], s_skirmishHouse[1], seed, &planA, &planB);
}

/**
 * One simulation step.
 *
 * Explosions and animations are not part of the game loop at all: GUI_DrawScreen()
 * ticks them, at frame rate, off g_timerGUI.  They write craters and ground tiles
 * into g_map, which is saved state, so leaving them out would leave the checksum
 * blind to a whole class of divergence -- and leaving them on the render clock is
 * a desync by construction, since two clients do not draw at the same rate.
 * Stepped here on the game clock, which is where mp.md has to put them for real.
 */
static void MpHarness_Step(void)
{
	Timer_StepGame();
	Timer_StepGUI();

	GameLoop_Team();
	GameLoop_Unit();
	GameLoop_Structure();
	GameLoop_House();

	Explosion_Tick();
	Animation_Tick();

	/* On the game clock for the same reason as the two above: it reorders the
	 * array GameLoop_Unit() walks, and per frame that order would depend on the
	 * frame rate. */
	Unit_SortOrder();
}

/**
 * Whether this process is playing a real, drawn match over the relay.
 *
 * The turn loop harness and the game are two different callers of the same
 * loop: the harness owns its clock and draws nothing, the game draws every
 * frame and until now let a 60 Hz ticker own its clock.  Naming the relay
 * without naming the harness asks for the second one.
 */
static bool MpGame_IsLive(void)
{
	return (s_mpRelayHost[0] != '\0' && !s_mpTurnLoop);
}

/**
 * Take what the lobby settled and put the match together from it.
 *
 * The same four statics --mp-relay fills in from the command line, so from here
 * on the two roads are one road: GM_SKIRMISH reaches MpGame_Begin() either way.
 * Both slots are people -- a lobby match is 1-v-1 by construction, and a house
 * left on the AI would be an opponent neither player agreed to.
 */
static void MpGame_TakeLobbyChoice(const LobbyChoice *choice)
{
	snprintf(s_mpRelayHost, sizeof(s_mpRelayHost), "%s", choice->relayHost);
	snprintf(s_mpRelayRoom, sizeof(s_mpRelayRoom), "%s", choice->room);
	s_mpRelayPort = choice->relayPort;
	s_mpTurnSlot  = choice->slot;
	s_mpLiveSeed  = choice->seed;
	s_mpTurnLoop  = false;

	s_skirmishHouse[0] = choice->house[0];
	s_skirmishHouse[1] = choice->house[1];

	Skirmish_SetController(0, MATCH_CONTROLLER_HUMAN_LOCAL);
	Skirmish_SetController(1, MATCH_CONTROLLER_HUMAN_LOCAL);
}

/**
 * Simulate up to now, in turns.
 *
 * The game loop used to run one simulation step per drawn frame while a wall
 * clock ticker advanced g_timerGame underneath it -- two clocks, neither
 * driving the other, which is fine when there is nobody to agree with and
 * impossible when there is.  Here the wall clock decides how many ticks are
 * *due*, the turn loop decides how many may actually run, and drawing is what
 * happens with the time left over.
 *
 * A stall returns immediately rather than spinning: the frame still draws, the
 * mouse still moves, and the match resumes on the tick the packet lands. That
 * is what a stall has to look like to a player -- a freeze of the world, not of
 * the program.
 */
static void MpGame_Step(void)
{
	/* Never inside itself.  A simulation step is allowed to sleep -- the
	 * original's screen shake does, in the middle of an explosion -- and every
	 * sleep goes through sleepIdle(), which is where the match pump lives.  So
	 * a step could re-enter the stepper and run further ticks from inside the
	 * one it had not finished, as many as the sleep happened to last.  That is
	 * the wall clock deciding how many ticks pass inside a tick, which is two
	 * different answers on two machines: the explosions on the two clients
	 * drifted a tick apart and never came back.
	 *
	 * The guard used to live in the pump, which only stopped the pump from
	 * re-entering itself -- not the frame loop's own call from being re-entered
	 * through a sleep, which is the case that happens. */
	static bool inside = false;

	char line[256];
	uint32 due;
	uint16 budget;

	if (inside) return;
	inside = true;

	/* Everything since the last step was somebody else drawing, handling input
	 * or waiting, and none of that may have moved the world.  The two finer
	 * clamps name a suspect; this one has no blind spot. */
	MpPurity_End(0, "the frame outside the simulation", g_timerGame);

	/* Sixty ticks a second, measured against the wall rather than against how
	 * fast this machine draws.  The other client is pacing itself the same way,
	 * and lockstep only works if both agree what a second holds. */
	/* The speed keys multiply the tick rate rather than the frame rate, and in
	 * a match that stays safe: both clients still run the same ticks in the same
	 * turns, so a client asking for x2 simply reaches each turn boundary sooner
	 * and waits there.  The slower player sets the pace, which is the only
	 * answer that does not let one of them decide how fast the world runs. */
	due = ((Timer_GetTime() - s_mpLiveStart) * 60 * GameLoop_GetSpeedFactor()) / 1000;

	/* Bounded, so a client that fell behind catches up over several frames
	 * instead of disappearing into one long one. */
	for (budget = 0; budget < 16 && s_mpLiveSteps < due; budget++) {
		if (MpTurn_IsDue() && !MpTurn_Advance()) {
			uint32 stallStart = Timer_GetTime();
			uint8 goneSlot = 0;

			/* The other player leaving ends the match rather than pausing it
			 * for ever.  The turn loop stops; the simulation carries on locally
			 * so the window stays alive and can be looked at and closed. */
			if (MpNet_HasLeft(&goneSlot) || !MpNet_IsConnected()) {
				MpPurity_Begin(0);
				snprintf(line, sizeof(line), "mp-live: the match ended at turn %u (%s)",
				         (unsigned)MpTurn_GetTurn(),
				         MpNet_IsConnected() ? "the other player left" : MpNet_GetError());
				PrintToConsole(line);
				MpTurn_End();
				Timer_ClaimAnimClock(false);
				MpNet_Disconnect();
				inside = false;
				return;
			}

			s_mpLiveStalledMs += Timer_GetTime() - stallStart;

			/* Time the world stood still is time the schedule must forget, or
			 * the next frame would sprint through sixteen ticks to catch up and
			 * the stall would become a fast-forward. */
			s_mpLiveStart += Timer_GetTime() - stallStart;
			MpPurity_Begin(0);
			inside = false;
			return;
		}

		Timer_StepGame();
		Timer_StepGUI();

		GameLoop_Team();
		GameLoop_Unit();
		GameLoop_Structure();
		GameLoop_House();

		/* On the game clock, not the render clock: craters and animations are
		 * map state, and two clients do not draw at the same rate.  GUI_DrawScreen()
		 * leaves them alone while a match is on. */
		Explosion_Tick();
		Animation_Tick();
		Unit_SortOrder();

		s_mpLiveSteps++;

		if (s_mpLiveSteps == s_mpLiveDumpTick) {
			char path[64];

			snprintf(path, sizeof(path), "mpdump-s%u-t%u.bin",
			         (unsigned)(s_mpTurnSlot + 1), (unsigned)s_mpLiveSteps);
			MpSync_Dump(path);
		}

		if (s_mpLiveSteps >= s_mpLiveNextSample) {
			MpSyncChecksum sample;

			/* Printed as well as exchanged.  The turn packets already carry a
			 * checksum and the loop compares them, so a desync is caught without
			 * this -- but a log the two players can diff afterwards says *what*
			 * diverged, and the packets say only that something did. */
			if (MpSync_Take(&sample)) {
				MpSync_Format(line, sizeof(line), s_mpLiveSteps, &sample);
				PrintToConsole(line);
			}

			/* The two generators separately, because which one drifted says
			 * where to look: the LCG is the one the presentation used to share. */
			snprintf(line, sizeof(line), "mp-live: tick %u turn %u, %u ms stalled so far, rng %08x lcg %08x",
			         (unsigned)s_mpLiveSteps, (unsigned)MpTurn_GetTurn(), (unsigned)s_mpLiveStalledMs,
			         (unsigned)Tools_Random_GetSeed(), (unsigned)Tools_RandomLCG_GetSeed());
			PrintToConsole(line);

			s_mpLiveNextSample += s_mpLiveSampleStep;
		}
	}

	{
		uint32 desyncTurn = 0;

		MpPurity_Begin(0);

		if (MpTurn_HasDesynced(&desyncTurn) && !s_mpLiveDesyncSeen) {
			snprintf(line, sizeof(line), "mp-live: DESYNC at turn %u (tick %u), about: %s",
			         (unsigned)desyncTurn, (unsigned)(desyncTurn * MP_TURN_LENGTH_DEFAULT),
			         MpTurn_GetDesyncChunks());
			PrintToConsole(line);

			if (s_mpDesyncDump) {
				snprintf(line, sizeof(line), "mp-live: both players' turn %u is in mpdesync-s*-turn%u.bin",
				         (unsigned)desyncTurn, (unsigned)desyncTurn);
				PrintToConsole(line);
			}

			/* Said once, then played on.  Stopping here would be the honest
			 * thing in front of a player and useless in front of a log: the
			 * samples after a divergence are what name the chunk that caused
			 * it, and a match that halts stops producing them. */
			s_mpLiveDesyncSeen = true;
		}
	}

	inside = false;
}

/**
 * Whether the two players still agree, in words, for the corner of the screen.
 *
 * A desync used to announce itself by the units behaving differently, which is
 * both late and ambiguous -- the player cannot tell a divergence from an
 * opponent playing badly.  Every turn already carries a checksum per chunk and
 * every client already compares them, so the answer exists; it only needed
 * somewhere to be shown.  Returns NULL outside a match.
 */
const char *MpGame_GetSyncLine(uint8 *colour)
{
	static char line[64];
	uint32 turn = 0;

	if (!MpTurn_IsActive()) return NULL;

	if (MpTurn_HasDesynced(&turn)) {
		/* Red, and it stays: from here the two games are different games, and
		 * nothing that happens afterwards puts that right. */
		if (colour != NULL) *colour = 8;
		snprintf(line, sizeof(line), "DESYNC t%u: %s", (unsigned)turn, MpTurn_GetDesyncChunks());
		return line;
	}

	if (colour != NULL) *colour = 4;
	snprintf(line, sizeof(line), "sync %u", (unsigned)MpTurn_GetTurn());
	return line;
}

/**
 * Put a starting squad on the map for both players.
 *
 * A match that opens with one construction yard each is a fine game and a poor
 * test: nothing to select, nothing to order, and the first interesting question
 * -- does a move order cross the wire -- is ten minutes of building away.
 *
 * Run identically on both clients, for both houses, in slot order, before the
 * turn loop starts: it is part of the agreed starting position, not something
 * either player did.
 */
static void MpGame_PlaceStartingUnits(uint16 count)
{
	static const uint16 s_squad[] = { UNIT_TRIKE, UNIT_SOLDIER, UNIT_QUAD, UNIT_TROOPER };
	uint8 slot;

	if (count == 0) return;

	for (slot = 0; slot < MATCH_SLOT_MAX; slot++) {
		uint16 origin = Skirmish_GetBaseOrigin(slot);
		uint8 houseID = Match_GetSlotHouse(slot);
		uint16 made = 0;
		uint16 ring;

		if (origin == 0xFFFF || houseID == HOUSE_INVALID) continue;

		/* Outwards from the yard until the squad fits.  The base occupies the
		 * middle, so the first ring is mostly taken. */
		for (ring = 2; ring < 8 && made < count; ring++) {
			int16 dx;

			for (dx = (int16)-ring; dx <= (int16)ring && made < count; dx++) {
				int16 dy;

				for (dy = (int16)-ring; dy <= (int16)ring && made < count; dy++) {
					uint16 packed;
					int16 x, y;

					/* The ring, not the disc: the inside was tried already. */
					if (abs(dx) != (int16)ring && abs(dy) != (int16)ring) continue;

					x = (int16)Tile_GetPackedX(origin) + dx;
					y = (int16)Tile_GetPackedY(origin) + dy;
					if (x < 1 || y < 1 || x > 62 || y > 62) continue;

					packed = Tile_PackXY((uint16)x, (uint16)y);

					if (Structure_Get_ByPackedTile(packed) != NULL) continue;
					if (Unit_Get_ByPackedTile(packed) != NULL) continue;
					if (g_map[packed].groundTileID == g_bloomTileID) continue;
					if (Map_GetLandscapeType(packed) != LST_NORMAL_SAND &&
					    Map_GetLandscapeType(packed) != LST_ENTIRELY_ROCK &&
					    Map_GetLandscapeType(packed) != LST_CONCRETE_SLAB) continue;

					{
						Unit *u = Unit_Create(UNIT_INDEX_INVALID, (uint8)s_squad[made % lengthof(s_squad)],
						                      houseID, Tile_UnpackTile(packed), 0);

						if (u == NULL) continue;

						/* Say what they are doing rather than letting Unit_Create
						 * decide: it picks the player's default action or the AI's
						 * depending on who owns the unit, which is one bytecode
						 * entry point or another -- and a squad that starts its
						 * script in two different places is a desync at tick one. */
						Unit_SetAction(u, ACTION_GUARD);
					}

					made++;
				}
			}
		}
	}
}

/**
 * Step the match from inside somebody else's loop.
 *
 * Registered with the timer's idle hook, so every modal screen in the game --
 * the build list, the mentat, a briefing -- keeps the world turning for the
 * other player while this one reads.  Reentrant by refusal: the simulation
 * itself reaches sleepIdle() in places, and stepping a turn from inside a turn
 * would be a different game.
 */
static void MpGame_Pump(void)
{
	if (!MpTurn_IsActive()) return;

	MpGame_Step();

	/* The world just moved inside whatever region the clamp is measuring, and
	 * moved legitimately.  Forgive that much and keep watching. */
	MpPurity_Rebase(1);
}

/**
 * Put a whole-screen notice up, for the moments when there is no game yet.
 *
 * Joining a match is the only place in the program where the player waits on
 * somebody else, and it used to be a msleep() loop: no events pumped and no
 * frame drawn, so the window went black, macOS put a beachball over it, and
 * every report of it was "the game hangs".  A wait a person can read is a wait
 * they can be patient about -- and it is the same screen that has to carry the
 * reason when the wait ends badly.
 */
static void MpGame_Notice(const char *title, const char *detail, const char *footer)
{
	GUI_Mouse_Hide_Safe();

	GFX_Screen_SetActive(SCREEN_0);
	GUI_ClearScreen(SCREEN_0);

	GUI_DrawText_Wrapper(NULL, 0, 0, 0, 0, 0x22);
	GUI_DrawText_Wrapper(title, SCREEN_WIDTH / 2, 80, 15, 0, 0x122);
	if (detail != NULL) GUI_DrawText_Wrapper(detail, SCREEN_WIDTH / 2, 100, 6, 0, 0x122);
	if (footer != NULL) GUI_DrawText_Wrapper(footer, SCREEN_WIDTH / 2, 130, 15, 0, 0x122);

	GUI_Mouse_Show_Safe();
}

/**
 * Say what went wrong, and hold it on the screen long enough to be read.
 *
 * Everything that can stop a match beginning happens before there is a world to
 * draw, so PrintToConsole() is the only place these ever went -- which is to
 * say nowhere, for anybody who started the game by double-clicking it.
 */
static void MpGame_Failed(const char *reason)
{
	uint32 until;

	MpGame_Notice("CANNOT START THE MATCH", reason, "PRESS ANY KEY");

	until = Timer_GetTime() + 6000;
	Input_History_Clear();

	while (Timer_GetTime() < until) {
		if (Input_Keyboard_NextKey() != 0) break;
		sleepIdle();
	}

	Input_History_Clear();
}

/**
 * Join the room and start the match everybody agreed on.
 *
 * Both clients build the same map from the same seed and hand both houses to
 * the AI, so what the two windows show is one match seen from two chairs --
 * which is exactly the claim stage 3c made and the first chance to watch it
 * being true.
 */
static bool MpGame_Begin(void)
{
	char line[256];
	char waiting[128];
	uint32 until;

	MpGame_Notice("CONNECTING TO THE RELAY", s_mpRelayHost, NULL);

	if (!MpNet_Connect(s_mpRelayHost, s_mpRelayPort, s_mpRelayRoom, s_mpTurnSlot)) {
		snprintf(line, sizeof(line), "mp-live: FAIL (%s)", MpNet_GetError());
		PrintToConsole(line);
		MpGame_Failed(MpNet_GetError());
		return false;
	}

	snprintf(line, sizeof(line), "mp-live: room %s, slot %u, seed %u -- waiting for the other player",
	         s_mpRelayRoom, (unsigned)(s_mpTurnSlot + 1), (unsigned)s_mpLiveSeed);
	PrintToConsole(line);

	snprintf(waiting, sizeof(waiting), "YOU ARE PLAYER %u -- THEY MUST BE THE OTHER ONE",
	         (unsigned)(s_mpTurnSlot + 1));
	MpGame_Notice("WAITING FOR THE OTHER PLAYER", waiting, "PRESS ESC TO GO BACK");
	Input_History_Clear();

	until = Timer_GetTime() + s_mpNetWaitMs;
	while (!MpNet_IsReady()) {
		MpNet_Pump();

		if (!MpNet_IsConnected() || Timer_GetTime() > until) {
			const char *reason = MpNet_IsConnected()
			                   ? "THE OTHER PLAYER NEVER JOINED"
			                   : MpNet_GetError();

			snprintf(line, sizeof(line), "mp-live: FAIL (%s)",
			         MpNet_IsConnected() ? "the other player never joined" : MpNet_GetError());
			PrintToConsole(line);
			MpNet_Disconnect();
			MpGame_Failed(reason);
			return false;
		}

		/* 0x1B is ESC.  Somebody who mistyped the code should not have to sit
		 * out the whole timeout to find out. */
		if (Input_Keyboard_NextKey() == 0x1B) {
			PrintToConsole("mp-live: cancelled at the wait");
			MpNet_Disconnect();
			return false;
		}

		/* Not msleep(): this is what pumps SDL's events and draws the frame, so
		 * the window stays a window rather than becoming a beachball. */
		sleepIdle();
	}

	/* Before the match, not after: the viewpoint has to name a house, and the
	 * houses are allocated during the start.  Set afterwards it does nothing at
	 * all -- both clients then watch through slot 1's eyes, which looks like a
	 * working match right up until you notice both windows are showing the same
	 * corner of the map and neither can give an order to the house it thinks it
	 * is playing. */
	/* --viewpoint=N overrides the slot, which is only ever useful for one thing:
	 * running two clients of one match with the *same* chair.  If a divergence
	 * survives that, it is not a viewpoint leak and the hunt should look
	 * elsewhere. */
	Skirmish_SetViewpoint((s_mpViewpointOverride != 0) ? (uint8)(s_mpViewpointOverride - 1) : s_mpTurnSlot);

	if (!MpHarness_StartMatch(s_mpLiveSeed)) {
		PrintToConsole("mp-live: FAIL (could not start a skirmish)");
		MpNet_Disconnect();
		return false;
	}

	/* Open on our own base rather than on slot 1's.  The camera is presentation,
	 * so this is free -- and without it the first thing a player sees is the
	 * other side's construction yard. */
	{
		uint16 origin = Skirmish_GetBaseOrigin(s_mpTurnSlot);

		if (origin != 0xFFFF) {
			Map_SetViewportPosition(origin);
			g_minimapPosition = g_viewportPosition;
			s_skirmishCameraBase = s_mpTurnSlot;
		}
	}

	s_mpLiveStart      = Timer_GetTime();
	s_mpLiveSteps      = 0;
	s_mpLiveStalledMs  = 0;
	s_mpLiveNextSample = s_mpLiveSampleStep;
	s_mpLiveDesyncSeen = false;

	/* Squad first, then the clock.  Placing units after the turn loop is running
	 * -- and worse, after the pump is registered -- means the world can advance
	 * in the middle of placing them, by however many ticks this machine happened
	 * to spend in Unit_Create.  The two clients then start with the same units
	 * holding script timers two ticks apart, which is a desync before either
	 * player has touched anything. */
	MpGame_PlaceStartingUnits(s_mpLiveUnits);

	/* Tick zero is the starting position itself, before anything has simulated:
	 * the only place to tell "they were created differently" from "they drifted
	 * apart". */
	if (s_mpLiveDumpTick == 0) {
		char path[64];

		snprintf(path, sizeof(path), "mpdump-s%u-t0.bin", (unsigned)(s_mpTurnSlot + 1));
		MpSync_Dump(path);
	}

	MpTurn_Begin(s_mpTurnSlot, MpTransport_Net(), s_mpTurnLength, s_mpTurnDelay);
	MpTurn_SetSnapshots(s_mpDesyncDump);

	Timer_SetMatchPump(&MpGame_Pump);

	snprintf(line, sizeof(line), "mp-live: playing, tl%u d%u, viewpoint slot %u",
	         (unsigned)s_mpTurnLength, (unsigned)s_mpTurnDelay, (unsigned)(s_mpTurnSlot + 1));
	PrintToConsole(line);

	return true;
}

/**
 * Somewhere legal to put a building, found the way a person finds one: by
 * looking at the map.
 *
 * Deliberately not Skirmish_Plan_TakePosition(), which is how the AI answers the
 * same question -- that one marks the plan entry, appends to the build history
 * and lays the slabs, so calling it from the scripted player changed the map and
 * the credits outside the command layer.  The replay caught it at t500 on the
 * first run: the recorded commands were faithful and the two passes still
 * disagreed, because the player had done something no command carried.
 */
static uint16 MpHarness_FindBuildSpot(uint8 houseID, uint16 type)
{
	uint16 x, y, width, height;
	uint16 dx, dy;

	if (!Skirmish_GetBaseRect(houseID, &x, &y, &width, &height)) return 0xFFFF;

	for (dy = 0; dy < height; dy++) {
		for (dx = 0; dx < width; dx++) {
			uint16 packed = Tile_PackXY(x + dx, y + dy);

			if (Structure_IsValidBuildLocation(packed, type, houseID) == 0) continue;

			return packed;
		}
	}

	return 0xFFFF;
}

/**
 * What to build next, in the order a person would.
 *
 * The first version of this took whatever the round-robin landed on, built a
 * House of Ix, a Heavy Vehicle factory and a Barracks, ran out of money at
 * t10000 and stood still for the remaining 30000 ticks with its production on
 * hold.  A test that quiet is barely a test, so the rule below is the minimum
 * economy: refine before anything, keep the lights on, then whatever is left.
 */
static uint16 MpHarness_PickStructure(const House *h, uint32 buildable, uint16 round)
{
	static const uint16 s_opening[] = {
		STRUCTURE_REFINERY, STRUCTURE_WINDTRAP, STRUCTURE_LIGHT_VEHICLE,
		STRUCTURE_HEAVY_VEHICLE, STRUCTURE_BARRACKS, STRUCTURE_OUTPOST
	};
	uint16 i;

	/* Power first once it is short: every structure of a browning-out house
	 * caps at half its hitpoints. */
	if (h->powerProduction < h->powerUsage + 20 && (buildable & (1u << STRUCTURE_WINDTRAP)) != 0) return STRUCTURE_WINDTRAP;

	for (i = 0; i < lengthof(s_opening); i++) {
		if ((h->structuresBuilt & (1u << s_opening[i])) != 0) continue;
		if ((buildable & (1u << s_opening[i])) == 0) continue;

		return s_opening[i];
	}

	/* The opening is done; from here it does not matter much what goes up, only
	 * that something does. */
	for (i = 0; i < STRUCTURE_MAX; i++) {
		uint16 candidate = (uint16)((round + i) % STRUCTURE_MAX);

		if (candidate == STRUCTURE_SLAB_1x1 || candidate == STRUCTURE_SLAB_2x2) continue;
		if ((buildable & (1u << candidate)) == 0) continue;

		return candidate;
	}

	return 0xFFFF;
}

/**
 * A player with no hands.
 *
 * It stands in for the person the command layer exists for: it looks at the
 * board, picks a recipient and submits through the same MpCommand_Submit() the
 * mouse does.  Everything it decides is a function of the tick and of simulation
 * state, so the first pass is reproducible -- and the second pass never runs it
 * at all, replaying what it submitted instead.
 *
 * It plays a house the AI does not touch (--human), which is what lets it use
 * the unit orders at all: UnitSelection_IsControllable() refuses a unit no
 * person controls, so against an AI house the whole unit half of the command
 * layer was unreachable and the earlier version of this only ordered production.
 *
 * Playing a house by hand means doing the things the AI does for itself.  It
 * has to place what the yard finishes, and it has to clear the hold the engine
 * puts on production when the money runs out -- both are player actions, and a
 * house nobody performs them for simply stops building.
 */
static void MpHarness_ScriptedPlayer(uint8 houseID, uint32 tick)
{
	MpCommand cmd;
	uint16 selection[8];
	House *h;
	uint16 round;
	uint16 count;
	uint16 i;
	uint16 j;

	if (tick == 0 || (tick % 100) != 0) return;

	h = House_Get_ByIndex(houseID);
	if (h == NULL) return;

	round = (uint16)(tick / 100);

	/* Sweep the pools by index rather than by find order: it is the one walk
	 * that does not depend on when anything was allocated. */
	for (i = 0; i < STRUCTURE_INDEX_MAX_SOFT; i++) {
		Structure *s = Structure_Get_ByIndex(i);
		uint32 buildable;
		uint16 type;

		if (s == NULL || !s->o.flags.s.used || !s->o.flags.s.allocated) continue;
		if (s->o.houseID != houseID) continue;
		if (!g_table_structureInfo[s->o.type].o.flags.factory) continue;

		/* Money came back: let it carry on. */
		if (s->o.flags.s.onHold && !s->o.flags.s.repairing && !s->o.flags.s.upgrading && h->credits != 0) {
			MpCommand_Init(&cmd, MP_CMD_STRUCTURE_HOLD, houseID);
			cmd.object = s->o.index;
			cmd.value  = 0;
			MpCommand_Submit(&cmd);
			continue;
		}

		if (s->o.type == STRUCTURE_CONSTRUCTION_YARD) {
			/* Something finished and is waiting for a spot.  The base plan knows
			 * where it was meant to go -- for a house somebody plays the plan is
			 * advice rather than a queue, and this is the player taking it. */
			if (s->o.linkedID != STRUCTURE_INVALID && s->countDown == 0) {
				Structure *ns = Structure_Get_ByIndex(s->o.linkedID);
				uint16 spot;

				if (ns == NULL) continue;

				spot = MpHarness_FindBuildSpot(houseID, ns->o.type);
				if (spot == 0xFFFF) continue;

				MpCommand_Init(&cmd, MP_CMD_STRUCTURE_PLACE, houseID);
				cmd.object = s->o.index;
				cmd.packed = spot;
				MpCommand_Submit(&cmd);
				continue;
			}

			if (s->countDown != 0) continue;

			buildable = Structure_GetBuildable(s);
			if (buildable == 0) continue;

			type = MpHarness_PickStructure(h, buildable, round);
			if (type == 0xFFFF) continue;

			MpCommand_Init(&cmd, MP_CMD_STRUCTURE_BUILD, houseID);
			cmd.object = s->o.index;
			cmd.value  = type;
			MpCommand_Submit(&cmd);
			continue;
		}

		if (s->countDown != 0 || s->o.linkedID != 0xFF) continue;

		buildable = Structure_GetBuildable(s);
		if (buildable == 0) continue;

		/* Pick one of the things this factory can make, by the round, so the
		 * choice is a function of the tick and of what is on the map. */
		type = 0xFFFF;
		for (j = 0; j < UNIT_MAX; j++) {
			uint16 candidate = (uint16)((round + j) % UNIT_MAX);

			if ((buildable & (1u << candidate)) == 0) continue;
			type = candidate;
			break;
		}
		if (type == 0xFFFF) break;

		MpCommand_Init(&cmd, MP_CMD_STRUCTURE_BUILD, houseID);
		cmd.object = s->o.index;
		cmd.value  = type;
		MpCommand_Submit(&cmd);
		break;
	}

	/* And now the half that could not be reached before: orders to units. */
	count = 0;
	for (i = 0; i < UNIT_INDEX_MAX && count < lengthof(selection); i++) {
		Unit *u = Unit_Get_ByIndex(i);

		if (u == NULL || !u->o.flags.s.used || !u->o.flags.s.allocated) continue;
		if (u->o.houseID != houseID) continue;
		if (u->o.flags.s.isNotOnMap) continue;
		if (g_table_unitInfo[u->o.type].movementType == MOVEMENT_WINGER) continue;
		/* Leave the harvesters to their own script: ordering them about is a
		 * fair thing for a player to do, but it drowns the economy and the
		 * match stops being one. */
		if (u->o.type == UNIT_HARVESTER) continue;

		selection[count++] = u->o.index;
	}

	if (count == 0) return;

	/* Three orders in rotation, so the recording carries more than one shape of
	 * command: go there, attack whatever is there, and stand ground. */
	switch (round % 3) {
		case 0:
			MpCommand_Init(&cmd, MP_CMD_UNIT_DEFAULT_ORDER, houseID);
			cmd.packed = Skirmish_GetBaseRally(Skirmish_GetOpponent(houseID));
			break;

		case 1:
			MpCommand_Init(&cmd, MP_CMD_UNIT_ORDER, houseID);
			cmd.action = ACTION_ATTACK;
			cmd.packed = Skirmish_GetBaseOrigin((houseID == s_skirmishHouse[0]) ? 1 : 0);
			break;

		default:
			MpCommand_Init(&cmd, MP_CMD_UNIT_ACTION, houseID);
			cmd.action = ACTION_AREA_GUARD;
			break;
	}

	if (cmd.type != MP_CMD_UNIT_ACTION && cmd.packed == 0xFFFF) return;

	cmd.count = (uint8)count;
	memcpy(cmd.unit, selection, count * sizeof(selection[0]));
	MpCommand_Submit(&cmd);
}

/**
 * Which parts of the state depend on who is watching.
 *
 * A component that differs here is a piece of the simulation still keyed on
 * g_playerHouseID rather than on the match descriptor: harmless in a campaign,
 * where the viewpoint and the only player are the same house, and a desync on
 * the first tick of a real match, where they are not.  See mp.md.
 */
static void MpHarness_ReportViewpoint(const MpSyncChecksum *a, const MpSyncChecksum *b, uint16 count)
{
	static const char *s_names[] = { "info", "house", "unit", "str", "map", "team", "new", "rng" };
	uint16 differing[8];
	uint32 firstTick[8];
	char line[256];
	uint16 total = 0;
	uint16 i;
	uint8 c;

	memset(differing, 0, sizeof(differing));
	memset(firstTick, 0, sizeof(firstTick));

	for (i = 0; i < count; i++) {
		const uint32 va[8] = { a[i].info, a[i].house, a[i].unit, a[i].structure, a[i].map, a[i].team, a[i].unitNew, a[i].rng };
		const uint32 vb[8] = { b[i].info, b[i].house, b[i].unit, b[i].structure, b[i].map, b[i].team, b[i].unitNew, b[i].rng };

		if (memcmp(&a[i], &b[i], sizeof(a[i])) != 0) total++;

		for (c = 0; c < 8; c++) {
			if (va[c] == vb[c]) continue;
			if (differing[c] == 0) firstTick[c] = (uint32)i * s_mpReplayStep;
			differing[c]++;
		}
	}

	snprintf(line, sizeof(line), "mp-viewpoint: %u of %u samples differ between the two viewpoints", (unsigned)total, (unsigned)count);
	PrintToConsole(line);

	for (c = 0; c < 8; c++) {
		if (differing[c] == 0) continue;

		snprintf(line, sizeof(line), "mp-viewpoint:   %-6s %4u samples, first at t%u",
		         s_names[c], (unsigned)differing[c], (unsigned)firstTick[c]);
		PrintToConsole(line);
	}

	PrintToConsole((total == 0) ? "mp-viewpoint: PASS (the simulation does not know who is watching)"
	                            : "mp-viewpoint: FAIL (the simulation still reads the viewpoint)");
}

/**
 * Stage 3 of mp.md: one pass of the command-layer harness.
 *
 * With the scripted player on, it plays the match and records every order it
 * gives; with it off, it plays the same match and executes whatever is already
 * in the recording instead.  Two passes that agree at every sample mean the
 * command stream is a complete account of what the player did -- the property
 * lockstep rests on, tested without a socket in sight.
 *
 * Printing the samples turns the same pass into half of the cross-process test:
 * two processes, one recording, and diff decides.
 */
static bool MpHarness_ReplayPass(bool scripted, MpSyncChecksum *out, uint16 *outCount, bool print)
{
	char line[256];
	uint16 samples = 0;
	uint16 commands = 0;
	uint16 next = 0;
	uint32 tick;

	if (!MpHarness_StartMatch(s_mpReplaySeed)) return false;

	if (scripted) {
		MpCommand_RecordBegin();
	} else {
		commands = MpCommand_GetRecordCount();
	}

	for (tick = 0; ; tick++) {
		/* Act first, sample second.  A sample taken on the same tick as a
		 * command, before anything else simulates, is what gives this test
		 * teeth: sampled a step later instead, the AI has re-ordered the same
		 * units in the meantime and a dropped command leaves no trace.  That is
		 * not a hypothetical -- it is what the first version of this test did,
		 * and deleting a command from the replay still passed. */
		if (scripted) {
			MpHarness_ScriptedPlayer(s_skirmishHouse[0], tick);
		} else {
			/* The recording is in tick order, so one cursor walks it. */
			while (next < commands) {
				uint32 when;
				const MpCommand *cmd = MpCommand_GetRecorded(next, &when);

				if (cmd == NULL || when != g_timerGame) break;
				MpCommand_Execute(cmd);
				next++;
			}
		}

		if (((tick % s_mpReplayStep) == 0 || tick == s_mpReplayTicks) && samples < MP_REPLAY_SAMPLES_MAX) {
			if (!MpSync_Take(&out[samples])) return false;

			if (print) {
				MpSync_Format(line, sizeof(line), tick, &out[samples]);
				PrintToConsole(line);
			}
			samples++;
		}

		if (tick == s_mpReplayTicks) break;

		MpHarness_Step();
	}

	if (scripted) MpCommand_RecordEnd();

	*outCount = samples;
	s_mpReplayUnplayed = (uint16)(commands - next);
	return true;
}

/**
 * Stage 5 of mp.md: one pass of the modal-screen harness.
 *
 * Opening a fullscreen screen must not change the world.  That is a claim about
 * the simulation, so it is tested against the simulation and not against the
 * screen: the pass plays a match, and with `open` set it performs what a modal
 * screen does to the world -- GUI_ModalScreen_Enter(), some ticks of the player
 * reading, GUI_ModalScreen_Leave(), and the recount the options screen carries
 * -- at a fixed cadence.  The event loops themselves are deliberately not here.
 * They are the part that was already solved: they reach sleepIdle(), the match
 * pump lives there, and the ticks they spend are the ticks this pass spends
 * anyway.
 *
 * Two passes that agree at every sample mean a player may read the manual
 * without ending the match for the other one.
 */
/**
 * Stand in for the turn loop's ownership of the clock.
 *
 * Half of what a modal screen does on its way out ends in
 * Timer_SetTimer(TIMER_GAME, true), and in a match that call is refused because
 * the turn loop owns the clock (timer.c:499).  This pass owns it for the same
 * reason and has to refuse it for itself -- left running, the 60 Hz ticker
 * advances g_timerGame beside MpHarness_Step() and the two passes end up at
 * different clocks, which is a property of how busy the machine was rather than
 * of the screen.  Called after every step of the cycle, not once at the end,
 * because a tick landing inside the gap would be just as invisible and just as
 * wrong.
 */
static void MpHarness_HoldClock(void)
{
	Timer_SetTimer(TIMER_GAME, false);
}

/**
 * One call of the modal cycle, with the purity clamp around it.
 *
 * The pass on its own answers "did anything move", which it can only do at the
 * next sample -- often thousands of ticks after the call that did it.  With
 * --sim-purity the same run also answers "which call, and which chunk", by
 * checksumming the world either side of every one of them.  That is the
 * difference between knowing there is a bug and knowing where it is.
 */
static void MpHarness_ModalCall(void (*fn)(void), const char *what, uint32 tick)
{
	MpPurity_Begin(0);
	fn();
	MpHarness_HoldClock();
	MpPurity_End(0, what, tick);
}

static void MpHarness_SelectMentat(void)    { GUI_ChangeSelectionType(SELECTIONTYPE_MENTAT); }
static void MpHarness_SelectStructure(void) { GUI_ChangeSelectionType(SELECTIONTYPE_STRUCTURE); }

static bool MpHarness_ModalPass(bool open, MpSyncChecksum *out, uint16 *outCount, bool print)
{
	char line[256];
	uint16 samples = 0;
	uint32 tick;
	bool inside = false;

	if (!MpHarness_StartMatch(s_mpReplaySeed)) return false;

	for (tick = 0; ; tick++) {
		if (open && s_mpModalEvery != 0) {
			uint32 phase = tick % s_mpModalEvery;

			/* Never on tick zero: the first sample is taken there, and a
			 * screen opened before it would be testing the setup rather than
			 * the match. */
			if (tick != 0 && phase == 0) {
				MpHarness_ModalCall(&GUI_ModalScreen_Enter, "modal-enter", tick);

				/* The build list is the third modal surface and the one a player
				 * opens most; it reaches the world through the selection type
				 * rather than through the pair above (structure.c:1892). */
				MpHarness_ModalCall(&MpHarness_SelectMentat, "selection-mentat", tick);

				inside = true;
			} else if (inside && phase == s_mpModalHold) {
				MpHarness_ModalCall(&MpHarness_SelectStructure, "selection-structure", tick);
				MpHarness_ModalCall(&GUI_ModalScreen_Leave, "modal-leave", tick);
				MpHarness_ModalCall(&GUI_Options_Recount, "options-recount", tick);
				inside = false;
			}
		}

		if (((tick % s_mpReplayStep) == 0 || tick == s_mpReplayTicks) && samples < MP_REPLAY_SAMPLES_MAX) {
			if (!MpSync_Take(&out[samples])) return false;

			/* Both passes leave their copy of one sample on disk, so that when
			 * they disagree the answer is a diff rather than a chunk name.  Same
			 * pair of files --mp-desync-dump writes, and the same reader:
			 * tools/mpdesync_diff.py. */
			if (s_mpModalDumpTick != 0 && tick == s_mpModalDumpTick) {
				MpSync_Dump(open ? "mpmodal-s2-turn0.bin" : "mpmodal-s1-turn0.bin");
			}

			if (print) {
				MpSync_Format(line, sizeof(line), tick, &out[samples]);
				PrintToConsole(line);
			}
			samples++;
		}

		if (tick == s_mpReplayTicks) break;

		MpHarness_Step();
	}

	if (inside) {
		GUI_ModalScreen_Leave();
		MpHarness_HoldClock();
	}

	*outCount = samples;
	return true;
}

/**
 * What a modal screen changed, named per savegame chunk.
 *
 * Same shape as the viewpoint report, and for the same reason: a chunk name is
 * a place to look, where "the game desynced" is not.
 */
static void MpHarness_ReportModal(const MpSyncChecksum *a, const MpSyncChecksum *b, uint16 count)
{
	static const char *s_names[] = { "info", "house", "unit", "str", "map", "team", "new", "rng" };
	uint16 differing[8];
	uint32 firstTick[8];
	char line[256];
	uint16 total = 0;
	uint16 i;
	uint8 c;

	memset(differing, 0, sizeof(differing));
	memset(firstTick, 0, sizeof(firstTick));

	for (i = 0; i < count; i++) {
		const uint32 va[8] = { a[i].info, a[i].house, a[i].unit, a[i].structure, a[i].map, a[i].team, a[i].unitNew, a[i].rng };
		const uint32 vb[8] = { b[i].info, b[i].house, b[i].unit, b[i].structure, b[i].map, b[i].team, b[i].unitNew, b[i].rng };

		if (memcmp(&a[i], &b[i], sizeof(a[i])) != 0) total++;

		for (c = 0; c < 8; c++) {
			if (va[c] == vb[c]) continue;
			if (differing[c] == 0) firstTick[c] = (uint32)i * s_mpReplayStep;
			differing[c]++;
		}
	}

	snprintf(line, sizeof(line), "mp-modal: %u of %u samples differ once the screens are opened", (unsigned)total, (unsigned)count);
	PrintToConsole(line);

	for (c = 0; c < 8; c++) {
		if (differing[c] == 0) continue;

		snprintf(line, sizeof(line), "mp-modal:   %-6s %4u samples, first at t%u",
		         s_names[c], (unsigned)differing[c], (unsigned)firstTick[c]);
		PrintToConsole(line);
	}

	PrintToConsole((total == 0) ? "mp-modal: PASS (a modal screen changes nothing the match can see)"
	                            : "mp-modal: FAIL (opening a screen moved the simulation)");
}

/**
 * Does concrete go on sand, and is it worth going there?
 *
 * Two claims, and they are separate.  The first is a placement rule and is
 * tested against a real map: a slab must be refusable and pourable on the same
 * tile depending only on the ini key, a building must be refused on bare sand
 * and accepted on the slab that was just poured there, and a wall must stay
 * refused -- a wall on sand offers no foundation and would let a player fence
 * off open desert.  The second is the reason the first is worth having, and it
 * is table data: concrete has to be faster than sand for every movement type,
 * or a road is not a road.
 *
 * @return 1 when everything held, 0 on the first thing that did not.
 */
static int MoveRules_Failed(const char *why, uint16 detail)
{
	char line[192];

	snprintf(line, sizeof(line), "move-rules-self-test: %s (%u)", why, (unsigned)detail);
	PrintToConsole(line);
	Unit_MoveRules_Init();
	return 0;
}

/**
 * When a 45 degree turn is free, and when it is not.
 *
 * Two claims.  The predicate is the rule itself: one octant either way, taken
 * only by a unit that arrived under power, and spent whether or not it is used
 * -- that last part is what stops a unit which stopped here from claiming a
 * free turn when its next order arrives, and it is the easiest thing to get
 * wrong. The second claim is that arriving on a tile is what arms it, which is
 * checked by actually driving a unit one tile.
 *
 * What this cannot show is that the turn is *worth* anything; that is a
 * question about a whole match, and --war-metrics answers it.
 *
 * @return 1 when everything held, 0 on the first thing that did not.
 */
static int MoveRules_SelfTest(void)
{
	Unit *u = NULL;
	uint16 packed;
	uint8 houseID;
	uint16 guard;

	if (!MpHarness_StartMatch(1000)) return 0;

	houseID = Match_GetSlotHouse(0);
	if (houseID == HOUSE_INVALID) return 0;

	/* Somewhere flat with room to drive east.  Rock rather than sand only
	 * because a Trike is quicker over it and the loop below is bounded. */
	for (packed = 0; packed + 2 < 64 * 64; packed++) {
		if (!Map_IsValidPosition(packed) || !Map_IsValidPosition((uint16)(packed + 1))) continue;
		if (Map_GetLandscapeType(packed) != LST_ENTIRELY_ROCK) continue;
		if (Map_GetLandscapeType((uint16)(packed + 1)) != LST_ENTIRELY_ROCK) continue;
		if (Object_GetByPackedTile(packed) != NULL) continue;
		if (Object_GetByPackedTile((uint16)(packed + 1)) != NULL) continue;

		u = Unit_Create(UNIT_INDEX_INVALID, UNIT_TRIKE, houseID, Tile_UnpackTile(packed), 0);
		if (u != NULL) break;
	}

	if (u == NULL) return MoveRules_Failed("could not put a trike on two clear rock tiles", 0);

	Unit_MoveRules_SetRollingTurn(true);

	/* Pointing north.  East is two octants away, north-east is one. */
	Unit_SetOrientation(u, 0, true, 0);

	u->rollingTurn = 1;
	if (!Unit_MoveRules_RollingTurn(u, 32)) {
		return MoveRules_Failed("45 degrees in motion was refused", 32);
	}
	if (Unit_MoveRules_RollingTurn(u, 32)) {
		return MoveRules_Failed("the same arrival paid for a second free turn", 32);
	}

	u->rollingTurn = 1;
	if (!Unit_MoveRules_RollingTurn(u, (int8)224)) {
		return MoveRules_Failed("45 degrees the other way was refused", 224);
	}

	u->rollingTurn = 1;
	if (Unit_MoveRules_RollingTurn(u, 64)) {
		return MoveRules_Failed("90 degrees was taken for free", 64);
	}

	u->rollingTurn = 1;
	if (Unit_MoveRules_RollingTurn(u, (int8)128)) {
		return MoveRules_Failed("a full about-turn was taken for free", 128);
	}

	/* From a standstill it costs the turn, however small. */
	u->rollingTurn = 0;
	if (Unit_MoveRules_RollingTurn(u, 32)) {
		return MoveRules_Failed("45 degrees from a standstill was free", 32);
	}

	/* And the key turns it off. */
	Unit_MoveRules_SetRollingTurn(false);
	u->rollingTurn = 1;
	if (Unit_MoveRules_RollingTurn(u, 32)) {
		return MoveRules_Failed("move_rolling_turn=0 did not disable the rule", 32);
	}
	Unit_MoveRules_SetRollingTurn(true);

	/* Arriving is what arms it.  Drive one tile east and look. */
	u->rollingTurn = 0;
	Unit_SetOrientation(u, 64, true, 0);
	Unit_SetOrientation(u, 64, true, 1);
	if (!Unit_StartMovement(u)) return MoveRules_Failed("the trike would not set off", packed);

	for (guard = 0; guard < 4096; guard++) {
		if (Unit_Move(u, min(u->speed * 16, Tile_GetDistance(u->o.position, u->currentDestination) + 16))) break;
	}

	if (guard == 4096) return MoveRules_Failed("the trike never arrived", packed);
	if (u->rollingTurn == 0) return MoveRules_Failed("arriving under power did not arm the turn", packed);

	Unit_Free(u);
	Unit_MoveRules_Init();
	return 1;
}

static int BuildRules_Failed(const char *why, uint16 detail)
{
	char line[192];

	snprintf(line, sizeof(line), "build-rules-self-test: %s (%u)", why, (unsigned)detail);
	PrintToConsole(line);
	Structure_BuildRules_Init();
	return 0;
}

static int BuildRules_SelfTest(void)
{
	uint16 packed;
	uint16 sand = 0xFFFF;
	uint16 anchor = 0xFFFF;
	uint8 houseID;
	uint8 mt;

	/* A real map rather than a synthetic one: the rule reads the landscape and
	 * the neighbourhood, and both of those are what a generated map makes. */
	if (!MpHarness_StartMatch(1000)) return 0;

	houseID = Match_GetSlotHouse(0);
	if (houseID == HOUSE_INVALID) return 0;

	/* Two adjacent empty sand tiles, anywhere on the map.  One of them becomes
	 * this house's concrete, which is the anchor the "must touch something of
	 * your own" rule wants; the other is the tile under test.  Hunting for a
	 * natural anchor was the first attempt and it finds nothing: a base is
	 * placed on rock and paves its own apron, so at tick zero there is no sand
	 * touching anything the house owns. */
	for (packed = 0; packed + 1 < 64 * 64 && sand == 0xFFFF; packed++) {
		uint16 neighbour = (uint16)(packed + 1);

		if (!Map_IsValidPosition(packed) || !Map_IsValidPosition(neighbour)) continue;
		if (Map_GetLandscapeType(packed) != LST_NORMAL_SAND) continue;
		if (Map_GetLandscapeType(neighbour) != LST_NORMAL_SAND) continue;
		if (Object_GetByPackedTile(packed) != NULL) continue;
		if (Object_GetByPackedTile(neighbour) != NULL) continue;

		anchor = neighbour;
		sand = packed;
	}

	if (anchor != 0xFFFF) {
		g_map[anchor].groundTileID = g_builtSlabTileID;
		g_map[anchor].houseID      = houseID;
	}

	if (sand == 0xFFFF) return BuildRules_Failed("the generated map has no pair of adjacent empty sand tiles", 0);

	/* Off: the original rule, rock only. */
	Structure_BuildRules_SetSlabOnSand(false);
	if (Structure_IsValidBuildLocation(sand, STRUCTURE_SLAB_1x1, houseID) != 0) {
		return BuildRules_Failed("slab accepted on sand with the rule off", sand);
	}

	/* On: concrete goes down.  A building on the same bare tile still does not --
	 * `isValidForStructure` is false for sand, and that is the original rule this
	 * change deliberately leaves alone.  Building on bare *rock* is allowed and
	 * merely unfounded; building on sand is not allowed at all, which is what
	 * makes paving a purchase rather than a decoration.  Turret because it is one
	 * tile: a 2x2 would be answering for its other three as well. */
	Structure_BuildRules_SetSlabOnSand(true);
	if (Structure_IsValidBuildLocation(sand, STRUCTURE_SLAB_1x1, houseID) == 0) {
		return BuildRules_Failed("slab refused on sand with the rule on", sand);
	}
	if (Structure_IsValidBuildLocation(sand, STRUCTURE_WALL, houseID) != 0) {
		return BuildRules_Failed("wall accepted on sand -- the rule is meant to skip walls", sand);
	}
	if (Structure_IsValidBuildLocation(sand, STRUCTURE_TURRET, houseID) != 0) {
		return BuildRules_Failed("a turret was accepted on bare sand", sand);
	}

	/* Pour it, and the same turret becomes not merely allowed but fully founded:
	 * a positive answer means no slabs are missing, which is what spares a
	 * structure the hitpoint penalty and the degrades flag. */
	g_map[sand].groundTileID = g_builtSlabTileID;
	g_map[sand].houseID      = houseID;
	if (Map_GetLandscapeType(sand) != LST_CONCRETE_SLAB) {
		return BuildRules_Failed("pouring concrete did not change the landscape type", sand);
	}
	if (Structure_IsValidBuildLocation(sand, STRUCTURE_TURRET, houseID) != 1) {
		return BuildRules_Failed("a turret on fresh concrete is still short of slabs", sand);
	}

	/* And the reason to pave at all. */
	for (mt = 0; mt < MOVEMENT_MAX; mt++) {
		if (mt == MOVEMENT_WINGER || mt == MOVEMENT_SLITHER) continue;

		if (g_table_landscapeInfo[LST_CONCRETE_SLAB].movementSpeed[mt] <=
		    g_table_landscapeInfo[LST_NORMAL_SAND].movementSpeed[mt]) {
			return BuildRules_Failed("concrete is not faster than sand, so it is not a road", mt);
		}
	}

	Structure_BuildRules_Init();
	return 1;
}

static int Pathfinder_Failed(const char *why, uint32 detail)
{
	char line[192];

	snprintf(line, sizeof(line), "pathfinder-self-test: %s (%u)", why, (unsigned)detail);
	PrintToConsole(line);
	Pathfinder_Init();
	return 0;
}

/** Tile index change per direction, as the search and the scripts both use it. */
static const int16 s_pathfinderDirection[8] = {-64, -63, 1, 65, 64, 63, -1, -65};
static const int8 s_pathfinderDirectionX[8] = { 0,  1, 1, 1, 0, -1, -1, -1};
static const int8 s_pathfinderDirectionY[8] = {-1, -1, 0, 1, 1,  1,  0, -1};

/**
 * Walk a route and add up what it costs, or 0 when it is not a route at all.
 *
 * "Not a route" covers everything the search could get wrong and still look
 * plausible: a direction byte out of range, a step that leaves the map, a step
 * onto a tile the unit may not enter, and -- the one a broken unwind produces --
 * a chain that does not actually end at the destination.
 */
static uint32 Pathfinder_ScoreRoute(Unit *unit, uint16 packedSrc, uint16 packedDst, const uint8 *route, uint16 length, bool *valid)
{
	uint32 total = 0;
	uint16 packed = packedSrc;
	uint16 i;

	*valid = false;

	for (i = 0; i < length; i++) {
		uint32 cost;
		int16 x;
		int16 y;

		if (route[i] > 7) return 0;

		x = (int16)Tile_GetPackedX(packed) + s_pathfinderDirectionX[route[i]];
		y = (int16)Tile_GetPackedY(packed) + s_pathfinderDirectionY[route[i]];
		if (x < 0 || x > 63 || y < 0 || y > 63) return 0;

		packed = (uint16)(packed + s_pathfinderDirection[route[i]]);

		cost = Pathfinder_StepCost(unit, packed, route[i]);
		if (cost == Pathfinder_Unreachable()) return 0;

		total += cost;
	}

	if (packed != packedDst) return 0;

	*valid = true;
	return total;
}

/**
 * Every tile's true distance, worked out a second time by a method with nothing
 * in common with the first.
 *
 * Repeated relaxation until nothing changes: no priority queue, no heuristic,
 * no tie-break -- so it shares no code and no assumption with A* beyond the
 * cost of a single step.  Slow, and that is fine for a test that runs once.
 * If A* and this disagree, A* is wrong.
 *
 * With `backward` it measures the cost of reaching `origin` from each tile
 * instead of leaving it, which is what the heuristic has to be a lower bound of.
 */
static bool Pathfinder_ReferenceDistances(Unit *unit, uint16 origin, bool backward, uint32 *dist)
{
	uint16 pass;
	bool changed = true;

	for (pass = 0; pass < 64 * 64; pass++) dist[pass] = 0xFFFFFFFF;
	dist[origin] = 0;

	for (pass = 0; pass < 64 * 64 && changed; pass++) {
		uint16 packed;

		changed = false;

		for (packed = 0; packed < 64 * 64; packed++) {
			uint8 direction;

			for (direction = 0; direction < 8; direction++) {
				uint16 neighbour;
				uint32 cost;
				int16 x = (int16)Tile_GetPackedX(packed) + s_pathfinderDirectionX[direction];
				int16 y = (int16)Tile_GetPackedY(packed) + s_pathfinderDirectionY[direction];

				if (x < 0 || x > 63 || y < 0 || y > 63) continue;

				neighbour = (uint16)(packed + s_pathfinderDirection[direction]);
				if (!Map_IsValidPosition(neighbour)) continue;

				/* The cost of a step is charged on the tile being entered, so
				 * the two directions of travel are not the same number and the
				 * backward pass has to say which one it means. */
				cost = Pathfinder_StepCost(unit, neighbour, direction);
				if (cost == Pathfinder_Unreachable()) continue;

				if (backward) {
					if (dist[neighbour] == 0xFFFFFFFF) continue;
					if (dist[neighbour] + cost < dist[packed]) {
						dist[packed] = dist[neighbour] + cost;
						changed = true;
					}
				} else {
					if (dist[packed] == 0xFFFFFFFF) continue;
					if (dist[packed] + cost < dist[neighbour]) {
						dist[neighbour] = dist[packed] + cost;
						changed = true;
					}
				}
			}
		}
	}

	return true;
}

/**
 * How long the engine really takes to cross one tile, measured by crossing it.
 *
 * The whole change rests on the claim that Pathfinder_StepTicks() is the time
 * the movement layer will actually spend, so that claim is worth checking
 * against the movement layer rather than against the arithmetic it was derived
 * from -- the accumulator below is Unit_MovementTick()'s, and every other part
 * of the step is the engine's own.  Without this, a mistake in the metric would
 * be invisible: the search and the reference both read it from the same place
 * and would agree with each other about the wrong number.
 *
 * @return Game ticks spent, or 0 when the unit would not set off.
 */
static uint32 Pathfinder_MeasuredTicks(Unit *unit, int8 orientation)
{
	uint32 ticks = 0;
	uint16 remainder = 0;
	uint16 guard;

	Unit_SetOrientation(unit, orientation, true, 0);
	Unit_SetOrientation(unit, orientation, true, 1);

	if (!Unit_StartMovement(unit)) return 0;

	for (guard = 0; guard < 100000; guard++) {
		remainder = (uint16)(remainder + unit->speedPerTick);
		ticks += 3;

		if ((remainder & 0xFF00) != 0) {
			if (Unit_Move(unit, min(unit->speed * 16, Tile_GetDistance(unit->o.position, unit->currentDestination) + 16))) return ticks;
		}

		remainder &= 0xFF;
	}

	return 0;
}

/**
 * That the route search returns the cheapest route there is, and keeps
 * returning the same one.
 *
 * The obstacle is a cup with its mouth pointing away from the unit and the
 * destination inside it -- the shape Westwood's router is worst at, because it
 * walks straight at the destination, meets the back of the cup and then has to
 * feel its way out along the wall it is already touching.  It is built out of
 * parked units rather than terrain so the test owns it exactly: an allied unit
 * standing still is a wall to both routers, by the same line of
 * Unit_GetTileEnterScore().
 *
 * Four claims, in the order they matter:
 *
 *  - the heuristic never overestimates, checked against every landscape type
 *    rather than argued;
 *  - the route is a route: connected, on the map, passable end to end;
 *  - it costs exactly what an independent shortest-path calculation says it
 *    should, which is the only way to catch a search that returns something
 *    valid but not optimal;
 *  - and the same question asked twice gets the same answer, byte for byte,
 *    which is what a lockstep match depends on.
 *
 * @return 1 when everything held, 0 on the first thing that did not.
 */
static int Pathfinder_SelfTest(void)
{
	static const int8 s_cupX[13] = {-2, -1, 0, 1, 2, -2, -2, -2, -2, 2, 2, 2, 2};
	static const int8 s_cupY[13] = {-3, -3, -3, -3, -3, -2, -1, 0, 1, -2, -1, 0, 1};

	Unit *unit = NULL;
	Unit *walls[13];
	uint8 route[128];
	uint8 sample[128];
	uint8 legacy[128];
	uint8 again[128];
	uint16 wallCount = 0;
	uint16 origin = 0xFFFF;
	uint16 packedSrc = 0;
	uint16 packedDst = 0;
	uint16 goals[2];
	uint16 length;
	uint16 legacyLength;
	uint16 failedLandscape = 0;
	uint16 packed;
	uint32 cost;
	uint32 reference;
	uint32 legacyCost;
	uint32 *dist;
	uint32 ticks;
	uint32 expansions;
	uint8 houseID;
	bool valid;
	int i;

	if (!MpHarness_StartMatch(1000)) return 0;

	houseID = Match_GetSlotHouse(0);
	if (houseID == HOUSE_INVALID) return 0;

	/* An eleven by eleven patch of open ground with nothing on it.  The cup is
	 * drawn in the middle of it, so every tile the answer depends on is one
	 * this test put there. */
	for (packed = 0; packed < 64 * 64 && origin == 0xFFFF; packed++) {
		int16 x = (int16)Tile_GetPackedX(packed);
		int16 y = (int16)Tile_GetPackedY(packed);
		int16 dx;
		int16 dy;
		bool clear = true;

		if (x + 10 > 63 || y + 10 > 63) continue;

		for (dy = 0; dy <= 10 && clear; dy++) {
			for (dx = 0; dx <= 10 && clear; dx++) {
				uint16 t = Tile_PackXY((uint16)(x + dx), (uint16)(y + dy));

				if (!Map_IsValidPosition(t)) clear = false;
				if (Object_GetByPackedTile(t) != NULL) clear = false;
				if (Map_GetLandscapeType(t) == LST_ENTIRELY_MOUNTAIN) clear = false;
				if (Map_GetLandscapeType(t) == LST_PARTIAL_MOUNTAIN) clear = false;
				if (Map_GetLandscapeType(t) == LST_WALL) clear = false;
			}
		}

		if (clear) origin = Tile_PackXY((uint16)(x + 5), (uint16)(y + 5));
	}

	if (origin == 0xFFFF) return Pathfinder_Failed("the generated map has no clear 11x11 patch", 0);

	packedDst = origin;
	packedSrc = (uint16)(origin - 5 * 64);

	unit = Unit_Create(UNIT_INDEX_INVALID, UNIT_TRIKE, houseID, Tile_UnpackTile(packedSrc), 0);
	if (unit == NULL) return Pathfinder_Failed("could not put a trike on the patch", packedSrc);

	/* Claim one, before anything is built: the estimate must never exceed the
	 * truth, for every ground this unit could be standing on. */
	if (!Pathfinder_CheckHeuristic(unit, &failedLandscape)) {
		Unit_Free(unit);
		return Pathfinder_Failed("the heuristic overestimates on landscape type", failedLandscape);
	}

	/* The metric, against the engine.  A throwaway unit because measuring means
	 * actually driving it, and a diagonal has to come out about sqrt(2) times a
	 * straight one -- 362 against 256 -- which is the one number in the cost
	 * function that no other part of this test can see. */
	{
		Unit *probe;
		uint32 straightMeasured;
		uint32 diagonalMeasured;
		uint32 straightClaimed;
		uint32 diagonalClaimed;

		probe = Unit_Create(UNIT_INDEX_INVALID, UNIT_TRIKE, houseID, Tile_UnpackTile((uint16)(packedSrc + 1)), 0);
		if (probe == NULL) {
			Unit_Free(unit);
			return Pathfinder_Failed("could not put a second trike on the patch", packedSrc);
		}

		straightClaimed  = Pathfinder_StepTicks(probe, (uint16)(packedSrc + 2), 2);
		straightMeasured = Pathfinder_MeasuredTicks(probe, 64);
		Unit_Free(probe);

		probe = Unit_Create(UNIT_INDEX_INVALID, UNIT_TRIKE, houseID, Tile_UnpackTile((uint16)(packedSrc + 1)), 0);
		if (probe == NULL) {
			Unit_Free(unit);
			return Pathfinder_Failed("could not put a second trike on the patch", packedSrc);
		}

		diagonalClaimed  = Pathfinder_StepTicks(probe, (uint16)(packedSrc + 2 + 64), 3);
		diagonalMeasured = Pathfinder_MeasuredTicks(probe, 96);
		Unit_Free(probe);

		if (straightMeasured == 0 || diagonalMeasured == 0) {
			Unit_Free(unit);
			return Pathfinder_Failed("the probe would not cross a tile", straightMeasured);
		}

		/* Within a movement tick either way: the engine stops when the tile is
		 * within sixteen units rather than at exactly zero, so a step is a
		 * fraction shorter than its nominal length. */
		if (straightClaimed + 6 < straightMeasured || straightMeasured + 6 < straightClaimed) {
			char line[192];

			snprintf(line, sizeof(line), "pathfinder-self-test: straight step claimed %u ticks, measured %u",
				(unsigned)straightClaimed, (unsigned)straightMeasured);
			PrintToConsole(line);
			Unit_Free(unit);
			return Pathfinder_Failed("the cost of a straight step is not what it takes", straightClaimed);
		}

		if (diagonalClaimed + 6 < diagonalMeasured || diagonalMeasured + 6 < diagonalClaimed) {
			char line[192];

			snprintf(line, sizeof(line), "pathfinder-self-test: diagonal step claimed %u ticks, measured %u",
				(unsigned)diagonalClaimed, (unsigned)diagonalMeasured);
			PrintToConsole(line);
			Unit_Free(unit);
			return Pathfinder_Failed("the cost of a diagonal step is not what it takes", diagonalClaimed);
		}
	}

	for (i = 0; i < 13; i++) {
		uint16 t = (uint16)(origin + s_cupX[i] + s_cupY[i] * 64);

		walls[wallCount] = Unit_Create(UNIT_INDEX_INVALID, UNIT_TRIKE, houseID, Tile_UnpackTile(t), 0);
		if (walls[wallCount] == NULL) break;
		wallCount++;
	}

	if (wallCount != 13) {
		for (i = 0; i < (int)wallCount; i++) Unit_Free(walls[i]);
		Unit_Free(unit);
		return Pathfinder_Failed("could not build the cup out of units", wallCount);
	}

	Pathfinder_SetEnabled(true);

	if (!Pathfinder_Run(unit, packedSrc, &packedDst, 1, true)) {
		cost = 0;
	} else {
		length = Pathfinder_GetRoute(packedDst, route, sizeof(route));
		cost = Pathfinder_ScoreRoute(unit, packedSrc, packedDst, route, length, &valid);
	}

	if (cost == 0 || !valid) {
		for (i = 0; i < (int)wallCount; i++) Unit_Free(walls[i]);
		Unit_Free(unit);
		return Pathfinder_Failed("no valid route out of the cup", packedDst);
	}

	/* The route the search says it found and the score it kept for it have to be
	 * the same number.  They come from different code -- one unwinds cameFrom,
	 * the other adds up the steps -- so a bug in either shows here. */
	if (!Pathfinder_GetTicks(packedDst, &ticks) || ticks != cost) {
		for (i = 0; i < (int)wallCount; i++) Unit_Free(walls[i]);
		Unit_Free(unit);
		return Pathfinder_Failed("the route does not cost what the search recorded", (uint32)cost);
	}

	dist = (uint32 *)malloc(64 * 64 * sizeof(uint32));
	if (dist == NULL) {
		for (i = 0; i < (int)wallCount; i++) Unit_Free(walls[i]);
		Unit_Free(unit);
		return Pathfinder_Failed("out of memory for the reference distances", 0);
	}

	Pathfinder_ReferenceDistances(unit, packedSrc, false, dist);
	reference = (dist[packedDst] == 0xFFFFFFFF) ? 0 : dist[packedDst];

	if (reference == 0 || cost != reference) {
		char line[192];

		snprintf(line, sizeof(line), "pathfinder-self-test: A* %u ticks, shortest possible %u", (unsigned)cost, (unsigned)reference);
		PrintToConsole(line);
		free(dist);
		for (i = 0; i < (int)wallCount; i++) Unit_Free(walls[i]);
		Unit_Free(unit);
		return Pathfinder_Failed("the route is not the cheapest one", (uint32)cost);
	}

	/* Claim one again, and this time against the thing itself rather than its
	 * ingredients: measure what it really costs to reach the destination from
	 * every tile on the map, and require the estimate to be no larger anywhere.
	 * A heuristic that overestimates makes A* close the destination too early
	 * and return a route that is valid, plausible and not the shortest -- with
	 * nothing anywhere to say so.  This is the assertion that catches it, and
	 * it catches it whatever the obstacle happens to look like. */
	Pathfinder_ReferenceDistances(unit, packedDst, true, dist);

	for (packed = 0; packed < 64 * 64; packed++) {
		if (dist[packed] == 0xFFFFFFFF) continue;

		if (Pathfinder_GetHeuristic(packed) > dist[packed]) {
			char line[192];

			snprintf(line, sizeof(line), "pathfinder-self-test: tile %u estimated at %u, truly %u",
				(unsigned)packed, (unsigned)Pathfinder_GetHeuristic(packed), (unsigned)dist[packed]);
			PrintToConsole(line);
			free(dist);
			for (i = 0; i < (int)wallCount; i++) Unit_Free(walls[i]);
			Unit_Free(unit);
			return Pathfinder_Failed("the heuristic overestimates, so routes are not optimal", packed);
		}
	}

	/* One obstacle proves the search can get round an obstacle.  It does not
	 * prove the search is a shortest-path search, because on an obstacle this
	 * small the first route found to a tile is usually the cheapest one anyway
	 * -- so a search that never revisits a tile it has already estimated would
	 * pass, and be wrong everywhere the map is more interesting.  The forward
	 * pass measured every tile, so asking about more destinations is very nearly
	 * free: take them from across the whole generated map, where the ground
	 * really does vary, and require the same exact agreement each time. */
	Pathfinder_ReferenceDistances(unit, packedSrc, false, dist);

	{
		uint16 checked = 0;

		for (packed = 173; packed < 64 * 64 && checked < 48; packed = (uint16)(packed + 173)) {
			uint16 sampleLength;
			uint32 sampleCost;
			uint16 sampleGoal = packed;
			bool sampleValid;

			if (dist[packed] == 0xFFFFFFFF || dist[packed] == 0) continue;

			if (!Pathfinder_Run(unit, packedSrc, &sampleGoal, 1, true)) {
				free(dist);
				for (i = 0; i < (int)wallCount; i++) Unit_Free(walls[i]);
				Unit_Free(unit);
				return Pathfinder_Failed("a tile the reference can reach was reported unreachable", packed);
			}

			sampleLength = Pathfinder_GetRoute(packed, sample, sizeof(sample));
			sampleCost = Pathfinder_ScoreRoute(unit, packedSrc, packed, sample, sampleLength, &sampleValid);

			if (!sampleValid || sampleCost != dist[packed]) {
				char line[192];

				snprintf(line, sizeof(line), "pathfinder-self-test: tile %u routed at %u ticks over %u steps, shortest possible %u",
					(unsigned)packed, (unsigned)sampleCost, (unsigned)sampleLength, (unsigned)dist[packed]);
				PrintToConsole(line);
				free(dist);
				for (i = 0; i < (int)wallCount; i++) Unit_Free(walls[i]);
				Unit_Free(unit);
				return Pathfinder_Failed("a sampled route is not the cheapest one", packed);
			}

			checked++;
		}

		if (checked < 16) {
			free(dist);
			for (i = 0; i < (int)wallCount; i++) Unit_Free(walls[i]);
			Unit_Free(unit);
			return Pathfinder_Failed("too few reachable tiles to sample, so this proves little", checked);
		}
	}

	free(dist);

	/* Same question, same answer.  Two clients must not disagree about it. */
	if (!Pathfinder_Run(unit, packedSrc, &packedDst, 1, true)) {
		for (i = 0; i < (int)wallCount; i++) Unit_Free(walls[i]);
		Unit_Free(unit);
		return Pathfinder_Failed("the same search failed the second time", packedDst);
	}
	if (Pathfinder_GetRoute(packedDst, again, sizeof(again)) != length || memcmp(route, again, length) != 0) {
		for (i = 0; i < (int)wallCount; i++) Unit_Free(walls[i]);
		Unit_Free(unit);
		return Pathfinder_Failed("the same search returned a different route", packedDst);
	}

	/* A set of goals is answered in one search, and the cheapest of them is the
	 * one that comes back -- this is what the firing-position and harvester
	 * callers ask, dozens of times per unit. */
	goals[0] = packedDst;
	goals[1] = (uint16)(origin + 4);
	if (!Pathfinder_Run(unit, packedSrc, goals, 2, false)) {
		for (i = 0; i < (int)wallCount; i++) Unit_Free(walls[i]);
		Unit_Free(unit);
		return Pathfinder_Failed("a two-goal search found neither", packedDst);
	}
	if (!Pathfinder_GetTicks(packedDst, &ticks) || ticks != cost) {
		for (i = 0; i < (int)wallCount; i++) Unit_Free(walls[i]);
		Unit_Free(unit);
		return Pathfinder_Failed("a goal cost more when asked about alongside another", (uint32)ticks);
	}

	/* What it bought.  Scored with the same metre stick, so the comparison is
	 * about the routes and not about the two routers' opinions of a tile. */
	legacyLength = Script_Unit_LegacyRoute(unit, packedSrc, packedDst, legacy, sizeof(legacy));
	legacyCost = Pathfinder_ScoreRoute(unit, packedSrc, packedDst, legacy, legacyLength, &valid);

	{
		char line[192];

		snprintf(line, sizeof(line), "pathfinder-self-test: cup of %u tiles -- A* %u ticks over %u steps, Westwood %s%u ticks over %u steps",
			(unsigned)wallCount, (unsigned)cost, (unsigned)length,
			valid ? "" : "no route, ", (unsigned)legacyCost, (unsigned)legacyLength);
		PrintToConsole(line);
	}

	if (valid && legacyCost < cost) {
		for (i = 0; i < (int)wallCount; i++) Unit_Free(walls[i]);
		Unit_Free(unit);
		return Pathfinder_Failed("the old router beat the shortest route, so the metric is wrong", (uint32)legacyCost);
	}

	/* And the key turns it off: with the rule off the tactical probe goes
	 * through Westwood's router instead, which is visible because no node is
	 * expanded at all. */
	Pathfinder_SetEnabled(false);
	expansions = Pathfinder_GetExpansions();
	Script_Unit_HasRoute(unit, packedSrc, packedDst, &ticks);
	if (Pathfinder_GetExpansions() != expansions) {
		Pathfinder_SetEnabled(true);
		for (i = 0; i < (int)wallCount; i++) Unit_Free(walls[i]);
		Unit_Free(unit);
		return Pathfinder_Failed("pathfinder_astar=0 did not disable the search", 0);
	}
	Pathfinder_SetEnabled(true);
	Script_Unit_HasRoute(unit, packedSrc, packedDst, &ticks);
	if (Pathfinder_GetExpansions() == expansions) {
		for (i = 0; i < (int)wallCount; i++) Unit_Free(walls[i]);
		Unit_Free(unit);
		return Pathfinder_Failed("pathfinder_astar=1 did not enable the search", 0);
	}

	for (i = 0; i < (int)wallCount; i++) Unit_Free(walls[i]);
	Unit_Free(unit);
	Pathfinder_Init();
	return 1;
}

static void GameLoop_Main(void)
{
	static uint32 l_timerNext = 0;
	static uint32 l_timerUnitStatus = 0;
	static int16  l_selectionState = -2;
	static uint16 l_lastGroupDigit = 0xFFFF;

	uint16 key;

	String_Init();
	Sprites_Init();

#ifdef MUNT
	if (IniFile_GetInteger("mt32midi", 1) != 0) Music_InitMT32();
#else
	if (IniFile_GetInteger("mt32midi", 0) != 0) Music_InitMT32();
#endif

	Input_Flags_SetBits(INPUT_FLAG_KEY_REPEAT | INPUT_FLAG_UNKNOWN_0010 | INPUT_FLAG_UNKNOWN_0200 |
	                    INPUT_FLAG_KBD_MOUSE_CLK);
	Input_Flags_ClearBits(INPUT_FLAG_KEY_RELEASE | INPUT_FLAG_UNKNOWN_0400 | INPUT_FLAG_UNKNOWN_0100 |
	                      INPUT_FLAG_UNKNOWN_0080 | INPUT_FLAG_UNKNOWN_0040 | INPUT_FLAG_UNKNOWN_0020 |
	                      INPUT_FLAG_UNKNOWN_0008 | INPUT_FLAG_UNKNOWN_0004 | INPUT_FLAG_NO_TRANSLATE);

	Timer_SetTimer(TIMER_GAME, true);
	Timer_SetTimer(TIMER_GUI, true);

	g_campaignID = 0;
	g_scenarioID = 1;
	g_playerHouseID = HOUSE_INVALID;
	g_selectionType = SELECTIONTYPE_MENTAT;
	g_selectionTypeNew = SELECTIONTYPE_MENTAT;

	if (g_palette1) Warning("g_palette1\n");
	else g_palette1 = calloc(1, 256 * 3);
	if (g_palette2) Warning("g_palette2\n");
	else g_palette2 = calloc(1, 256 * 3);

	g_readBufferSize = 12000;
	g_readBuffer = calloc(1, g_readBufferSize);

	ReadProfileIni("PROFILE.INI");
	Unit_CombatBalance_Init();
	Unit_MoveRules_Init();
	Structure_BuildRules_Init();
	Pathfinder_Init();
	if (s_pathfinderOverride >= 0) Pathfinder_SetEnabled(s_pathfinderOverride != 0);
	Starport_Init();

	free(g_readBuffer); g_readBuffer = NULL;

	File_ReadBlockFile("IBM.PAL", g_palette1, 256 * 3);

	GUI_ClearScreen(SCREEN_0);

	Video_SetPalette(g_palette1, 0, 256);

	GFX_SetPalette(g_palette1);
	GFX_SetPalette(g_palette2);

	g_paletteMapping1 = malloc(256);
	g_paletteMapping2 = malloc(256);

	GUI_Palette_CreateMapping(g_palette1, g_paletteMapping1, 0xC, 0x55);
	g_paletteMapping1[0xFF] = 0xFF;
	g_paletteMapping1[0xDF] = 0xDF;
	g_paletteMapping1[0xEF] = 0xEF;

	GUI_Palette_CreateMapping(g_palette1, g_paletteMapping2, 0xF, 0x55);
	g_paletteMapping2[0xFF] = 0xFF;
	g_paletteMapping2[0xDF] = 0xDF;
	g_paletteMapping2[0xEF] = 0xEF;

	Script_LoadFromFile("TEAM.EMC", g_scriptTeam, g_scriptFunctionsTeam, NULL);
	Script_LoadFromFile("BUILD.EMC", g_scriptStructure, g_scriptFunctionsStructure, NULL);

	GUI_Palette_CreateRemap(HOUSE_MERCENARY);

	g_cursorSpriteID = 0;

	Sprites_SetMouseSprite(0, 0, g_sprites[0]);

	while (g_mouseHiddenDepth > 1) {
		GUI_Mouse_Show_Safe();
	}

	Window_WidgetClick_Create();
	GameOptions_Load();
	Unit_Init();
	Team_Init();
	House_Init();
	Structure_Init();

	GUI_Mouse_Show_Safe();

	if (s_lobbySelfTest) {
		s_lobbySelfTestResult = GUI_Lobby_RunSelfTest();
		PrintToConsole((s_lobbySelfTestResult == 1) ? "lobby-self-test: PASS"
		                                            : "lobby-self-test: FAIL");
		return;
	}

	if (s_moveRulesSelfTest) {
		g_readBufferSize = (g_enableVoices == 0) ? 12000 : 20000;
		g_readBuffer = calloc(1, g_readBufferSize);

		s_moveRulesSelfTestResult = MoveRules_SelfTest();
		PrintToConsole((s_moveRulesSelfTestResult == 1) ? "move-rules-self-test: PASS"
		                                                : "move-rules-self-test: FAIL");
		return;
	}

	if (s_pathfinderSelfTest) {
		g_readBufferSize = (g_enableVoices == 0) ? 12000 : 20000;
		g_readBuffer = calloc(1, g_readBufferSize);

		s_pathfinderSelfTestResult = Pathfinder_SelfTest();
		PrintToConsole((s_pathfinderSelfTestResult == 1) ? "pathfinder-self-test: PASS"
		                                                 : "pathfinder-self-test: FAIL");
		return;
	}

	if (s_buildRulesSelfTest) {
		g_readBufferSize = (g_enableVoices == 0) ? 12000 : 20000;
		g_readBuffer = calloc(1, g_readBufferSize);

		s_buildRulesSelfTestResult = BuildRules_SelfTest();
		PrintToConsole((s_buildRulesSelfTestResult == 1) ? "build-rules-self-test: PASS"
		                                                 : "build-rules-self-test: FAIL");
		return;
	}

	if (s_combatBalanceSelfTest) {
		s_combatBalanceSelfTestResult = Unit_CombatBalance_RunRegressionTest();
		if (s_combatBalanceSelfTestResult == 1) {
			PrintToConsole("combat-balance-self-test: PASS");
		} else if (s_combatBalanceSelfTestResult == -1) {
			PrintToConsole("combat-balance-self-test: SKIP (module disabled)");
		} else {
			PrintToConsole("combat-balance-self-test: FAIL");
		}
		return;
	}

	if (s_ecoBaseline || s_ecoSearch || s_ecoGrid || s_ecoQueue || s_ecoCarryall ||
	    s_warMatrix || s_warTiming || s_warLadder || s_warTelemetry || s_warMetrics) {
		g_readBufferSize = (g_enableVoices == 0) ? 12000 : 20000;
		g_readBuffer = calloc(1, g_readBufferSize);

		if (s_ecoBaseline) EcoSearch_RunBaseline(s_ecoTicks, s_ecoMaps);
		if (s_ecoGrid) EcoSearch_RunGrid(s_ecoTicks, s_ecoMaps);
		if (s_ecoQueue) EcoSearch_RunQueueSweep(s_ecoTicks, s_ecoMaps);
		if (s_ecoCarryall) EcoSearch_RunCarryallSweep(s_ecoTicks, s_ecoMaps);
		if (s_ecoSearch) EcoSearch_Run(s_ecoPopulation, s_ecoGenerations, s_ecoTicks, s_ecoMaps);
		if (s_warMatrix) WarSearch_RunMatrix(s_warTicks, s_warMaps);
		if (s_warTiming) WarSearch_RunTiming(s_warTicks, s_warMaps);
		if (s_warLadder) WarSearch_RunLadder(s_warTicks, s_warMaps);
		if (s_warMetrics) WarSearch_RunMetrics(s_skirmishHouse[0], s_skirmishHouse[1], s_warTicks, s_warMaps);
		if (s_warTelemetry) WarSearch_RunTelemetry(s_skirmishHouse[0], s_skirmishHouse[1], s_warTicks, s_warTelemetryStep, s_warPlayShare[0], s_warPlayShare[1], s_warSwitchTick, s_warPlaySeed);
		return;
	}

	/* The menu allocates this on its way into a game; both skirmish entry
	 * points skip the menu, and the voice player writes through it. */
	if (s_skirmishSelfTest || s_skirmishDirect || s_warPlay || s_mpChecksum || s_mpReplay || s_mpModal) {
		g_readBufferSize = (g_enableVoices == 0) ? 12000 : 20000;
		g_readBuffer = calloc(1, g_readBufferSize);
	}

	/* Stage 0 of mp.md: prove the simulation is deterministic before a single
	 * line of network code exists.  Same binary and same seed must give the same
	 * log, twice in a row and then on a second platform.  The checksum is split
	 * per savegame chunk, so a mismatch names what diverged. */
	if (s_mpChecksum) {
		MpSyncChecksum checksum;
		char line[256];
		uint32 tick;

		if (!MpHarness_StartMatch(s_mpChecksumSeed)) {
			PrintToConsole("mp-checksum: FAIL (could not start a skirmish)");
			return;
		}

		for (tick = 0; ; tick++) {
			if ((tick % s_mpChecksumStep) == 0 || tick == s_mpChecksumTicks) {
				if (!MpSync_Take(&checksum)) {
					PrintToConsole("mp-checksum: FAIL (could not serialise the state)");
					return;
				}

				MpSync_Format(line, sizeof(line), tick, &checksum);
				PrintToConsole(line);
			}

			if (tick == s_mpChecksumTicks) break;

			MpHarness_Step();
		}

		PrintToConsole("mp-checksum: DONE");
		return;
	}

	/* Stage 4 of mp.md: the turn loop.
	 *
	 * The same match, played the way a networked one has to be: a command does
	 * not happen when it is given, it is stamped for a turn far enough ahead
	 * that every player's copy will have arrived, and it runs there.  The
	 * simulation clock stops dead while somebody's packet is missing, which is
	 * the only way latency is allowed to show.
	 *
	 * Without --mp-net this is one process playing both slots over a loopback
	 * transport -- which is not a stand-in for the network, it is what a local
	 * match uses.  With it, this process is one player and the other is somebody
	 * else's, and the two see each other only through packets. */
	if (s_mpTurnLoop) {
		const MpTransport *transport;
		MpSyncChecksum sample;
		char line[256];
		uint32 waited = 0;
		uint32 samples = 0;
		uint32 startedAt = 0;
		uint32 lateBy = 0;
		uint32 stalledMs = 0;
		uint32 tick;
		uint8 houseID;

		if (s_mpRelayHost[0] != '\0') {
			uint32 until;

			if (!MpNet_Connect(s_mpRelayHost, s_mpRelayPort, s_mpRelayRoom, s_mpTurnSlot)) {
				snprintf(line, sizeof(line), "mp-turnloop: FAIL (%s)", MpNet_GetError());
				PrintToConsole(line);
				return;
			}

			snprintf(line, sizeof(line), "mp-turnloop: room %s, slot %u, waiting for the other player",
			         s_mpRelayRoom, (unsigned)(s_mpTurnSlot + 1));
			PrintToConsole(line);

			/* Both players in the room before the first tick.  A match that
			 * started half joined would spend its opening turns stalled, and the
			 * stall figures below are meant to measure the wire, not the lobby. */
			until = Timer_GetTime() + s_mpNetWaitMs;
			while (!MpNet_IsReady()) {
				MpNet_Pump();

				if (!MpNet_IsConnected() || Timer_GetTime() > until) {
					snprintf(line, sizeof(line), "mp-turnloop: FAIL (%s)",
					         MpNet_IsConnected() ? "the other player never joined" : MpNet_GetError());
					PrintToConsole(line);
					MpNet_Disconnect();
					return;
				}

				msleep(5);
			}

			transport = MpTransport_Net();
			Skirmish_SetViewpoint(s_mpTurnSlot);
		} else if (s_mpNetDirectory[0] != '\0') {
			transport = MpTransport_File(s_mpNetDirectory);
			MpTransport_File_SetLag(s_mpNetLagMs);
			Skirmish_SetViewpoint(s_mpTurnSlot);
		} else {
			MpTransport_Loopback_Reset();
			transport = MpTransport_Loopback();
		}

		if (!MpHarness_StartMatch(s_mpReplaySeed)) {
			PrintToConsole("mp-turnloop: FAIL (could not start a skirmish)");
			return;
		}

		houseID = s_skirmishHouse[s_mpTurnSlot];

		MpTurn_Begin(s_mpTurnSlot, transport, s_mpTurnLength, s_mpTurnDelay);
		MpTurn_SetSnapshots(s_mpDesyncDump);

		startedAt = Timer_GetTime();

		for (tick = 0; ; tick++) {
			/* Real time or as fast as the CPU allows.  The difference decides
			 * whether a lag measurement means anything: a turn is 133 ms of wall
			 * clock in a real game and microseconds in a headless one, so only
			 * the paced run can answer whether a given ping stalls anybody. */
			if (s_mpRealtime) {
				uint32 due = startedAt + (tick * 1000 / 60);
				uint32 now = Timer_GetTime();

				if (now < due) {
					msleep(due - now);
				} else if (now - due > lateBy) {
					lateBy = now - due;
				}
			}

			/* Turn boundaries first: everybody's commands for the turn starting
			 * here are applied before anything simulates in it. */
			while (MpTurn_IsDue()) {
				uint32 stallStart = Timer_GetTime();

				/* A loopback match has nobody on the other side of the wire, so
				 * this process speaks for the empty slot too -- one turn ahead
				 * of the one being applied, which is what the absent player
				 * would have sent. */
				if (s_mpNetDirectory[0] == '\0' && s_mpRelayHost[0] == '\0') {
					uint8 other = (uint8)((s_mpTurnSlot == 0) ? 1 : 0);
					MpPacket empty;

					memset(&empty, 0, sizeof(empty));
					empty.turn      = MpTurn_GetTurn();
					empty.checkTurn = MP_TURN_NO_CHECKSUM;
					transport->send(other, &empty);
				}

				if (MpTurn_Advance()) {
					waited = 0;
					continue;
				}

				/* Stalled.  A real client would draw another frame here; the
				 * harness has nothing to draw, so it sleeps and polls. */
				msleep(2);
				waited += 2;
				stalledMs += Timer_GetTime() - stallStart;

				/* Time spent here is time the simulation clock stood still, so it
				 * has to come off the real-time schedule as well -- otherwise the
				 * pacing below would sprint to catch up and hide the stall. */
				if (s_mpRealtime) startedAt += Timer_GetTime() - stallStart;

				/* The other player leaving is not a stall, it is the end of the
				 * match, and the relay says so the moment it happens.  Sitting
				 * out the full timeout for a packet nobody is left to send would
				 * turn a one-line answer into twenty seconds of silence -- and
				 * in a real game, into twenty seconds of frozen screen. */
				if (s_mpRelayHost[0] != '\0') {
					uint8 goneSlot = 0;

					if (MpNet_HasLeft(&goneSlot)) {
						snprintf(line, sizeof(line), "mp-turnloop: FAIL (slot %u left the match at turn %u)",
						         (unsigned)(goneSlot + 1), (unsigned)MpTurn_GetTurn());
						PrintToConsole(line);
						MpNet_Disconnect();
						return;
					}

					if (!MpNet_IsConnected()) {
						snprintf(line, sizeof(line), "mp-turnloop: FAIL (%s)", MpNet_GetError());
						PrintToConsole(line);
						MpNet_Disconnect();
						return;
					}
				}

				if (waited > s_mpNetWaitMs) {
					snprintf(line, sizeof(line), "mp-turnloop: FAIL (no packet for turn %u after %u ms)",
					         (unsigned)MpTurn_GetTurn(), (unsigned)waited);
					PrintToConsole(line);
					if (s_mpRelayHost[0] != '\0') MpNet_Disconnect();
					return;
				}
			}

			MpHarness_ScriptedPlayer(houseID, tick);

			if ((tick % s_mpReplayStep) == 0 || tick == s_mpReplayTicks) {
				if (!MpSync_Take(&sample)) {
					PrintToConsole("mp-turnloop: FAIL (could not serialise the state)");
					return;
				}

				MpSync_Format(line, sizeof(line), tick, &sample);
				PrintToConsole(line);
				samples++;
			}

			if (tick == s_mpReplayTicks) break;

			MpHarness_Step();
		}

		{
			uint32 desyncTurn = 0;
			bool desynced = MpTurn_HasDesynced(&desyncTurn);
			char wire[192];

			if (s_mpRelayHost[0] != '\0') {
				snprintf(wire, sizeof(wire), "relay %s:%u room %s", s_mpRelayHost, (unsigned)s_mpRelayPort, s_mpRelayRoom);
			} else {
				snprintf(wire, sizeof(wire), "lag%u", (unsigned)s_mpNetLagMs);
			}

			/* Name the wire, because the same numbers mean different things on
			 * each one: the file transport's lag is a figure we chose, the
			 * relay's is whatever the internet did. */
			snprintf(line, sizeof(line), "mp-turnloop: slot %u tl%u d%u %s played %u turns over %u ticks, %u samples, %u ms stalled",
			         (unsigned)(s_mpTurnSlot + 1), (unsigned)s_mpTurnLength, (unsigned)s_mpTurnDelay,
			         wire, (unsigned)MpTurn_GetTurn(), (unsigned)s_mpReplayTicks,
			         (unsigned)samples, (unsigned)stalledMs);
			PrintToConsole(line);

			if (desynced) {
				snprintf(line, sizeof(line), "mp-turnloop: FAIL (the two players disagreed about turn %u)", (unsigned)desyncTurn);
				PrintToConsole(line);
			} else {
				PrintToConsole("mp-turnloop: DONE");
			}
		}

		MpTurn_End();
		Timer_ClaimAnimClock(false);
		if (s_mpRelayHost[0] != '\0') MpNet_Disconnect();
		return;
	}

	/* Stage 5 of mp.md: the modal screens.  A player who opens the options or
	 * the mentat must not end the match for the other one. */
	if (s_mpModal) {
		MpSyncChecksum quiet[MP_REPLAY_SAMPLES_MAX];
		MpSyncChecksum opened[MP_REPLAY_SAMPLES_MAX];
		uint16 quietCount = 0;
		uint16 openedCount = 0;

		if (!MpHarness_ModalPass(false, quiet, &quietCount, false) ||
		    !MpHarness_ModalPass(true, opened, &openedCount, false)) {
			PrintToConsole("mp-modal: FAIL (could not run the match)");
			return;
		}

		if (quietCount != openedCount) {
			PrintToConsole("mp-modal: FAIL (the two passes did not reach the same length)");
			return;
		}

		MpHarness_ReportModal(quiet, opened, quietCount);
		return;
	}

	if (s_mpReplay) {
		MpSyncChecksum live[MP_REPLAY_SAMPLES_MAX];
		MpSyncChecksum replayed[MP_REPLAY_SAMPLES_MAX];
		char line[256];
		uint16 liveCount = 0;
		uint16 replayCount = 0;
		uint16 commands;
		uint16 i;

		/* The consumer half of the cross-process test: no scripted player at
		 * all, just somebody else's recording and this process's own opinion of
		 * what the match looks like as it runs. */
		if (s_mpPlayFile[0] != '\0') {
			if (!MpCommand_LoadRecord(s_mpPlayFile, s_mpReplaySeed)) {
				PrintToConsole("mp-replay: FAIL (could not read the recording, or it was taken on another seed)");
				return;
			}

			commands = MpCommand_GetRecordCount();
			if (commands == 0) {
				PrintToConsole("mp-replay: FAIL (the recording is empty, so nothing would be tested)");
				return;
			}

			if (!MpHarness_ReplayPass(false, replayed, &replayCount, true)) {
				PrintToConsole("mp-replay: FAIL (could not run the match)");
				return;
			}

			snprintf(line, sizeof(line), "mp-replay: played %u of %u commands, %u samples",
			         (unsigned)(commands - s_mpReplayUnplayed), (unsigned)commands, (unsigned)replayCount);
			PrintToConsole(line);
			PrintToConsole((s_mpReplayUnplayed == 0) ? "mp-replay: PLAYED" : "mp-replay: FAIL (commands left unreplayed)");
			return;
		}

		if (s_mpViewpoint) Skirmish_SetViewpoint(0);

		if (!MpHarness_ReplayPass(true, live, &liveCount, s_mpRecordFile[0] != '\0')) {
			PrintToConsole("mp-replay: FAIL (could not start a skirmish)");
			return;
		}

		commands = MpCommand_GetRecordCount();
		snprintf(line, sizeof(line), "mp-replay: recorded %u commands over %u ticks", (unsigned)commands, (unsigned)s_mpReplayTicks);
		PrintToConsole(line);

		if (commands == 0) {
			PrintToConsole("mp-replay: FAIL (the scripted player issued nothing, so nothing was tested)");
			return;
		}

		/* The producer half: hand the recording to a file and stop.  Whether the
		 * other process agrees is decided outside, by diffing the two logs --
		 * this one has no way to know and should not pretend to. */
		if (s_mpRecordFile[0] != '\0') {
			if (!MpCommand_SaveRecord(s_mpRecordFile, s_mpReplaySeed)) {
				PrintToConsole("mp-replay: FAIL (could not write the recording)");
				return;
			}

			snprintf(line, sizeof(line), "mp-replay: RECORDED %u commands, %u samples", (unsigned)commands, (unsigned)liveCount);
			PrintToConsole(line);
			return;
		}

		/* Second pass: same match, same process, no scripted player -- and, when
		 * asked, seen from the other house.  Two clients of one match differ in
		 * their viewpoint and in nothing else, so anything the simulation still
		 * reads out of g_playerHouseID shows up here and nowhere else. */
		if (s_mpViewpoint) Skirmish_SetViewpoint(1);

		if (!MpHarness_ReplayPass(false, replayed, &replayCount, false)) {
			PrintToConsole("mp-replay: FAIL (could not restart the skirmish)");
			return;
		}

		if (s_mpViewpoint) {
			MpHarness_ReportViewpoint(live, replayed, min(liveCount, replayCount));
			return;
		}

		for (i = 0; i < liveCount && i < replayCount; i++) {
			if (memcmp(&replayed[i], &live[i], sizeof(live[i])) == 0) continue;

			MpSync_Format(line, sizeof(line), (uint32)i * s_mpReplayStep, &live[i]);
			PrintToConsole(line);
			MpSync_Format(line, sizeof(line), (uint32)i * s_mpReplayStep, &replayed[i]);
			PrintToConsole(line);
			PrintToConsole("mp-replay: FAIL (the replay diverged -- something the player did never became a command)");
			return;
		}

		snprintf(line, sizeof(line), "mp-replay: %u samples matched, %u of %u commands replayed",
		         (unsigned)replayCount, (unsigned)(commands - s_mpReplayUnplayed), (unsigned)commands);
		PrintToConsole(line);
		PrintToConsole((s_mpReplayUnplayed == 0 && liveCount == replayCount) ? "mp-replay: PASS" : "mp-replay: FAIL (commands left unreplayed)");
		return;
	}

	if (s_skirmishSelfTest) {
		char line[512];
		char trace[544];
		uint32 tick;
		bool started;
		uint8 i;

		/* Drive the simulation by hand: the real loop is paced by the GUI, and
		 * a base takes minutes of wall clock to grow.  Advancing the game timer
		 * ourselves runs the same subsystems as fast as the CPU allows. */
		if (s_warPlay) {
			/* "--war=a,b --skirmish-self-test" is the headless twin of the match
			 * "--war=a,b" plays in the GUI: same shares, same seed, same map. */
			SkirmishEconomyPlan planA, planB;

			WarSearch_MakePlan(s_warPlayShare[0], s_warPlayShare[0], 0, &planA);
			WarSearch_MakePlan(s_warPlayShare[1], s_warPlayShare[1], 0, &planB);
			started = Skirmish_StartWar(s_skirmishHouse[0], s_skirmishHouse[1], s_warPlaySeed, &planA, &planB);
		} else {
			started = Skirmish_Start(s_skirmishHouse[0], s_skirmishHouse[1]);
		}

		if (!started) {
			PrintToConsole("skirmish-self-test: FAIL (could not start a skirmish)");
			return;
		}

		for (tick = 0; tick < s_skirmishSelfTestTicks; tick++) {
			Timer_AdvanceGame();

			GameLoop_Team();
			GameLoop_Unit();
			GameLoop_Structure();
			GameLoop_House();

			/* A trace rather than a verdict: base growth and losses only mean
			 * something as a curve. */
			if (tick != 0 && (tick % (s_skirmishSelfTestTicks / 5)) == 0) {
				for (i = 0; i < SKIRMISH_PLAYER_MAX; i++) {
					if (!Skirmish_GetSummary(i, line, sizeof(line))) continue;

					snprintf(trace, sizeof(trace), "t%u spice%u %s", (unsigned)tick, (unsigned)Skirmish_GetMapSpice(), line);
					PrintToConsole(trace);
				}
			}
		}

		for (i = 0; i < SKIRMISH_PLAYER_MAX; i++) {
			if (Skirmish_GetSummary(i, line, sizeof(line))) PrintToConsole(line);
			if (Skirmish_GetBuildOrder(i, line, sizeof(line))) PrintToConsole(line);
			if (Skirmish_GetTeams(i, line, sizeof(line))) PrintToConsole(line);
		}

		if (Skirmish_GetBystanders(line, sizeof(line))) PrintToConsole(line);
		if (Skirmish_GetCasualties(line, sizeof(line))) PrintToConsole(line);

		PrintToConsole("skirmish-self-test: DONE");
		return;
	}

	if (s_selectionSelfTest) {
		static const char *saves[] = { "_SAVE004.DAT", "_SAVE003.DAT", "_SAVE002.DAT", "_SAVE001.DAT", "_SAVE000.DAT" };
		uint16 tested = 0;
		uint16 i;

		for (i = 0; i < lengthof(saves); i++) {
			int result;

			if (!SaveGame_LoadFile(saves[i])) continue;
			g_selectionType = SELECTIONTYPE_UNIT;
			g_selectionTypeNew = SELECTIONTYPE_UNIT;
			result = UnitSelection_RunRegressionTest();
			if (result == 0) {
				s_selectionSelfTestResult = 0;
				break;
			}
			if (result == 1) tested++;
		}
		if (tested != 0 && s_selectionSelfTestResult != 0) s_selectionSelfTestResult = 1;
		if (s_selectionSelfTestResult == 1) {
			char message[64];

			snprintf(message, sizeof(message), "selection-self-test: PASS (%u saves)", tested);
			PrintToConsole(message);
		} else if (s_selectionSelfTestResult == -1) {
			PrintToConsole("selection-self-test: SKIP (no save with two movable combat units)");
		} else {
			PrintToConsole("selection-self-test: FAIL");
		}
		return;
	}

	/* Let players skip the intro immediately, including on their first launch. */
	g_canSkipIntro = true;

	/* --skirmish drops straight into a match: the mode has no menu entry, it
	 * is a development tool rather than something to play. */
	if (s_skirmishDirect || s_ecoPlay || s_warPlay) g_gameMode = GM_SKIRMISH;

	for (;; sleepIdle()) {
		if (g_gameMode == GM_MENU) {
			GameLoop_GameIntroAnimationMenu();

			if (!g_running) break;
			if (g_gameMode == GM_MENU) continue;

			GUI_Mouse_Hide_Safe();

			GUI_DrawFilledRectangle(g_curWidgetXBase << 3, g_curWidgetYBase, (g_curWidgetXBase + g_curWidgetWidth) << 3, g_curWidgetYBase + g_curWidgetHeight, 12);

			Input_History_Clear();

			if (s_enableLog != 0) Mouse_SetMouseMode((uint8)s_enableLog, "DUNE.LOG");

			GFX_SetPalette(g_palette1);

			GUI_Mouse_Show_Safe();
		}

		if (g_gameMode == GM_PICKHOUSE) {
			Music_Play(28);

			g_playerHouseID = HOUSE_MERCENARY;
			g_playerHouseID = GUI_PickHouse();

			GUI_Mouse_Hide_Safe();

			GFX_ClearBlock(SCREEN_0);

			Sprites_LoadTiles();

			GUI_Palette_CreateRemap(g_playerHouseID);

			Voice_LoadVoices(g_playerHouseID);

			GUI_Mouse_Show_Safe();

			g_gameMode = GM_RESTART;
			g_scenarioID = 1;
			g_campaignID = 0;
			g_strategicRegionBits = 0;
		}

		if (g_gameMode == GM_SKIRMISH) {
			bool started;

			g_playerHouseID = HOUSE_MERCENARY;

			GUI_Mouse_Hide_Safe();

			GFX_ClearBlock(SCREEN_0);

			GUI_Palette_CreateRemap(g_playerHouseID);
			Voice_LoadVoices(g_playerHouseID);

			GUI_Mouse_Show_Safe();

			GUI_ChangeSelectionType(SELECTIONTYPE_MENTAT);

			/* --economy-play watches one cell of the economy grid: a single house,
			 * no enemy, no combat units, on the map the search scored it on. */
			if (s_ecoPlay) {
				SkirmishEconomyPlan plan;

				EcoSearch_MakePlan(s_ecoPlayRefineries, s_ecoPlayHarvesters, s_ecoPlayCarryalls, s_ecoPlayWait, s_ecoPlayCarryallWait, &plan);
				started = Skirmish_StartEconomy(s_skirmishHouse[0], s_ecoPlaySeed, &plan);
			} else if (s_warPlay) {
				/* --war watches one cell of the matrix: two tuned economies, each
				 * spending its own share of the take on the army. */
				SkirmishEconomyPlan planA, planB;

				WarSearch_MakePlan(s_warPlayShare[0], s_warPlayShare[0], 0, &planA);
				WarSearch_MakePlan(s_warPlayShare[1], s_warPlayShare[1], 0, &planB);
				started = Skirmish_StartWar(s_skirmishHouse[0], s_skirmishHouse[1], s_warPlaySeed, &planA, &planB);
			} else if (MpGame_IsLive()) {
				/* Two processes, one match, seen from two chairs -- the same
				 * thing --mp-turnloop proves headless, with the drawing left in. */
				started = MpGame_Begin();
			} else {
				started = Skirmish_Start(s_skirmishHouse[0], s_skirmishHouse[1]);
			}

			if (!started) {
				g_gameMode = GM_MENU;

				/* The flag is a test, and a test that goes back to a menu
				 * nobody is sitting at spins for ever. */
				if (s_lobbyPlay) {
					PrintToConsole("lobby-play: FAIL (the match did not begin)");
					g_running = false;
				}
			} else {
				g_gameMode = GM_NORMAL;

				GUI_ChangeSelectionType(SELECTIONTYPE_STRUCTURE);

				Music_Play(Tools_RandomUI_Range(0, 8) + 8);
				l_timerNext = g_timerGUI + 300;
			}
		}

		if (g_selectionTypeNew != g_selectionType) {
			GUI_ChangeSelectionType(g_selectionTypeNew);
		}

		MpPurity_Begin(1);
		GUI_PaletteAnimate();
		MpPurity_End(1, "GUI_PaletteAnimate", g_timerGame);

		if (g_gameMode == GM_RESTART) {
			GUI_ChangeSelectionType(SELECTIONTYPE_MENTAT);

			Game_LoadScenario(g_playerHouseID, g_scenarioID);
			if (!g_debugScenario && !g_debugSkipDialogs) {
				GUI_Mentat_ShowBriefing();
			} else {
				Debug("Skipping GUI_Mentat_ShowBriefing()\n");
			}

			g_gameMode = GM_NORMAL;

			GUI_ChangeSelectionType(g_debugScenario ? SELECTIONTYPE_DEBUG : SELECTIONTYPE_STRUCTURE);

			Music_Play(Tools_RandomUI_Range(0, 8) + 8);
			l_timerNext = g_timerGUI + 300;
		}

		if (l_selectionState != g_selectionState) {
			MpPurity_Begin(1);
			Map_SetSelectionObjectPosition(0xFFFF);
			Map_SetSelectionObjectPosition(g_selectionRectanglePosition);
			MpPurity_End(1, "Map_SetSelectionObjectPosition", g_timerGame);
			l_selectionState = g_selectionState;
		}

		if (!Driver_Voice_IsPlaying() && !Sound_StartSpeech()) {
			if (g_gameConfig.music == 0) {
				Music_Play(2);

				g_musicInBattle = 0;
			} else if (g_musicInBattle > 0) {
				Music_Play(Tools_RandomUI_Range(0, 5) + 17);
				l_timerNext = g_timerGUI + 300;
				g_musicInBattle = -1;
			} else {
				g_musicInBattle = 0;
				if (g_enableSoundMusic != 0 && g_timerGUI > l_timerNext) {
					if (!Driver_Music_IsPlaying()) {
						Music_Play(Tools_RandomUI_Range(0, 8) + 8);
						l_timerNext = g_timerGUI + 300;
					}
				}
			}
		}

		GFX_Screen_SetActive(SCREEN_0);

		/* Input is allowed to change the world only through a command, so the
		 * same clamp applies: whatever a click does directly here is something
		 * the other player will never hear about. */
		MpPurity_Begin(1);
		key = GUI_Widget_HandleEvents(g_widgetLinkedListHead);
		MpPurity_End(1, "GUI_Widget_HandleEvents", g_timerGame);

		MpPurity_Begin(1);
		GUI_Widget_Viewport_HandleEdgeScroll();
		MpPurity_End(1, "GUI_Widget_Viewport_HandleEdgeScroll", g_timerGame);
		/* Group buttons have no widget shortcuts, so handle the physical M/A
		 * keys here.  Single-unit widgets may consume the same key first; in
		 * that case they have already switched out of UNIT and this is a no-op. */
		if ((((key & 0x7FFF) == GUI_Widget_GetShortcut('M')) || Input_Test(GUI_Widget_GetShortcut('M')) != 0) && InGame_BeginSelectedAction(ACTION_MOVE)) {
			key = 0;
		}
		if ((((key & 0x7FFF) == GUI_Widget_GetShortcut('A')) || Input_Test(GUI_Widget_GetShortcut('A')) != 0) && InGame_BeginSelectedAction(ACTION_ATTACK)) {
			key = 0;
		}
		/* Tab walks the spectator camera from one AI base to the next.  A match
		 * opens on the first one and the other is in the opposite corner of a
		 * 62x62 map, which at the start is one Construction Yard and reads as
		 * "there is only one AI on the map".  0x10 is Tab in OpenDUNE's internal
		 * scancode set (s_keymapNormal[] in input.c), not the PC set's 0x0F. */
		if ((key & 0x7FFF) == 0x0010 && Skirmish_IsActive()) {
			uint8 tried;

			for (tried = 0; tried < SKIRMISH_PLAYER_MAX; tried++) {
				uint16 origin;

				s_skirmishCameraBase = (uint8)((s_skirmishCameraBase + 1) % SKIRMISH_PLAYER_MAX);
				origin = Skirmish_GetBaseOrigin(s_skirmishCameraBase);
				if (origin == 0xFFFF) continue;

				Map_SetViewportPosition(origin);
				g_minimapPosition = g_viewportPosition;
				break;
			}
			key = 0;
		}

		/* [ and ] step the simulation speed.  Event driven rather than polled:
		 * Input_Test() is true for as long as the key is held, which would run
		 * through the whole range in a few frames.
		 *
		 * The codes are OpenDUNE's own, not the PC set: SDL hands the video layer
		 * a PC scancode, s_keyTranslate[] turns it into the internal one, and the
		 * two differ here.  s_keymapNormal[] in input.c is the table to read them
		 * off -- index 0x1B is '[' and 0x1C is ']'.  Bound to the PC codes
		 * instead, '[' ran the speed up and ']' did nothing at all. */
		if ((key & 0x7FFF) == 0x001B) {
			GameLoop_StepSpeed(-1);
			key = 0;
		}
		if ((key & 0x7FFF) == 0x001C) {
			GameLoop_StepSpeed(1);
			key = 0;
		}

		/* T = Hunt.  Polling the physical key also covers keys consumed by a
		 * sidebar widget before this general in-game shortcut sees them. */
		if (((key & 0x7FFF) == 0x0015 || Input_Test(0x15) != 0) && g_selectionType == SELECTIONTYPE_UNIT && g_unitSelectionCount != 0) {
			UnitSelection_OrderHunt();
			key = 0;
		}
		/* Y = Air Transit: choose a landing tile for the selected ground units. */
		if (((key & 0x7FFF) == 0x0016 || Input_Test(0x16) != 0) && g_selectionType == SELECTIONTYPE_UNIT && g_unitSelectionCount != 0) {
			if (UnitSelection_BeginAirTransit()) {
				g_unitActive = g_unitSelected;
				g_activeAction = ACTION_MOVE;
				GUI_ChangeSelectionType(SELECTIONTYPE_TARGET);
			}
			key = 0;
		}

		/* Control groups, StarCraft style: Ctrl or Option with a digit binds the
		 * current selection, the bare digit brings it back.  Command is not an
		 * option here - Video_Key_Callback() drops every event carrying it so
		 * macOS keeps Command-Tab and friends. */
		if (g_selectionType == SELECTIONTYPE_UNIT || g_selectionType == SELECTIONTYPE_STRUCTURE) {
			uint16 digit = 0xFFFF;
			uint16 i;

			for (i = 0; i < 10; i++) {
				uint8 code = (i == 0) ? 0x0B : (uint8)(0x01 + i);

				if ((key & 0x7FFF) == code || Input_Test(code) != 0) {
					digit = i;
					break;
				}
			}

			/* Act once per press: the polled key state stays set while held. */
			if (digit != 0xFFFF && digit != l_lastGroupDigit) {
				if (Input_Test(0x1d) != 0 || Input_Test(0x38) != 0) {
					UnitSelection_AssignControlGroup(digit);
				} else {
					UnitSelection_RecallControlGroup(digit);
				}
				key = 0;
			}
			l_lastGroupDigit = digit;
		}

		if (g_selectionType == SELECTIONTYPE_TARGET || g_selectionType == SELECTIONTYPE_PLACE || g_selectionType == SELECTIONTYPE_UNIT || g_selectionType == SELECTIONTYPE_STRUCTURE) {
			if (g_unitSelected != NULL) {
				if (l_timerUnitStatus < g_timerGame) {
					MpPurity_Begin(1);
					Unit_DisplayStatusText(g_unitSelected);
					MpPurity_End(1, "Unit_DisplayStatusText", g_timerGame);
					l_timerUnitStatus = g_timerGame + 300;
				}

				if (g_selectionType != SELECTIONTYPE_TARGET) {
					g_selectionPosition = Tile_PackTile(Tile_Center(g_unitSelected->o.position));
				}
			}

			MpPurity_Begin(1);
			GUI_Widget_ActionPanel_Draw(false);
			MpPurity_End(1, "GUI_Widget_ActionPanel_Draw", g_timerGame);

			MpPurity_Begin(1);
			InGame_Numpad_Move(key);
			MpPurity_End(1, "InGame_Numpad_Move", g_timerGame);

			MpPurity_Begin(1);
			GUI_DrawCredits(g_playerHouseID, 0);
			MpPurity_End(1, "GUI_DrawCredits", g_timerGame);

			if (MpTurn_IsActive()) {
				/* A networked match runs on the turn loop's clock, and the speed
				 * keys do not apply to it: how fast the world runs is not a thing
				 * one player gets to decide. */
				MpGame_Step();
			} else {
				/* Every step above x1 is another sequential simulation tick.
				 * Advancing the game timer between passes keeps every timer-driven
				 * system in step, rather than speeding up selected subsystems. */
				uint16 passes = GameLoop_GetSpeedFactor();
				uint16 pass;

				for (pass = 0; pass < passes; pass++) {
					if (pass != 0) Timer_AdvanceGame();

					GameLoop_Team();
					GameLoop_Unit();
					GameLoop_Structure();
					GameLoop_House();
				}
			}

			/* Drawing is not allowed to change the world.  See mpsync.c. */
			MpPurity_Begin(1);
			GUI_DrawScreen(SCREEN_0);
			MpPurity_End(1, "GUI_DrawScreen", g_timerGame);
		}

		MpPurity_Begin(1);
		GUI_DisplayText(NULL, 0);
		MpPurity_End(1, "GUI_DisplayText", g_timerGame);

		if (g_running && !g_debugScenario) {
			GameLoop_LevelEnd();
		}

		if (!g_running) break;
	}

	GUI_Mouse_Hide_Safe();

	if (s_enableLog != 0) Mouse_SetMouseMode(INPUT_MOUSE_MODE_NORMAL, "DUNE.LOG");

	/* The original DOS-style dissolve writes random 8-pixel strips while the
	 * desktop fullscreen Space is being torn down.  macOS can snapshot that
	 * intermediate frame, leaving a ghost image after the process has exited.
	 * Present one complete black frame instead of animating the final teardown. */
	Widget_SetCurrentWidget(0);
	GFX_ClearScreen(SCREEN_0);
	Video_Tick();
}

/**
 * Initialize Timer, Video, Mouse, GFX, Fonts, Random number generator
 * and current Widget
 */
static bool OpenDune_Init(int screen_magnification, VideoScaleFilter filter, int frame_rate)
{
	if (!Font_Init()) {
		Error(
			"--------------------------\n"
			"ERROR LOADING DATA FILE\n"
			"\n"
			"Did you copy the Dune2 1.07eu data files into the data directory ?\n"
			"\n"
		);

		return false;
	}

	Timer_Init();

	if (!Video_Init(screen_magnification, filter)) return false;

	Mouse_Init();

	/* Add the general tickers */
	Timer_Add(Timer_Tick, 1000000 / 60, false);
	Timer_Add(Video_Tick, 1000000 / frame_rate, true);

	g_mouseDisabled = -1;

	GFX_Init();
	GFX_ClearScreen(SCREEN_ACTIVE);

	Font_Select(g_fontNew8p);

	g_palette_998A = calloc(256 * 3, sizeof(uint8));

	memset(&g_palette_998A[45], 63, 3);	/* Set color 15 to WHITE */

	GFX_SetPalette(g_palette_998A);

	/* The simulation stream is reseeded per match by Skirmish_StartInternal();
	 * this is only so it is never unseeded in the campaign, which has no match
	 * seed of its own.  The presentation stream is never reseeded and never
	 * needs to be -- nothing it feeds is compared between machines. */
	Tools_RandomLCG_Seed((unsigned)time(NULL));
	Tools_RandomUI_Seed((unsigned)time(NULL) + 1);

	Widget_SetCurrentWidget(0);

	return true;
}

/**
 * Print a IBM 437 encoded string to the system console,
 * performing conversion to the system charset.
 *
 * @param str IBM 437 code page encoded character string
 */
/** The clamp reports through here, because the console printer is local. */
void MpPurity_Report(const char *line)
{
	PrintToConsole(line);
}

static void PrintToConsole(const char * str)
{
#if defined(TOS) || defined(DOS)
	/* directly output the IBM437 string */
	puts(str);
#else
	static const unsigned char cp437toLatin1[] = {
		0xC7, 0xFC, 0xE9, 0xE2, 0xE4, 0xE0, 0xE5, 0xE7,	/* 0x80 - 0x87 */
		0xEA, 0xEB, 0xE8, 0xEF, 0xEE, 0xEC, 0xC4, 0xC5,	/* 0x88 - 0x8f */
		0xC9, 0xE6, 0xC6, 0xF4, 0xF6, 0xF2, 0xFB, 0xF9,	/* 0x90 - 0x97 */
		0xFF, 0xD6, 0xDC, 0xA2, 0xA3, 0xA5				/* 0x98 - 0x9d */
	};
	int utf8_output;
	char * LANG = getenv("LANG");

	utf8_output = (LANG != NULL && strstr(LANG, "UTF-8") != NULL);
	while (*str) {
		unsigned char c = *str++;	/* IBM 437 code page character */

		if (c & 0x80) {
			if ((c & 0x7f) < sizeof(cp437toLatin1)) {
				c = cp437toLatin1[c & 0x7f];
			}
			if (utf8_output) {
				putchar(0xc0 | (c >> 6));
				putchar(0x80 | (c & 0x3f));
			} else {
				putchar(c);
			}
		} else {
			putchar(c);
		}
	}
	putchar('\n');
#endif
	/* A match that is watched rather than measured ends by somebody closing the
	 * window, and a block buffered stdout would take the whole log with it. */
	fflush(stdout);
}

#ifdef TOS
void exit_handler(void)
{
	PrintToConsole("Press any key to quit.");
	(void)Cnecin();
}
#endif /* TOS */

#if defined(__APPLE__) && defined(SDL_MAJOR_VERSION) && (SDL_MAJOR_VERSION == 1)
int SDL_main(int argc, char **argv)
#else
int main(int argc, char **argv)
#endif /* __APPLE__ */
{
	bool commit_dune_cfg = false;
	VideoScaleFilter scale_filter = FILTER_NEAREST_NEIGHBOR;
	int scaling_factor = 2;
	int frame_rate = 60;
	char filter_text[64];
#if defined(_WIN32)
	#if defined(__MINGW32__) && defined(__STRICT_ANSI__)
	#if 0 /* NOTE : disabled because it generates warnings when cross compiling
	       * for MinGW32 under linux */
		int __cdecl __MINGW_NOTHROW _fileno (FILE*);
	#endif
	#endif
	FILE *err = fopen("error.log", "w");
	FILE *out = fopen("output.log", "w");

	#if defined(_MSC_VER)
		_CrtSetDbgFlag( _CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF );
	#endif

	if (err != NULL) _dup2(_fileno(err), _fileno(stderr));
	if (out != NULL) _dup2(_fileno(out), _fileno(stdout));
	FreeConsole();
#endif /* _WIN32 */
#ifdef TOS
	(void)Cconws(window_caption);
	(void)Cconws("\r\nrevision:   ");
	(void)Cconws(g_opendune_revision);
	(void)Cconws("\r\nbuild date: ");
	(void)Cconws(g_opendune_build_date);
	(void)Cconws("\r\n");
	/* open log files and set buffering mode */
	g_errlog = fopen("error.log", "w");
	if(g_errlog != NULL) setvbuf(g_errlog, NULL, _IONBF, 0);
#ifdef _DEBUG
	g_outlog = fopen("output.log", "w");
	if(g_outlog != NULL) setvbuf(g_outlog, NULL, _IOLBF, 0);
#endif
	if(atexit(exit_handler) != 0) {
		Error("atexit() failed\n");
	}
#endif /* TOS */
#ifdef DOS
	/* open log files and set buffering mode */
	g_errlog = fopen("error.log", "w");
	if(g_errlog != NULL) setvbuf(g_errlog, NULL, _IONBF, 0);
#ifdef _DEBUG
	g_outlog = fopen("output.log", "w");
	if(g_outlog != NULL) setvbuf(g_outlog, NULL, _IOLBF, 0);
#endif
#endif /* DOS */
	CrashLog_Init();

	{
		int i;
		for (i = 1; i < argc; i++) {
			if (strcmp(argv[i], "--selection-self-test") == 0) s_selectionSelfTest = true;
			if (strcmp(argv[i], "--build-rules-self-test") == 0) s_buildRulesSelfTest = true;
			if (strcmp(argv[i], "--move-rules-self-test") == 0) s_moveRulesSelfTest = true;
			if (strcmp(argv[i], "--lobby-self-test") == 0) s_lobbySelfTest = true;
			if (strncmp(argv[i], "--lobby-play=", 13) == 0) {
				/* relay,code,pair,slot -- the four things the lobby's rows set. */
				unsigned pair = 0, slot = 1;

				s_lobbyPlayRelay[0] = '\0';
				s_lobbyPlayCode[0] = '\0';
				sscanf(argv[i] + 13, "%127[^,],%31[^,],%u,%u", s_lobbyPlayRelay, s_lobbyPlayCode, &pair, &slot);
				s_lobbyPlayPair = (uint8)(pair % 6);
				s_lobbyPlaySlot = (uint8)((slot >= 2) ? 1 : 0);
				s_lobbyPlay = (s_lobbyPlayRelay[0] != '\0' && s_lobbyPlayCode[0] != '\0');
			}
			if (strcmp(argv[i], "--pathfinder-self-test") == 0) s_pathfinderSelfTest = true;
			if (strcmp(argv[i], "--combat-balance-self-test") == 0) s_combatBalanceSelfTest = true;
			if (strcmp(argv[i], "--economy-trace") == 0) EcoSearch_SetTrace(true);
			/* The A/B switch for the route search.  It has to be a flag and not
			 * only the ini key, because opendune.ini is searched in the user's
			 * Application Support directory first: a copy there shadows anything
			 * put next to the binary, so on a machine that has one there is no
			 * way to change this key alone without editing the player's file. */
			if (strncmp(argv[i], "--pathfinder=", 13) == 0) s_pathfinderOverride = (atoi(argv[i] + 13) != 0) ? 1 : 0;
			/* "--doctrine=B" for both sides, "--doctrine=A,B" to play one against
			 * the other on the same map: the only honest test of a battle
			 * strategy is the strategy it replaces, on the same seed. */
			if (strncmp(argv[i], "--doctrine=", 11) == 0) {
				if (!Doctrine_ParseArgument(argv[i] + 11)) {
					Warning("--doctrine expects doctrine names, e.g. --doctrine=A,B\n");
				}
			}
			if (strncmp(argv[i], "--economy-play", 14) == 0) {
				s_ecoPlay = true;
				if (argv[i][14] == '=') sscanf(argv[i] + 15, "%hu,%hu,%hu,%hu,%hu,%u", &s_ecoPlayRefineries, &s_ecoPlayHarvesters, &s_ecoPlayCarryalls, &s_ecoPlayWait, &s_ecoPlayCarryallWait, &s_ecoPlaySeed);
			}
			if (strncmp(argv[i], "--economy-carryall", 18) == 0) {
				s_ecoCarryall = true;
				if (argv[i][18] == '=') sscanf(argv[i] + 19, "%u,%hu", &s_ecoTicks, &s_ecoMaps);
			}
			if (strncmp(argv[i], "--economy-queue", 15) == 0) {
				s_ecoQueue = true;
				if (argv[i][15] == '=') sscanf(argv[i] + 16, "%u,%hu", &s_ecoTicks, &s_ecoMaps);
			}
			if (strncmp(argv[i], "--economy-grid", 14) == 0) {
				s_ecoGrid = true;
				if (argv[i][14] == '=') sscanf(argv[i] + 15, "%u,%hu", &s_ecoTicks, &s_ecoMaps);
			}
			if (strncmp(argv[i], "--economy-baseline", 18) == 0) {
				s_ecoBaseline = true;
				if (argv[i][18] == '=') sscanf(argv[i] + 19, "%u,%hu", &s_ecoTicks, &s_ecoMaps);
			} else if (strncmp(argv[i], "--economy-search", 16) == 0) {
				s_ecoSearch = true;
				if (argv[i][16] == '=') sscanf(argv[i] + 17, "%hu,%hu,%u,%hu", &s_ecoPopulation, &s_ecoGenerations, &s_ecoTicks, &s_ecoMaps);
			}
			/* Chained, because "--war" is a prefix of every other war flag: a
			 * separate test for --war-trace also started a match. */
			if (strcmp(argv[i], "--war-trace") == 0) {
				WarSearch_SetTrace(true);
			} else if (strncmp(argv[i], "--war-matrix", 12) == 0) {
				s_warMatrix = true;
				if (argv[i][12] == '=') sscanf(argv[i] + 13, "%u,%hu", &s_warTicks, &s_warMaps);
			} else if (strncmp(argv[i], "--war-timing", 12) == 0) {
				s_warTiming = true;
				if (argv[i][12] == '=') sscanf(argv[i] + 13, "%u,%hu", &s_warTicks, &s_warMaps);
			} else if (strncmp(argv[i], "--war-telemetry", 15) == 0) {
				s_warTelemetry = true;
				if (argv[i][15] == '=') sscanf(argv[i] + 16, "%u,%hu,%u", &s_warTicks, &s_warTelemetryStep, &s_warPlaySeed);
			} else if (strncmp(argv[i], "--war-metrics", 13) == 0) {
				s_warMetrics = true;
				if (argv[i][13] == '=') sscanf(argv[i] + 14, "%u,%hu", &s_warTicks, &s_warMaps);
			} else if (strncmp(argv[i], "--war-ladder", 12) == 0) {
				s_warLadder = true;
				if (argv[i][12] == '=') sscanf(argv[i] + 13, "%u,%hu", &s_warTicks, &s_warMaps);
			} else if (strncmp(argv[i], "--war", 5) == 0) {
				s_warPlay = true;
				if (argv[i][5] == '=') sscanf(argv[i] + 6, "%hu,%hu,%u", &s_warPlayShare[0], &s_warPlayShare[1], &s_warPlaySeed);
			}
			if (strncmp(argv[i], "--mp-checksum", 13) == 0) {
				s_mpChecksum = true;
				if (argv[i][13] == '=') sscanf(argv[i] + 14, "%u,%u,%u", &s_mpChecksumTicks, &s_mpChecksumStep, &s_mpChecksumSeed);
			} else if (strncmp(argv[i], "--mp-record=", 12) == 0) {
				snprintf(s_mpRecordFile, sizeof(s_mpRecordFile), "%s", argv[i] + 12);
			} else if (strncmp(argv[i], "--mp-play=", 10) == 0) {
				snprintf(s_mpPlayFile, sizeof(s_mpPlayFile), "%s", argv[i] + 10);
			} else if (strncmp(argv[i], "--mp-turnloop", 13) == 0) {
				s_mpTurnLoop = true;
				if (argv[i][13] == '=') sscanf(argv[i] + 14, "%u,%u,%u", &s_mpReplayTicks, &s_mpReplayStep, &s_mpReplaySeed);
			} else if (strncmp(argv[i], "--mp-turn=", 10) == 0) {
				/* Turn length in ticks and turn delay in turns: the two numbers
				 * the whole latency argument in mp.md is about. */
				unsigned length = 0, delay = 0;

				sscanf(argv[i] + 10, "%u,%u", &length, &delay);
				if (length != 0) s_mpTurnLength = (uint16)length;
				if (delay  != 0) s_mpTurnDelay  = (uint8)delay;
			} else if (strncmp(argv[i], "--mp-seed=", 10) == 0) {
				/* Which map the match is played on.  Both players must name the
				 * same one; distributing it is the lobby's job, and there is no
				 * lobby yet. */
				unsigned seed = 0;

				sscanf(argv[i] + 10, "%u", &seed);
				if (seed != 0) s_mpLiveSeed = seed;
			} else if (strncmp(argv[i], "--speed=", 8) == 0) {
				/* The same knob as the [ and ] keys, set at start so a test does
				 * not begin by pressing it twice in each window. */
				unsigned speed = 0;

				sscanf(argv[i] + 8, "%u", &speed);
				while (speed > 1 && s_gameSpeedFactor < speed) GameLoop_StepSpeed(1);
			} else if (strncmp(argv[i], "--mp-units=", 11) == 0) {
				/* How many units each side starts with, so a test has something
				 * to order about before the first building is up. */
				unsigned units = 0;

				sscanf(argv[i] + 11, "%u", &units);
				s_mpLiveUnits = (uint16)units;
			} else if (strncmp(argv[i], "--sim-purity", 12) == 0) {
				/* Checksum the world around drawing and around input, and name
				 * whatever changed it.  One machine, no network: the question
				 * "did that function change the world" does not need two. */
				MpPurity_SetEnabled(true);
				if (strcmp(argv[i], "--sim-purity=dump") == 0) MpPurity_SetDump(true);
			} else if (strcmp(argv[i], "--mp-desync-dump") == 0) {
				/* Keep the last few turns of state on disk so that when the two
				 * players disagree about turn N, both still have their own turn
				 * N to compare.  The mismatch is always noticed after the fact;
				 * this is what stops the evidence from being gone by then. */
				s_mpDesyncDump = true;
			} else if (strncmp(argv[i], "--mp-dump=", 10) == 0) {
				/* Write every chunk to a file at one tick, so two clients that
				 * disagree can be compared byte for byte. */
				unsigned tick = 0;

				sscanf(argv[i] + 10, "%u", &tick);
				s_mpLiveDumpTick = tick;
			} else if (strncmp(argv[i], "--mp-sample=", 12) == 0) {
				/* How often a live match prints a checksum.  Closer together
				 * when hunting a desync, because the log has to bracket it. */
				unsigned step = 0;

				sscanf(argv[i] + 12, "%u", &step);
				if (step != 0) s_mpLiveSampleStep = step;
			} else if (strncmp(argv[i], "--mp-wait=", 10) == 0) {
				/* How long either transport waits for a packet that has not
				 * come, and how long the lobby waits for the second player. */
				unsigned wait = 0;

				sscanf(argv[i] + 10, "%u", &wait);
				if (wait != 0) s_mpNetWaitMs = wait;
			} else if (strncmp(argv[i], "--mp-relay=", 11) == 0) {
				/* host[:port][,room[,slot]] -- where the relay is, and which
				 * match to join once we get there. */
				unsigned port = 0, slot = 0;
				char host[128];
				char room[64];

				host[0] = '\0';
				room[0] = '\0';
				if (sscanf(argv[i] + 11, "%127[^:,]:%u,%63[^,],%u", host, &port, room, &slot) < 2) {
					host[0] = '\0';
					room[0] = '\0';
					sscanf(argv[i] + 11, "%127[^,],%63[^,],%u", host, room, &slot);
				}
				if (host[0] != '\0') snprintf(s_mpRelayHost, sizeof(s_mpRelayHost), "%s", host);
				if (room[0] != '\0') snprintf(s_mpRelayRoom, sizeof(s_mpRelayRoom), "%s", room);
				if (port != 0) s_mpRelayPort = (uint16)port;
				if (slot >= 1 && slot <= MATCH_SLOT_MAX) s_mpTurnSlot = (uint8)(slot - 1);
			} else if (strncmp(argv[i], "--mp-realtime", 13) == 0) {
				s_mpRealtime = true;
			} else if (strncmp(argv[i], "--mp-lag=", 9) == 0) {
				s_mpNetLagMs = (uint32)atoi(argv[i] + 9);
			} else if (strncmp(argv[i], "--mp-net=", 9) == 0) {
				/* slot[,directory[,waitms]] -- which player this process is, and
				 * where the two of them leave each other packets. */
				unsigned slot = 0, wait = 0;
				char directory[256];

				directory[0] = '\0';
				sscanf(argv[i] + 9, "%u,%255[^,],%u", &slot, directory, &wait);
				if (slot >= 1 && slot <= MATCH_SLOT_MAX) s_mpTurnSlot = (uint8)(slot - 1);
				if (directory[0] != '\0') snprintf(s_mpNetDirectory, sizeof(s_mpNetDirectory), "%s", directory);
				if (wait != 0) s_mpNetWaitMs = wait;
			} else if (strncmp(argv[i], "--mp-viewpoint", 14) == 0) {
				s_mpReplay = true;
				s_mpViewpoint = true;
				if (argv[i][14] == '=') sscanf(argv[i] + 15, "%u,%u,%u", &s_mpReplayTicks, &s_mpReplayStep, &s_mpReplaySeed);
			} else if (strncmp(argv[i], "--mp-modal-dump=", 16) == 0) {
				sscanf(argv[i] + 16, "%u", &s_mpModalDumpTick);
			} else if (strncmp(argv[i], "--mp-modal", 10) == 0) {
				s_mpModal = true;
				if (argv[i][10] == '=') sscanf(argv[i] + 11, "%u,%u,%u", &s_mpReplayTicks, &s_mpReplayStep, &s_mpReplaySeed);
			} else if (strncmp(argv[i], "--mp-replay", 11) == 0) {
				s_mpReplay = true;
				if (argv[i][11] == '=') sscanf(argv[i] + 12, "%u,%u,%u", &s_mpReplayTicks, &s_mpReplayStep, &s_mpReplaySeed);
			}
			if (strncmp(argv[i], "--human=", 8) == 0) {
				/* Which skirmish slots a person plays, counted the way a person
				 * counts players: --human=1, or --human=1,2 for both. */
				unsigned a = 0, b = 0;

				sscanf(argv[i] + 8, "%u,%u", &a, &b);
				if (a >= 1 && a <= MATCH_SLOT_MAX) Skirmish_SetController((uint8)(a - 1), MATCH_CONTROLLER_HUMAN_LOCAL);
				if (b >= 1 && b <= MATCH_SLOT_MAX) Skirmish_SetController((uint8)(b - 1), MATCH_CONTROLLER_HUMAN_LOCAL);
			}
			if (strncmp(argv[i], "--viewpoint=", 12) == 0) {
				unsigned over = 0;

				sscanf(argv[i] + 12, "%u", &over);
				if (over >= 1 && over <= MATCH_SLOT_MAX) s_mpViewpointOverride = (uint8)over;
			}
			if (strncmp(argv[i], "--viewpoint=", 12) == 0) {
				/* Whose screen this process is.  The one thing two clients of the
				 * same match do not share, so the one thing worth varying between
				 * two otherwise identical runs. */
				unsigned v = 0;

				sscanf(argv[i] + 12, "%u", &v);
				if (v >= 1 && v <= MATCH_SLOT_MAX) Skirmish_SetViewpoint((uint8)(v - 1));
			}
			if (strncmp(argv[i], "--skirmish-self-test", 20) == 0) {
				s_skirmishSelfTest = true;
				if (argv[i][20] == '=') s_skirmishSelfTestTicks = (uint32)atoi(argv[i] + 21);
			} else if (strncmp(argv[i], "--skirmish", 10) == 0) {
				s_skirmishDirect = true;
				if (argv[i][10] == '=') GameLoop_SkirmishParseHouses(argv[i] + 11);
			}
		}
	}

	/* Load opendune.ini file */
	Load_IniFile();

	/* set globals according to opendune.ini */
	g_dune2_enhanced = (IniFile_GetInteger("dune2_enhanced", 1) != 0) ? true : false;
	g_debugGame = (IniFile_GetInteger("debug_game", 0) != 0) ? true : false;
	g_debugScenario = (IniFile_GetInteger("debug_scenario", 0) != 0) ? true : false;
	g_debugSkipDialogs = (IniFile_GetInteger("debug_skip_dialogs", 0) != 0) ? true : false;
	s_enableLog = (uint8)IniFile_GetInteger("debug_log_game", 0);
	g_starPortEnforceUnitLimit = (IniFile_GetInteger("startport_unit_cap", 0) != 0) ? true : false;

	Debug("Globals :\n");
	Debug("  g_dune2_enhanced = %d\n", (int)g_dune2_enhanced);
	Debug("  g_debugGame = %d\n", (int)g_debugGame);
	Debug("  g_debugScenario = %d\n", (int)g_debugScenario);
	Debug("  g_debugSkipDialogs = %d\n", (int)g_debugSkipDialogs);
	Debug("  s_enableLog = %d\n", (int)s_enableLog);
	Debug("  g_starPortEnforceUnitLimit = %d\n", (int)g_starPortEnforceUnitLimit);

	if (!File_Init()) {
		return 1;
	}

	/* Loading config from dune.cfg */
	if (!Config_Read("dune.cfg", &g_config)) {
		Config_Default(&g_config);
		commit_dune_cfg = true;
	}
	/* reading config from opendune.ini which prevail over dune.cfg */
	SetLanguage_From_IniFile(&g_config);

	/* Writing config to dune.cfg */
	if (commit_dune_cfg && !Config_Write("dune.cfg", &g_config)) {
		Error("Error writing to dune.cfg file.\n");
		return 1;
	}

	Input_Init();
	/* if no mouse is detected, we should Input_Flags_SetBits(INPUT_FLAG_MOUSE_EMUL) */

	Drivers_All_Init();

	scaling_factor = IniFile_GetInteger("scalefactor", 2);
	if (IniFile_GetString("scalefilter", NULL, filter_text, sizeof(filter_text)) != NULL) {
		if (strcasecmp(filter_text, "nearest") == 0) {
			scale_filter = FILTER_NEAREST_NEIGHBOR;
		} else if (strcasecmp(filter_text, "scale2x") == 0) {
			scale_filter = FILTER_SCALE2X;
		} else if (strcasecmp(filter_text, "hqx") == 0) {
			scale_filter = FILTER_HQX;
		} else {
			Error("unrecognized scalefilter value '%s'\n", filter_text);
		}
	}

	frame_rate = IniFile_GetInteger("framerate", 60);

	if (!OpenDune_Init(scaling_factor, scale_filter, frame_rate)) exit(1);

	g_mouseDisabled = 0;

	GameLoop_Main();

	if (MpTurn_IsActive()) MpTurn_End();
	Timer_ClaimAnimClock(false);
	if (MpGame_IsLive()) MpNet_Disconnect();

	PrintToConsole(String_Get_ByIndex(STR_THANK_YOU_FOR_PLAYING_DUNE_II));

	PrepareEnd();
	Free_IniFile();

	if (s_selectionSelfTest && s_selectionSelfTestResult != 1) return 1;
	if (s_combatBalanceSelfTest && s_combatBalanceSelfTestResult == 0) return 1;
	if (s_buildRulesSelfTest && s_buildRulesSelfTestResult != 1) return 1;
	if (s_pathfinderSelfTest && s_pathfinderSelfTestResult != 1) return 1;
	if (s_moveRulesSelfTest && s_moveRulesSelfTestResult != 1) return 1;
	if (s_lobbySelfTest && s_lobbySelfTestResult != 1) return 1;
	return 0;
}

/**
 * Prepare the map (after loading scenario or savegame). Does some basic
 *  sanity-check and corrects stuff all over the place.
 */
void Game_Prepare(void)
{
	PoolFindStruct find;
	uint16 oldSelectionType;
	Tile *t;
	int i;

	g_validateStrictIfZero++;

	oldSelectionType = g_selectionType;
	g_selectionType = SELECTIONTYPE_MENTAT;

	Structure_Recount();
	Unit_Recount();
	Team_Recount();

	t = &g_map[0];
	for (i = 0; i < 64 * 64; i++, t++) {
		Structure *s;
		Unit *u;

		u = Unit_Get_ByPackedTile(i);
		s = Structure_Get_ByPackedTile(i);

		if (u == NULL || !u->o.flags.s.used) t->hasUnit = false;
		if (s == NULL || !s->o.flags.s.used) t->hasStructure = false;
		if (t->isUnveiled) Map_UnveilTile(i, g_playerHouseID);
	}

	find.houseID = HOUSE_INVALID;
	find.index   = 0xFFFF;
	find.type    = 0xFFFF;

	while (true) {
		Unit *u;

		u = Unit_Find(&find);
		if (u == NULL) break;

		if (u->o.flags.s.isNotOnMap) continue;

		Unit_RemoveFog(u);
		Unit_UpdateMap(1, u);
	}

	find.houseID = HOUSE_INVALID;
	find.index   = 0xFFFF;
	find.type    = 0xFFFF;

	while (true) {
		Structure *s;

		s = Structure_Find(&find);
		if (s == NULL) break;
		if (s->o.type == STRUCTURE_SLAB_1x1 || s->o.type == STRUCTURE_SLAB_2x2 || s->o.type == STRUCTURE_WALL) continue;

		if (s->o.flags.s.isNotOnMap) continue;

		Structure_RemoveFog(s);

		if (s->o.type == STRUCTURE_STARPORT && s->o.linkedID != 0xFF) {
			Unit *u = Unit_Get_ByIndex(s->o.linkedID);

			if (!u->o.flags.s.used || !u->o.flags.s.isNotOnMap) {
				s->o.linkedID = 0xFF;
				s->countDown = 0;
			} else {
				Structure_SetState(s, STRUCTURE_STATE_READY);
			}
		}

		Script_Load(&s->o.script, s->o.type);

		if (s->o.type == STRUCTURE_PALACE) {
			House_Get_ByIndex(s->o.houseID)->palacePosition = s->o.position;
		}

		if ((House_Get_ByIndex(s->o.houseID)->palacePosition.x != 0) || (House_Get_ByIndex(s->o.houseID)->palacePosition.y != 0)) continue;
		House_Get_ByIndex(s->o.houseID)->palacePosition = s->o.position;
	}

	find.houseID = HOUSE_INVALID;
	find.index   = 0xFFFF;
	find.type    = 0xFFFF;

	while (true) {
		House *h;

		h = House_Find(&find);
		if (h == NULL) break;

		h->structuresBuilt = Structure_GetStructuresBuilt(h);
		House_UpdateCreditsStorage((uint8)h->index);
		House_CalculatePowerAndCredit(h);
	}

	GUI_Palette_CreateRemap(g_playerHouseID);

	Map_SetSelection(g_selectionPosition);

	if (g_structureActiveType != 0xFFFF) {
		Map_SetSelectionSize(g_table_structureInfo[g_structureActiveType].layout);
	} else {
		Structure *s = Structure_Get_ByPackedTile(g_selectionPosition);

		if (s != NULL) Map_SetSelectionSize(g_table_structureInfo[s->o.type].layout);
	}

	Voice_LoadVoices(g_playerHouseID);

	g_tickHousePowerMaintenance = max(g_timerGame + 70, g_tickHousePowerMaintenance);
	g_viewport_forceRedraw = true;
	g_playerCredits = 0xFFFF;

	g_selectionType = oldSelectionType;
	g_validateStrictIfZero--;
}

/**
 * Initialize a game, by setting most variables to zero, cleaning the map, etc
 *  etc.
 */
void Game_Init(void)
{
	/* Any game that starts from here is not the skirmish that may still be
	 * running: campaign restart, House pick and savegame load all pass
	 * through, and they must not inherit the skirmish AI hooks. */
	Skirmish_Reset();

	Unit_Init();
	Structure_Init();
	Team_Init();
	House_Init();

	/* The pools above hold the objects; these hold when each subsystem next
	 * runs, and they are absolute.  A second match in one process inherits the
	 * first one's deadlines and stands still until the clock passes them. */
	Unit_ResetTicks();
	Structure_ResetTicks();
	Team_ResetTicks();
	House_ResetTicks();

	Animation_Init();
	Explosion_Init();
	memset(g_map, 0, 64 * 64 * sizeof(Tile));

	memset(g_displayedViewport, 0, sizeof(g_displayedViewport));
	memset(g_displayedMinimap,  0, sizeof(g_displayedMinimap));
	memset(g_changedTilesMap,   0, sizeof(g_changedTilesMap));
	memset(g_dirtyViewport,     0, sizeof(g_dirtyViewport));
	memset(g_dirtyMinimap,      0, sizeof(g_dirtyMinimap));

	memset(g_mapTileID, 0, 64 * 64 * sizeof(uint16));
	memset(g_starportAvailable, 0, sizeof(g_starportAvailable));

	Sound_Output_Feedback(0xFFFE);

	g_playerCreditsNoSilo     = 0;
	g_houseMissileCountdown   = 0;
	g_selectionState          = 0; /* Invalid. */
	g_structureActivePosition = 0;

	g_unitHouseMissile = NULL;
	g_unitActive       = NULL;
	g_structureActive  = NULL;

	g_activeAction          = 0xFFFF;
	g_structureActiveType   = 0xFFFF;

	GUI_DisplayText(NULL, -1);

	sleepIdle();	/* let the game a chance to update screen, etc. */
}

/**
 * Load a scenario in a safe way, and prepare the game.
 * @param houseID The House which is going to play the game.
 * @param scenarioID The Scenario to load.
 */
void Game_LoadScenario(uint8 houseID, uint16 scenarioID)
{
	Sound_Output_Feedback(0xFFFE);

	/* The campaign is not a match: with no descriptor every Match_* query falls
	 * back to the original single-player rule, which is exactly the campaign. */
	Match_Reset();

	Game_Init();
	UnitSelection_ClearControlGroups();

	g_validateStrictIfZero++;

	if (!Scenario_Load(scenarioID, houseID)) {
		GUI_DisplayModalMessage("No more scenarios!", 0xFFFF);

		PrepareEnd();
		exit(0);
	}

	Game_Prepare();

	if (scenarioID < 5) {
		g_hintsShown1 = 0;
		g_hintsShown2 = 0;
	}

	g_validateStrictIfZero--;
}

/**
 * Close down facilities used by the program. Always called just before the
 *  program terminates.
 */
void PrepareEnd(void)
{
	free(g_palette_998A); g_palette_998A = NULL;

	GameLoop_Uninit();

	String_Uninit();
	Sprites_Uninit();
	Font_Uninit();
	Voice_UnloadVoices();

	Drivers_All_Uninit();

	if (g_mouseFileID != 0xFF) Mouse_SetMouseMode(INPUT_MOUSE_MODE_NORMAL, NULL);

	File_Uninit();
	Timer_Uninit();
	GFX_Uninit();
	Video_Uninit();
}
