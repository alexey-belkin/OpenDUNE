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
static void GameLoop_GameIntroAnimationMenu(void)
{
	static const uint16 mainMenuStrings[][6] = {
		{STR_PLAY_A_GAME, STR_REPLAY_INTRODUCTION, STR_EXIT_GAME, STR_NULL,         STR_NULL,         STR_NULL}, /* Neither HOF nor save. */
		{STR_PLAY_A_GAME, STR_REPLAY_INTRODUCTION, STR_LOAD_GAME, STR_EXIT_GAME,    STR_NULL,         STR_NULL}, /* Has a save game. */
		{STR_PLAY_A_GAME, STR_REPLAY_INTRODUCTION, STR_EXIT_GAME, STR_HALL_OF_FAME, STR_NULL,         STR_NULL}, /* Has a HOF. */
		{STR_PLAY_A_GAME, STR_REPLAY_INTRODUCTION, STR_LOAD_GAME, STR_EXIT_GAME,    STR_HALL_OF_FAME, STR_NULL}  /* Has a HOF and a save game. */
	};

	bool loadGame = false;
	static bool drawMenu = true;
	static uint16 stringID = STR_REPLAY_INTRODUCTION;
	uint16 maxWidth;
	static bool hasSave = false;
	static bool hasFame = false;
	static const char *strings[6];
	static uint16 index = 0xFFFF;

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

		for (i = 0; i < 6; i++) {
			strings[i] = NULL;

			if (mainMenuStrings[index][i] == 0) {
				if (g_widgetProperties[21].height == 0) g_widgetProperties[21].height = i;
				continue;
			}

			strings[i] = String_Get_ByIndex(mainMenuStrings[index][i]);
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
 * It orders production rather than units, and that limitation is worth stating
 * plainly.  A unit order is refused unless a human controls the unit
 * (UnitSelection_IsControllable), and flagging one of the skirmish AI's houses
 * as human does not work either: an AI house that runs out of money gets its
 * production put on hold, because clearing the hold is a player action, and it
 * never builds again.  A scripted player that really drives a house of its own
 * needs the human-versus-AI mode, which is a later stage.  Until then this
 * exercises the layer with commands that do reach an AI house, which is enough
 * to prove that recording and replaying one is faithful.
 */
static void MpHarness_ScriptedPlayer(uint32 tick)
{
	MpCommand cmd;
	uint16 round;
	uint16 i;

	if (tick == 0 || (tick % 500) != 0) return;

	round = (uint16)(tick / 500);

	/* Sweep the pool by index rather than by find order: it is the one walk that
	 * does not depend on when anything was allocated. */
	for (i = 0; i < STRUCTURE_INDEX_MAX_SOFT; i++) {
		Structure *s = Structure_Get_ByIndex(i);
		uint32 buildable;
		uint16 type;
		uint16 bit;

		if (s == NULL || !s->o.flags.s.used || !s->o.flags.s.allocated) continue;
		if (s->o.houseID != s_skirmishHouse[0]) continue;
		if (!g_table_structureInfo[s->o.type].o.flags.factory) continue;

		buildable = Structure_GetBuildable(s);
		if (buildable == 0) continue;

		/* Pick one of the things this factory can make, by the round, so the
		 * choice is a function of the tick and of what is on the map. */
		type = 0xFFFF;
		for (bit = 0; bit < 32; bit++) {
			uint16 candidate = (uint16)((round + bit) % 32);

			if ((buildable & (1u << candidate)) == 0) continue;
			type = candidate;
			break;
		}
		if (type == 0xFFFF) continue;

		MpCommand_Init(&cmd, MP_CMD_STRUCTURE_BUILD, s->o.houseID);
		cmd.object = s->o.index;
		cmd.value  = type;
		MpCommand_Submit(&cmd);
		break;
	}
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
			MpHarness_ScriptedPlayer(tick);
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
	if (s_skirmishSelfTest || s_skirmishDirect || s_warPlay || s_mpChecksum || s_mpReplay) {
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

		/* Second pass: same match, same process, no scripted player. */
		if (!MpHarness_ReplayPass(false, replayed, &replayCount, false)) {
			PrintToConsole("mp-replay: FAIL (could not restart the skirmish)");
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
			} else {
				started = Skirmish_Start(s_skirmishHouse[0], s_skirmishHouse[1]);
			}

			if (!started) {
				g_gameMode = GM_MENU;
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

		GUI_PaletteAnimate();

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
			Map_SetSelectionObjectPosition(0xFFFF);
			Map_SetSelectionObjectPosition(g_selectionRectanglePosition);
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

		key = GUI_Widget_HandleEvents(g_widgetLinkedListHead);
		GUI_Widget_Viewport_HandleEdgeScroll();
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
					Unit_DisplayStatusText(g_unitSelected);
					l_timerUnitStatus = g_timerGame + 300;
				}

				if (g_selectionType != SELECTIONTYPE_TARGET) {
					g_selectionPosition = Tile_PackTile(Tile_Center(g_unitSelected->o.position));
				}
			}

			GUI_Widget_ActionPanel_Draw(false);

			InGame_Numpad_Move(key);

			GUI_DrawCredits(g_playerHouseID, 0);

			/* Every step above x1 is another sequential simulation tick.
			 * Advancing the game timer between passes keeps every timer-driven
			 * system in step, rather than speeding up selected subsystems. */
			{
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

			GUI_DrawScreen(SCREEN_0);
		}

		GUI_DisplayText(NULL, 0);

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
			if (strcmp(argv[i], "--combat-balance-self-test") == 0) s_combatBalanceSelfTest = true;
			if (strcmp(argv[i], "--economy-trace") == 0) EcoSearch_SetTrace(true);
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
			} else if (strncmp(argv[i], "--mp-replay", 11) == 0) {
				s_mpReplay = true;
				if (argv[i][11] == '=') sscanf(argv[i] + 12, "%u,%u,%u", &s_mpReplayTicks, &s_mpReplayStep, &s_mpReplaySeed);
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

	PrintToConsole(String_Get_ByIndex(STR_THANK_YOU_FOR_PLAYING_DUNE_II));

	PrepareEnd();
	Free_IniFile();

	if (s_selectionSelfTest && s_selectionSelfTestResult != 1) return 1;
	if (s_combatBalanceSelfTest && s_combatBalanceSelfTestResult == 0) return 1;
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
