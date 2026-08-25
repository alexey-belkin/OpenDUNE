/** @file src/gui/widget_click.c %Widget clicking handling routines. */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "multichar.h"
#include "types.h"
#include "../os/endian.h"
#include "../os/math.h"
#include "../os/sleep.h"
#include "../os/strings.h"

#include "gui.h"
#include "widget.h"
#include "../mpcommand.h"
#include "../match.h"
#include "../mpturn.h"
#include "../os/error.h"
#include "../audio/driver.h"
#include "../audio/sound.h"
#include "../config.h"
#include "../file.h"
#include "../gfx.h"
#include "../house.h"
#include "../input/input.h"
#include "../input/mouse.h"
#include "../load.h"
#include "../map.h"
#include "../opendune.h"
#include "../pool/structure.h"
#include "../pool/unit.h"
#include "../save.h"
#include "../sprites.h"
#include "../string.h"
#include "../structure.h"
#include "../table/strings.h"
#include "../tile.h"
#include "../timer.h"
#include "../unit.h"


char g_savegameDesc[5][51];                                 /*!< Array of savegame descriptions for the SaveLoad window. */
static uint16 s_savegameIndexBase = 0;
static uint16 s_savegameCountOnDisk = 0;                    /*!< Amount of savegames on disk. */
static uint32 s_factoryListLastClick;                       /*!< GUI tick of the previous factory-list click. */
static uint16 s_factoryListLastIndex = 0xFFFF;              /*!< Previously clicked factory-list item. */

void GUI_Production_List_ResetDoubleClick(void)
{
	s_factoryListLastClick = 0;
	s_factoryListLastIndex = 0xFFFF;
}

static char *GenerateSavegameFilename(uint16 number)
{
	static char filename[13];
	if (number >= 1000) {
		Warning("Savegame #%hu not supported\n", number);
		return NULL;
	}
	sprintf(filename, "_save%03hu.dat", number);
	return filename;
}

/**
 * Handles scrolling of a scrollbar.
 *
 * @param scrollbar The scrollbar.
 * @param scroll The amount of scrolling.
 */
static void GUI_Widget_Scrollbar_Scroll(WidgetScrollbar *scrollbar, uint16 scroll)
{
	scrollbar->scrollPosition += scroll;

	if ((int16)scrollbar->scrollPosition >= scrollbar->scrollMax - scrollbar->scrollPageSize) {
		scrollbar->scrollPosition = scrollbar->scrollMax - scrollbar->scrollPageSize;
	}

	if ((int16)scrollbar->scrollPosition <= 0) scrollbar->scrollPosition = 0;

	GUI_Widget_Scrollbar_CalculatePosition(scrollbar);

	GUI_Widget_Scrollbar_Draw(scrollbar->parent);
}

/**
 * Handles Click event for a sprite/text button.
 *
 * @param w The widget.
 * @return False, always.
 */
bool GUI_Widget_SpriteTextButton_Click(Widget *w)
{
	Structure *s;

	VARIABLE_NOT_USED(w);

	s = Structure_Get_ByPackedTile(g_selectionPosition);
	if (Structure_Queue_CanOrder(s)) {
		MpCommand cmd;

		/* This button fires on mouse release: 0x04 is left and 0x40 is right. */
		MpCommand_Init(&cmd, MP_CMD_STRUCTURE_QUEUE, s->o.houseID);
		cmd.object = s->o.index;
		cmd.value  = ((w->state.buttonState & 0x40) != 0) ? 1 : 0;
		MpCommand_Submit(&cmd);

		GUI_Widget_ActionPanel_Draw(true);
		return false;
	}

	switch (g_productionStringID) {
		default: break;

		case STR_PLACE_IT:
			if (s->o.type == STRUCTURE_CONSTRUCTION_YARD) {
				Structure *ns;

				ns = Structure_Get_ByIndex(s->o.linkedID);
				g_structureActive = ns;
				g_structureActiveType = s->objectType;
				g_selectionState = Structure_IsValidBuildLocation(g_selectionRectanglePosition, g_structureActiveType, g_playerHouseID);
				g_structureActivePosition = g_selectionPosition;

				/* linkedID stays with the yard until the placement command runs:
				 * entering placement mode is one player looking at their own
				 * screen, and it may not move state the other client cannot see.
				 * A yard destroyed mid-placement now takes the unplaced building
				 * with it (Structure_Destroy), which is why Cancel below no
				 * longer has to put anything back. */

				GUI_ChangeSelectionType(SELECTIONTYPE_PLACE);
			}
			break;

		case STR_ON_HOLD: {
			MpCommand cmd;

			MpCommand_Init(&cmd, MP_CMD_STRUCTURE_HOLD, s->o.houseID);
			cmd.object = s->o.index;
			cmd.value  = 0;
			MpCommand_Submit(&cmd);
		} break;

		case STR_BUILD_IT: {
			MpCommand cmd;

			MpCommand_Init(&cmd, MP_CMD_STRUCTURE_BUILD, s->o.houseID);
			cmd.object = s->o.index;
			cmd.value  = s->objectType;
			MpCommand_Submit(&cmd);
		} break;

		case STR_LAUNCH:
		case STR_FREMEN:
		case STR_SABOTEUR: {
			MpCommand cmd;

			MpCommand_Init(&cmd, MP_CMD_STRUCTURE_SPECIAL, s->o.houseID);
			cmd.object = s->o.index;
			MpCommand_Submit(&cmd);
		} break;

		case STR_D_DONE: {
			MpCommand cmd;

			MpCommand_Init(&cmd, MP_CMD_STRUCTURE_HOLD, s->o.houseID);
			cmd.object = s->o.index;
			cmd.value  = 1;
			MpCommand_Submit(&cmd);
		} break;
	}
	return false;
}

/**
 * Handles Click event for scrollbar up button.
 *
 * @param w The widget.
 * @return False, always.
 */
bool GUI_Widget_Scrollbar_ArrowUp_Click(Widget *w)
{
	GUI_Widget_Scrollbar_Scroll(w->data, -1);

	return false;
}

/**
 * Handles Click event for scrollbar down button.
 *
 * @param w The widget.
 * @return False, always.
 */
bool GUI_Widget_Scrollbar_ArrowDown_Click(Widget *w)
{
	GUI_Widget_Scrollbar_Scroll(w->data, 1);

	return false;
}

/**
 * Handles Click event for scrollbar button.
 *
 * @param w The widget.
 * @return False, always.
 */
bool GUI_Widget_Scrollbar_Click(Widget *w)
{
	WidgetScrollbar *scrollbar;
	uint16 positionX, positionY;

	scrollbar = w->data;

	positionX = w->offsetX;
	if (w->offsetX < 0) positionX += g_widgetProperties[w->parentID].width << 3;
	positionX += g_widgetProperties[w->parentID].xBase << 3;

	positionY = w->offsetY;
	if (w->offsetY < 0) positionY += g_widgetProperties[w->parentID].height;
	positionY += g_widgetProperties[w->parentID].yBase;

	if ((w->state.buttonState & 0x44) != 0) {
		scrollbar->pressed = 0;
		GUI_Widget_Scrollbar_Draw(w);
	}

	if ((w->state.buttonState & 0x11) != 0) {
		int16 positionCurrent;
		int16 positionBegin;
		int16 positionEnd;

		scrollbar->pressed = 0;

		if (w->width > w->height) {
			positionCurrent = g_mouseX;
			positionBegin = positionX + scrollbar->position + 1;
		} else {
			positionCurrent = g_mouseY;
			positionBegin = positionY + scrollbar->position + 1;
		}

		positionEnd = positionBegin + scrollbar->size;

		if (positionCurrent <= positionEnd && positionCurrent >= positionBegin) {
			scrollbar->pressed = 1;
			scrollbar->pressedPosition = positionCurrent - positionBegin;
		} else {
			GUI_Widget_Scrollbar_Scroll(scrollbar, (positionCurrent < positionBegin ? -scrollbar->scrollPageSize : scrollbar->scrollPageSize));
		}
	}

	if ((w->state.buttonState & 0x22) != 0 && scrollbar->pressed != 0) {
		int16 position, size;

		if (w->width > w->height) {
			size = w->width - 2 - scrollbar->size;
			position = g_mouseX - scrollbar->pressedPosition - positionX - 1;
		} else {
			size = w->height - 2 - scrollbar->size;
			position = g_mouseY - scrollbar->pressedPosition - positionY - 1;
		}

		if (position < 0) {
			position = 0;
		} else if (position > size) {
			position = size;
		}

		if (scrollbar->position != position) {
			scrollbar->position = position;
			scrollbar->dirty = 1;
		}

		GUI_Widget_Scrollbar_CalculateScrollPosition(scrollbar);

		if (scrollbar->dirty != 0) GUI_Widget_Scrollbar_Draw(w);
	}

	return false;
}

/**
 * Handles Click event for unit commands button.
 *
 * @param w The widget.
 * @return True, always.
 */
bool GUI_Widget_TextButton_Click(Widget *w)
{
	const UnitInfo *ui;
	const ActionInfo *ai;
	const uint16 *actions;
	ActionType action;
	Unit *u;
	uint16 *found;
	ActionType unitAction;

	if (g_unitSelectionCount > 1) {
		uint16 slot;

		if (w->index >= 8 && w->index <= 11) {
			slot = w->index - 8;
		} else if (w->index >= 30 && w->index <= 33) {
			slot = w->index - 26;
		} else {
			return true;
		}

		action = UnitSelection_GetActionForSlot(slot);
		if (action == ACTION_INVALID) return true;

		{
			bool narrow = g_dune2_enhanced && (Input_Test(0x2c) || Input_Test(0x39));	/* LSHIFT or RSHIFT */

			action = UnitSelection_GetPanelAction(action, narrow);
			if (narrow && action == ACTION_ATTACK) action = ACTION_AMBUSH;
		}

		GUI_Widget_MakeSelected(w, false);
		if (UnitSelection_BeginAction(action)) {
			g_unitActive = g_unitSelected;
			g_activeAction = action;
			GUI_ChangeSelectionType(SELECTIONTYPE_TARGET);
		}

		return true;
	}

	u = g_unitSelected;
	if (u == NULL) return true;
	ui = &g_table_unitInfo[u->o.type];

	actions = ui->o.actionsPlayer;
	if (Unit_GetHouseID(u) != g_playerHouseID && u->o.type != UNIT_HARVESTER) {
		actions = g_table_actionsAI;
	}

	action = actions[w->index - 8];
	if (g_dune2_enhanced) {
		bool narrow = (Input_Test(0x2c) || Input_Test(0x39)) != 0;	/* LSHIFT or RSHIFT is pressed */

		/* The AI command list keeps its literal meaning; only what the player is
		 * offered for their own units is translated. */
		if (actions == ui->o.actionsPlayer) action = UnitSelection_GetPanelAction(action, narrow);
		if (narrow && action == ACTION_ATTACK) action = ACTION_AMBUSH; /* AMBUSH instead of ATTACK */

		Debug("GUI_Widget_TextButton_Click(%p index=%d) action=%d\n", w, w->index, action);
	}

	unitAction = u->nextActionID;
	if (unitAction == ACTION_INVALID) {
		unitAction = u->actionID;
	}

	GUI_Widget_MakeSelected(w, false);

	ai = &g_table_actionInfo[action];

	if (ai->selectionType != g_selectionType) {
		if (ai->selectionType == SELECTIONTYPE_TARGET) UnitSelection_BeginTargeting();
		g_unitActive = g_unitSelected;
		g_activeAction = action;
		GUI_ChangeSelectionType(ai->selectionType);

		return true;
	}

	/* One unit selected used to take a different road out of this function than
	 * two did: the group above submits an order, and this branch reached in and
	 * changed the unit where it stood.  Unit_SetAction() loads bytecode, so the
	 * clicking player's copy of that unit started running a different script
	 * from everybody else's -- the unit chunk diverged, then the tiles it walked
	 * over, then the randoms its script drew.  It is the same order either way;
	 * it goes the same way.
	 *
	 * The deviation shake that used to sit above this went with it, into the
	 * executor, for the same reason. */
	{
		MpCommand cmd;

		MpCommand_Init(&cmd, MP_CMD_UNIT_ACTION, (uint8)g_playerHouseID);
		cmd.action  = (uint8)action;
		cmd.count   = 1;
		cmd.unit[0] = u->o.index;

		MpCommand_Submit(&cmd);
	}

	if (ui->movementType == MOVEMENT_FOOT) Sound_StartSound(ai->soundID);

	if (unitAction == action) return true;

	found = memchr(actions, unitAction, 4);
	if (found == NULL) return true;

	GUI_Widget_MakeNormal(GUI_Widget_Get_ByIndex(g_widgetLinkedListHead, (uint16)(found - actions + 8)), false);

	return true;
}

/**
 * Handles Click event for current selection name.
 *
 * @return False, always.
 */
bool GUI_Widget_Name_Click(Widget *w)
{
	Object *o;
	uint16 packed;

	VARIABLE_NOT_USED(w);

	o = Object_GetByPackedTile(g_selectionPosition);

	if (o == NULL) return false;

	packed = Tile_PackTile(o->position);

	Map_SetViewportPosition(packed);
	Map_SetSelection(packed);

	return false;
}

/**
 * Handles Click event for "Cancel" button.
 *
 * @return True, always.
 */
bool GUI_Widget_Cancel_Click(Widget *w)
{
	VARIABLE_NOT_USED(w);

	if (g_structureActiveType != 0xFFFF) {
		/* Purely local now: nothing was taken from the yard to give back. */
		g_structureActive = NULL;
		g_structureActiveType = 0xFFFF;

		GUI_ChangeSelectionType(SELECTIONTYPE_STRUCTURE);

		g_selectionState = 0; /* Invalid. */
	}

	if (g_unitActive == NULL) return true;

	g_unitActive = NULL;
	g_activeAction = 0xFFFF;
	g_cursorSpriteID = 0;

	Sprites_SetMouseSprite(0, 0, g_sprites[0]);

	GUI_ChangeSelectionType(SELECTIONTYPE_UNIT);

	return true;
}

/**
 * Handles Click event for current selection picture.
 *
 * @return False, always.
 */
bool GUI_Widget_Picture_Click(Widget *w)
{
	Structure *s;

	VARIABLE_NOT_USED(w);

	if (g_unitSelected != NULL) {
		Unit_DisplayStatusText(g_unitSelected);

		return false;
	}

	s = Structure_Get_ByPackedTile(g_selectionPosition);

	if (s == NULL || !g_table_structureInfo[s->o.type].o.flags.factory) return false;

	Structure_BuildObject(s, 0xFFFF);

	return false;
}

/**
 * Handles Click event for "Repair/Upgrade" button.
 *
 * @param w The widget.
 * @return False, always.
 */
bool GUI_Widget_RepairUpgrade_Click(Widget *w)
{
	MpCommand cmd;
	Structure *s;

	VARIABLE_NOT_USED(w);

	s = Structure_Get_ByPackedTile(g_selectionPosition);
	if (s == NULL) return false;

	/* Starting a repair changes the structure and spends the house's money, so
	 * it is an order and not a button press.  Pressed here it was one player's
	 * building repairing itself on one machine. */
	MpCommand_Init(&cmd, MP_CMD_STRUCTURE_REPAIR, s->o.houseID);
	cmd.object = s->o.index;
	MpCommand_Submit(&cmd);

	return false;
}

static void GUI_Widget_Undraw(Widget *w, uint8 colour)
{
	uint16 offsetX;
	uint16 offsetY;
	uint16 width;
	uint16 height;

	if (w == NULL) return;

	offsetX = w->offsetX + (g_widgetProperties[w->parentID].xBase << 3);
	offsetY = w->offsetY + g_widgetProperties[w->parentID].yBase;
	width = w->width;
	height = w->height;

	if (GFX_Screen_IsActive(SCREEN_0)) {
		GUI_Mouse_Hide_InRegion(offsetX, offsetY, offsetX + width, offsetY + height);
	}

	GUI_DrawFilledRectangle(offsetX, offsetY, offsetX + width, offsetY + height, colour);

	if (GFX_Screen_IsActive(SCREEN_0)) {
		GUI_Mouse_Show_InRegion();
	}
}

void GUI_Window_Create(WindowDesc *desc)
{
	uint8 i;

	if (desc == NULL) return;

	g_widgetLinkedListTail = NULL;

	GFX_Screen_SetActive(SCREEN_1);

	Widget_SetCurrentWidget(desc->index);

	GUI_Widget_DrawBorder(g_curWidgetIndex, 2, true);

	if (GUI_String_Get_ByIndex(desc->stringID) != NULL) {
		GUI_DrawText_Wrapper(GUI_String_Get_ByIndex(desc->stringID), (g_curWidgetXBase << 3) + (g_curWidgetWidth << 2), g_curWidgetYBase + 6 + ((desc == &g_yesNoWindowDesc) ? 2 : 0), 238, 0, 0x122);
	}

	if (GUI_String_Get_ByIndex(desc->widgets[0].stringID) == NULL) {
		GUI_DrawText_Wrapper(String_Get_ByIndex(STR_THERE_ARE_NO_SAVED_GAMES_TO_LOAD), (g_curWidgetXBase + 2) << 3, g_curWidgetYBase + 42, 232, 0, 0x22);
	}

	for (i = 0; i < desc->widgetCount; i++) {
		Widget *w = &g_table_windowWidgets[i];

		if (GUI_String_Get_ByIndex(desc->widgets[i].stringID) == NULL) continue;

		w->next      = NULL;
		w->offsetX   = desc->widgets[i].offsetX;
		w->offsetY   = desc->widgets[i].offsetY;
		w->width     = desc->widgets[i].width;
		w->height    = desc->widgets[i].height;
		w->shortcut  = 0;
		w->shortcut2 = 0;

		if (desc != &g_savegameNameWindowDesc) {
			if (desc->widgets[i].labelStringId != STR_NULL) {
				w->shortcut = GUI_Widget_GetShortcut(*GUI_String_Get_ByIndex(desc->widgets[i].labelStringId));
			} else {
				w->shortcut = GUI_Widget_GetShortcut(*GUI_String_Get_ByIndex(desc->widgets[i].stringID));
			}
		}

		w->shortcut2 = desc->widgets[i].shortcut2;
		if (w->shortcut == 0x1B) {
			w->shortcut2 = 0x13;
		}

		w->stringID = desc->widgets[i].stringID;
		w->drawModeNormal   = DRAW_MODE_CUSTOM_PROC;
		w->drawModeSelected = DRAW_MODE_CUSTOM_PROC;
		w->drawModeDown     = DRAW_MODE_CUSTOM_PROC;
		w->drawParameterNormal.proc   = &GUI_Widget_TextButton_Draw;
		w->drawParameterSelected.proc = &GUI_Widget_TextButton_Draw;
		w->drawParameterDown.proc     = &GUI_Widget_TextButton_Draw;
		w->parentID = desc->index;
		memset(&w->state, 0, sizeof(w->state));

		g_widgetLinkedListTail = GUI_Widget_Link(g_widgetLinkedListTail, w);

		GUI_Widget_MakeVisible(w);
		GUI_Widget_MakeNormal(w, false);
		GUI_Widget_Draw(w);

		if (desc->widgets[i].labelStringId == STR_NULL) continue;

		if (g_config.language == LANGUAGE_FRENCH) {
			GUI_DrawText_Wrapper(GUI_String_Get_ByIndex(desc->widgets[i].labelStringId), (g_widgetProperties[w->parentID].xBase << 3) + 40, w->offsetY + g_widgetProperties[w->parentID].yBase + 3, 232, 0, 0x22);
		} else {
			GUI_DrawText_Wrapper(GUI_String_Get_ByIndex(desc->widgets[i].labelStringId), w->offsetX + (g_widgetProperties[w->parentID].xBase << 3) - 10, w->offsetY + g_widgetProperties[w->parentID].yBase + 3, 232, 0, 0x222);
		}
	}

	if (s_savegameCountOnDisk >= 5 && desc->addArrows) {
		Widget *w = &g_table_windowWidgets[8];

		w->drawParameterNormal.sprite   = g_sprites[59];
		w->drawParameterSelected.sprite = g_sprites[60];
		w->drawParameterDown.sprite     = g_sprites[60];
		w->next             = NULL;
		w->parentID         = desc->index;

		GUI_Widget_MakeNormal(w, false);
		GUI_Widget_MakeInvisible(w);
		GUI_Widget_Undraw(w, 233);

		g_widgetLinkedListTail = GUI_Widget_Link(g_widgetLinkedListTail, w);

		w = &g_table_windowWidgets[9];

		w->drawParameterNormal.sprite   = g_sprites[61];
		w->drawParameterSelected.sprite = g_sprites[62];
		w->drawParameterDown.sprite     = g_sprites[62];
		w->next             = NULL;
		w->parentID         = desc->index;

		GUI_Widget_MakeNormal(w, false);
		GUI_Widget_MakeInvisible(w);
		GUI_Widget_Undraw(w, 233);

		g_widgetLinkedListTail = GUI_Widget_Link(g_widgetLinkedListTail, w);
	}

	GUI_Mouse_Hide_Safe();

	Widget_SetCurrentWidget(desc->index);

	GUI_Screen_Copy(g_curWidgetXBase, g_curWidgetYBase, g_curWidgetXBase, g_curWidgetYBase, g_curWidgetWidth, g_curWidgetHeight, SCREEN_1, SCREEN_0);

	GUI_Mouse_Show_Safe();

	GFX_Screen_SetActive(SCREEN_0);
}

/**
 * Somewhere to keep the screen while a window sits on top of it.
 *
 * SCREEN_2 is where Sprites_LoadTiles() parks UNIT.EMC, so stashing pixels
 * there overwrites the script every unit is executing.  The original could
 * afford it: a window stopped the world, and nothing read the script until the
 * window closed and the tiles were reloaded.  In a match the world keeps
 * running underneath -- the turn loop is pumped from sleepIdle(), which is what
 * the window's own event loop calls -- so the units go on executing a buffer
 * that now holds a picture of the sidebar.  It crashes as "[SCRIPT] Unknown
 * opcode" a few frames after the menu opens.
 *
 * SCREEN_3 is only ever touched by the intro and the credits, neither of which
 * can be on screen during a game, and it is the larger of the two.
 */
static Screen GUI_Window_ScratchScreen(void)
{
	return MpTurn_IsActive() ? SCREEN_3 : SCREEN_2;
}

void GUI_Window_BackupScreen(WindowDesc *desc)
{
	Widget_SetCurrentWidget(desc->index);

	GUI_Mouse_Hide_Safe();
	GFX_CopyToBuffer(g_curWidgetXBase * 8, g_curWidgetYBase, g_curWidgetWidth * 8, g_curWidgetHeight, GFX_Screen_Get_ByIndex(GUI_Window_ScratchScreen()));
	GUI_Mouse_Show_Safe();
}

void GUI_Window_RestoreScreen(WindowDesc *desc)
{
	Widget_SetCurrentWidget(desc->index);

	GUI_Mouse_Hide_Safe();
	GFX_CopyFromBuffer(g_curWidgetXBase * 8, g_curWidgetYBase, g_curWidgetWidth * 8, g_curWidgetHeight, GFX_Screen_Get_ByIndex(GUI_Window_ScratchScreen()));
	GUI_Mouse_Show_Safe();
}

/**
 * Handles Click event for "Game controls" button.
 *
 * @param w The widget.
 */
static void GUI_Widget_GameControls_Click(Widget *w)
{
	WindowDesc *desc = &g_gameControlWindowDesc;
	bool loop;

	GUI_Window_BackupScreen(desc);

	GUI_Window_Create(desc);

	for (loop = true; loop; sleepIdle()) {
		Widget *w2 = g_widgetLinkedListTail;
		uint16 key = GUI_Widget_HandleEvents(w2);

		if ((key & 0x8000) != 0) {
			w = GUI_Widget_Get_ByIndex(w2, key & 0x7FFF);

			switch ((key & 0x7FFF) - 0x1E) {
				case 0:
					g_gameConfig.music ^= 0x1;
					if (g_gameConfig.music == 0) Driver_Music_Stop();
					break;

				case 1:
					g_gameConfig.sounds ^= 0x1;
					if (g_gameConfig.sounds == 0) Driver_Sound_Stop();
					break;

				case 2:
					g_gameConfig.gameSpeed = (g_gameConfig.gameSpeed == GAME_SPEED_FAST) ? GAME_SPEED_NORMAL : GAME_SPEED_FAST;
					break;

				case 3:
					g_gameConfig.hints ^= 0x1;
					break;

				case 4:
					g_gameConfig.autoScroll ^= 0x1;
					break;

				case 5:
					g_gameConfig.unitHealthBars ^= 0x1;
					break;

				case 6:
					g_gameConfig.debugLines ^= 0x1;
					break;

				case 7:
					loop = false;
					break;

				default: break;
			}

			GUI_Widget_MakeNormal(w, false);

			GUI_Widget_Draw(w);
		}

		GUI_PaletteAnimate();
	}

	GUI_Window_RestoreScreen(desc);
}

/* shade everything except colors 231 to 238 */
static void ShadeScreen(void)
{
	uint16 i;

	memmove(g_palette_998A, g_palette1, 256 * 3);

	for (i = 0; i < 231 * 3; i++) g_palette1[i] = g_palette1[i] / 2;
	for (i = 239 * 3; i < 256 * 3; i++) g_palette1[i] = g_palette1[i] / 2;

	GFX_SetPalette(g_palette_998A);
}

static void UnshadeScreen(void)
{
	memmove(g_palette1, g_palette_998A, 256 * 3);

	GFX_SetPalette(g_palette1);
}

static bool GUI_YesNo(uint16 stringID)
{
	WindowDesc *desc = &g_yesNoWindowDesc;
	bool loop;
	bool ret = false;

	desc->stringID = stringID;

	GUI_Window_BackupScreen(desc);

	GUI_Window_Create(desc);

	for (loop = true; loop; sleepIdle()) {
		uint16 key = GUI_Widget_HandleEvents(g_widgetLinkedListTail);

		if ((key & 0x8000) != 0) {
			switch (key & 0x7FFF) {
				case 0x1E: ret = true; break;
				case 0x1F: ret = false; break;
				default: break;
			}
			loop = false;
		}

		GUI_PaletteAnimate();
	}

	GUI_Window_RestoreScreen(desc);

	return ret;
}
/**
 * What a modal screen does to the *world* on its way in.
 *
 * Every fullscreen screen in this game -- the options, the mentat, the build
 * list -- opened with its own copy of these three calls.  They are collected
 * here for one reason: in a match the question "what does opening this window
 * change" has to have an answer that can be read in one place and tested in
 * one place.  Everything else those screens do is presentation and stays with
 * them.
 *
 * The timer call is a no-op in a match -- the turn loop owns the clock and
 * Timer_SetTimer() refuses TIMER_GAME while it does (timer.c:499) -- and
 * unloading the tiles costs nothing a fullscreen window has not already cost.
 * The world keeps turning behind the window either way: the nested event loops
 * reach sleepIdle(), and the match pump lives there.
 */
void GUI_ModalScreen_Enter(void)
{
	Driver_Voice_Play(NULL, 0xFF);

	Sprites_UnloadTiles();

	Timer_SetTimer(TIMER_GAME, false);
}

/** The other half of GUI_ModalScreen_Enter(). */
void GUI_ModalScreen_Leave(void)
{
	Sprites_LoadTiles();

	Timer_SetTimer(TIMER_GAME, true);
}

/**
 * The find-array rebuild the options screen inherited from savegame loading.
 *
 * Outside a match it is merely pointless: the arrays are maintained as units
 * and structures come and go, so a rebuild produces the same set.  It does not
 * produce the same *order*.  Allocation appends (pool/unit.c:151) and death
 * compacts (pool/unit.c:184), so the live order is creation order -- and
 * Unit_SortOrder() then works it towards front-to-back with one bubble pass per
 * tick, which makes the order a function of the whole history, not of the set.
 * Unit_Recount() throws that away and rebuilds in index order.
 *
 * GameLoop_Unit() walks that array. Reordering it on one client and not the
 * other means the two machines tick their units in different sequences and draw
 * from the shared RNG in a different order, and from there it is two different
 * games -- which is exactly what happened when a player opened Options during a
 * match.  No savegame chunk records this order, so nothing caught it either.
 */
void GUI_Options_Recount(void)
{
	/* Match_IsActive() rather than MpTurn_IsActive(): the order matters from the
	 * first tick of a match, and the turn loop only starts several hundred
	 * milliseconds of setup later.  A skirmish is held to the same rule, which
	 * is what lets --mp-modal test this without a socket. */
	if (Match_IsActive()) return;

	Structure_Recount();
	Unit_Recount();
}

/**
 * Handles Click event for "Options" button.
 *
 * @param w The widget.
 * @return False, always.
 */
bool GUI_Widget_Options_Click(Widget *w)
{
	WindowDesc *desc = &g_optionsWindowDesc;
	uint16 cursor = g_cursorSpriteID;
	bool loop;

	g_cursorSpriteID = 0;

	Sprites_SetMouseSprite(0, 0, g_sprites[0]);

	GUI_ModalScreen_Enter();

	memmove(g_palette_998A, g_paletteActive, 256 * 3);

	GUI_DrawText_Wrapper(NULL, 0, 0, 0, 0, 0x22);

	ShadeScreen();

	GUI_Window_BackupScreen(desc);

	GUI_Window_Create(desc);

	for (loop = true; loop; sleepIdle()) {
		Widget *w2 = g_widgetLinkedListTail;
		uint16 key = GUI_Widget_HandleEvents(w2);

		if ((key & 0x8000) != 0) {
			w = GUI_Widget_Get_ByIndex(w2, key);

			GUI_Window_RestoreScreen(desc);

			/* Four of these seven buttons end or replace this client's world,
			 * and a networked match is not this client's world to end.  Loading
			 * is the sharp one -- it swaps the whole simulation out from under
			 * the turn loop while the other player carries on -- but restart and
			 * pick-a-house leave the opponent playing against nobody just as
			 * surely.  Saving is refused with them because a save taken here can
			 * only be loaded there. */
			if (MpTurn_IsActive()) {
				switch ((key & 0x7FFF) - 0x1E) {
					case 0: case 1: case 3: case 4:
						GUI_DisplayModalMessage("Not while a network game is running.", 0xFFFF);
						GUI_Window_BackupScreen(desc);
						GUI_Window_Create(desc);
						continue;

					default:
						break;
				}
			}

			switch ((key & 0x7FFF) - 0x1E) {
				case 0:
					if (GUI_Widget_SaveLoad_Click(false)) loop = false;
					break;

				case 1:
					if (GUI_Widget_SaveLoad_Click(true)) loop = false;
					break;

				case 2:
					GUI_Widget_GameControls_Click(w);
					break;

				case 3:
					/* "Are you sure you wish to restart?" */
					if (!GUI_YesNo(STR_ARE_YOU_SURE_YOU_WISH_TO_RESTART)) break;

					loop = false;
					g_gameMode = GM_RESTART;
					break;

				case 4:
					/* "Are you sure you wish to pick a new house?" */
					if (!GUI_YesNo(STR_ARE_YOU_SURE_YOU_WISH_TO_PICK_A_NEW_HOUSE)) break;

					loop = false;
					Driver_Music_FadeOut();
					g_gameMode = GM_PICKHOUSE;
					break;

				case 5:
					loop = false;
					break;

				case 6:
					/* "Are you sure you want to quit playing?" */
					loop = !GUI_YesNo(STR_ARE_YOU_SURE_YOU_WANT_TO_QUIT_PLAYING);
					g_running = loop;

					Sound_Output_Feedback(0xFFFE);

					while (Driver_Voice_IsPlaying()) sleepIdle();
					break;

				default: break;
			}

			if (g_running && loop) {
				GUI_Window_BackupScreen(desc);

				GUI_Window_Create(desc);
			}
		}

		GUI_PaletteAnimate();
	}

	g_textDisplayNeedsUpdate = true;

	GUI_ModalScreen_Leave();
	GUI_DrawInterfaceAndRadar(SCREEN_0);

	UnshadeScreen();

	GUI_Widget_MakeSelected(w, false);

	GameOptions_Save();

	GUI_Options_Recount();

	g_cursorSpriteID = cursor;

	Sprites_SetMouseSprite(0, 0, g_sprites[cursor]);

	return false;
}

static uint16 GetSavegameCount(void)
{
	uint16 i;

	for (i = 0;; i++) {
		if (!File_Exists_Personal(GenerateSavegameFilename(i))) return i;
	}
}

static void FillSavegameDesc(bool save)
{
	uint8 i;

	for (i = 0; i < 5; i++) {
		char *desc = g_savegameDesc[i];
		char *filename;
		uint8 fileId;

		*desc = '\0';

		if (s_savegameIndexBase - i < 0) continue;

		if (s_savegameIndexBase - i == s_savegameCountOnDisk) {
			if (!save) continue;

			strncpy(desc, String_Get_ByIndex(STR_EMPTY_SLOT_), 50);
			continue;
		}

		filename = GenerateSavegameFilename(s_savegameIndexBase - i);

		fileId = ChunkFile_Open_Personal(filename);
		if (fileId == FILE_INVALID) continue;
		ChunkFile_Read(fileId, HTOBE32(CC_NAME), desc, 50);
		ChunkFile_Close(fileId);
		continue;
	}
}


/**
 * Handles Click event for savegame button.
 *
 * @param index The index of the clicked button.
 * @return True if a game has been saved, False otherwise.
 */
static bool GUI_Widget_Savegame_Click(uint16 index)
{
	WindowDesc *desc = &g_savegameNameWindowDesc;
	bool loop;
	char *saveDesc = g_savegameDesc[index];
	bool widgetPaint;
	bool ret;

	if (*saveDesc == '[') *saveDesc = 0;

	GUI_Window_BackupScreen(desc);

	GUI_Window_Create(desc);

	ret = false;
	widgetPaint = true;

	if (*saveDesc == '[') index = s_savegameCountOnDisk;

	GFX_Screen_SetActive(SCREEN_0);

	Widget_SetCurrentWidget(15);

	GUI_Mouse_Hide_Safe();
	GUI_DrawBorder((g_curWidgetXBase << 3) - 1, g_curWidgetYBase - 1, (g_curWidgetWidth << 3) + 2, g_curWidgetHeight + 2, 4, false);
	GUI_Mouse_Show_Safe();

	for (loop = true; loop; sleepIdle()) {
		uint16 eventKey;
		Widget *w = g_widgetLinkedListTail;

		GUI_DrawText_Wrapper(NULL, 0, 0, 232, 235, 0x22);

		eventKey = GUI_EditBox(saveDesc, 50, 15, g_widgetLinkedListTail, NULL, widgetPaint);
		widgetPaint = false;

		if ((eventKey & 0x8000) == 0) continue;

		GUI_Widget_MakeNormal(GUI_Widget_Get_ByIndex(w, eventKey & 0x7FFF), false);

		switch (eventKey & 0x7FFF) {
			case 0x1E:	/* RETURN / Save Button */
				if (*saveDesc == 0) break;

				SaveGame_SaveFile(GenerateSavegameFilename(s_savegameIndexBase - index), saveDesc);
				loop = false;
				ret = true;
				break;

			case 0x1F:	/* ESCAPE / Cancel Button */
				loop = false;
				ret = false;
				FillSavegameDesc(true);
				break;

			default: break;
		}
	}

	GUI_Window_RestoreScreen(desc);

	return ret;
}

static void UpdateArrows(bool save, bool force)
{
	static uint16 previousIndex = 0;
	Widget *w;

	if (!force && s_savegameIndexBase == previousIndex) return;

	previousIndex = s_savegameIndexBase;

	w = &g_table_windowWidgets[9];
	if (s_savegameIndexBase >= 5) {
		GUI_Widget_MakeVisible(w);
	} else {
		GUI_Widget_MakeInvisible(w);
		GUI_Widget_Undraw(w, 233);
	}

	w = &g_table_windowWidgets[8];
	if (s_savegameCountOnDisk - (save ? 0 : 1) > s_savegameIndexBase) {
		GUI_Widget_MakeVisible(w);
	} else {
		GUI_Widget_MakeInvisible(w);
		GUI_Widget_Undraw(w, 233);
	}
}

/**
 * Handles Click event for "Save Game" or "Load Game" button.
 *
 * @param save Wether to save or load.
 * @return True if a game has been saved or loaded, False otherwise.
 */
bool GUI_Widget_SaveLoad_Click(bool save)
{
	WindowDesc *desc = &g_saveLoadWindowDesc;
	bool loop;

	s_savegameCountOnDisk = GetSavegameCount();

	s_savegameIndexBase = max(0, s_savegameCountOnDisk - (save ? 0 : 1));

	FillSavegameDesc(save);

	desc->stringID = save ? STR_SELECT_A_POSITION_TO_SAVE_TO : STR_SELECT_A_SAVED_GAME_TO_LOAD;

	GUI_Window_BackupScreen(desc);

	GUI_Window_Create(desc);

	UpdateArrows(save, true);

	for (loop = true; loop; sleepIdle()) {
		Widget *w = g_widgetLinkedListTail;
		uint16 key = GUI_Widget_HandleEvents(w);

		UpdateArrows(save, false);

		if ((key & 0x8000) != 0) {
			Widget *w2;

			key &= 0x7FFF;
			w2 = GUI_Widget_Get_ByIndex(w, key);

			switch (key) {
				case 0x26:
					s_savegameIndexBase = min(s_savegameCountOnDisk - (save ? 0 : 1), s_savegameIndexBase + 1);

					FillSavegameDesc(save);

					GUI_Widget_DrawAll(w);
					break;

				case 0x27:
					s_savegameIndexBase = max(0, s_savegameIndexBase - 1);

					FillSavegameDesc(save);

					GUI_Widget_DrawAll(w);
					break;

				case 0x23:
					loop = false;
					break;

				default: {
					GUI_Window_RestoreScreen(desc);

					key -= 0x1E;

					if (!save) {
						return SaveGame_LoadFile(GenerateSavegameFilename(s_savegameIndexBase - key));
					}

					if (GUI_Widget_Savegame_Click(key)) return true;

					GUI_Window_BackupScreen(desc);

					UpdateArrows(save, true);

					GUI_Window_Create(desc);

					UpdateArrows(save, true);
				} break;
			}

			GUI_Widget_MakeNormal(w2, false);
		}

		GUI_PaletteAnimate();
	}

	GUI_Window_RestoreScreen(desc);

	return false;
}

/**
 * Handles Click event for "Clear List" button.
 *
 * @param w The widget.
 * @return True, always.
 */
bool GUI_Widget_HOF_ClearList_Click(Widget *w)
{
	/* "Are you sure you want to clear the high scores?" */
	if (GUI_YesNo(STR_ARE_YOU_SURE_YOU_WANT_TO_CLEAR_THE_HIGH_SCORES)) {
		HallOfFameStruct *data = w->data;

		memset(data, 0, 128);

		if (File_Exists_Personal("SAVEFAME.DAT")) File_Delete_Personal("SAVEFAME.DAT");

		GUI_HallOfFame_DrawData(data, true);

		g_doQuitHOF = true;
	}

	GUI_Widget_MakeNormal(w, false);

	return true;
}

/**
 * Handles Click event for "Resume Game" button.
 *
 * @return True, always.
 */
bool GUI_Widget_HOF_Resume_Click(Widget *w)
{
	VARIABLE_NOT_USED(w);

	g_doQuitHOF = true;

	return true;
}

/**
 * Handles Click event for the list in production window.
 *
 * @return True, always.
 */
bool GUI_Production_List_Click(Widget *w)
{
	uint16 selected = w->index - 46;
	bool doubleClick = !g_factoryWindowStarport && selected == s_factoryListLastIndex && g_timerGUI - s_factoryListLastClick <= 30;

	GUI_FactoryWindow_B495_0F30();

	g_factoryWindowSelected = selected;

	GUI_FactoryWindow_DrawDetails();

	GUI_FactoryWindow_UpdateSelection(true);

	s_factoryListLastClick = g_timerGUI;
	s_factoryListLastIndex = selected;

	/* A second click on the same available item confirms exactly the same
	 * action as the Build button.  Starport ordering keeps its explicit
	 * quantity controls, so it remains single-click selection only. */
	if (doubleClick) {
		s_factoryListLastIndex = 0xFFFF;
		GUI_Production_BuildThis_Click(NULL);
	}

	return true;
}

/**
 * Handles Click event for the "Resume Game" button in production window.
 *
 * @return True, always.
 */
bool GUI_Production_ResumeGame_Click(Widget *w)
{
	g_factoryWindowResult = FACTORY_RESUME;

	if (g_factoryWindowStarport) {
		uint8 i = 0;
		House *h = g_playerHouse;
		while (g_factoryWindowOrdered != 0) {
			if (g_factoryWindowItems[i].amount != 0) {
				/* Nothing was taken in a match, so there is nothing to give
				 * back -- only the basket to empty. */
				if (!MpTurn_IsActive()) {
					h->credits += g_factoryWindowItems[i].amount * g_factoryWindowItems[i].credits;
				}
				g_factoryWindowOrdered -= g_factoryWindowItems[i].amount;
				g_factoryWindowItems[i].amount = 0;
			}

			i++;

			GUI_DrawCredits(g_playerHouseID, 0);
		}
	}

	if (w != NULL) GUI_Widget_MakeNormal(w, false);

	return true;
}

/**
 * Handles Click event for the "Ugrade" button in production window.
 *
 * @return True, always.
 */
bool GUI_Production_Upgrade_Click(Widget *w)
{
	GUI_Widget_MakeNormal(w, false);

	g_factoryWindowResult = FACTORY_UPGRADE;

	return true;
}

static void GUI_FactoryWindow_ScrollList(int16 step)
{
	uint16 i;
	uint16 y = 32;

	GUI_FactoryWindow_B495_0F30();

	GUI_Mouse_Hide_Safe();

	for (i = 0; i < 32; i++) {
		y += step;
		GFX_Screen_Copy2(72, y, 72, 16, 32, 136, SCREEN_1, SCREEN_0, false);
	}

	GUI_Mouse_Show_Safe();

	GUI_FactoryWindow_PrepareScrollList();

	GUI_FactoryWindow_UpdateSelection(true);
}

static void GUI_FactoryWindow_FailScrollList(int16 step)
{
	uint16 i;
	uint16 y = 32;

	GUI_FactoryWindow_B495_0F30();

	GUI_Mouse_Hide_Safe();

	GUI_FactoryWindow_B495_0F30();

	for (i = 0; i < 6; i++) {
		y += step;
		GFX_Screen_Copy2(72, y, 72, 16, 32, 136, SCREEN_1, SCREEN_0, false);
	}

	for (i = 0; i < 6; i++) {
		y -= step;
		GFX_Screen_Copy2(72, y, 72, 16, 32, 136, SCREEN_1, SCREEN_0, false);
	}

	GUI_Mouse_Show_Safe();

	GUI_FactoryWindow_UpdateSelection(true);
}

/**
 * Handles Click event for the "Down" button in production window.
 *
 * @return True, always.
 */
bool GUI_Production_Down_Click(Widget *w)
{
	bool drawDetails = false;

	if (g_factoryWindowSelected < 3 && (g_factoryWindowSelected + 1) < g_factoryWindowTotal) {
		g_timerTimeout = 10;
		GUI_FactoryWindow_B495_0F30();
		g_factoryWindowSelected++;

		GUI_FactoryWindow_UpdateSelection(true);

		drawDetails = true;
	} else {
		if (g_factoryWindowBase + 4 < g_factoryWindowTotal) {
			g_timerTimeout = 10;
			g_factoryWindowBase++;
			drawDetails = true;

			GUI_FactoryWindow_ScrollList(1);

			GUI_FactoryWindow_UpdateSelection(true);
		} else {
			GUI_FactoryWindow_DrawDetails();

			GUI_FactoryWindow_FailScrollList(1);
		}
	}

	for (; g_timerTimeout != 0; sleepIdle()) {
		GUI_FactoryWindow_UpdateSelection(false);
	}

	if (drawDetails) GUI_FactoryWindow_DrawDetails();

	GUI_Widget_MakeNormal(w, false);

	return true;
}

/**
 * Handles Click event for the "Up" button in production window.
 *
 * @return True, always.
 */
bool GUI_Production_Up_Click(Widget *w)
{
	bool drawDetails = false;

	if (g_factoryWindowSelected != 0) {
		g_timerTimeout = 10;
		GUI_FactoryWindow_B495_0F30();
		g_factoryWindowSelected--;

		GUI_FactoryWindow_UpdateSelection(true);

		drawDetails = true;
	} else {
		if (g_factoryWindowBase != 0) {
			g_timerTimeout = 10;
			g_factoryWindowBase--;
			drawDetails = true;

			GUI_FactoryWindow_ScrollList(-1);

			GUI_FactoryWindow_UpdateSelection(true);
		} else {
			GUI_FactoryWindow_DrawDetails();

			GUI_FactoryWindow_FailScrollList(-1);
		}
	}

	for (; g_timerTimeout != 0; sleepIdle()) {
		GUI_FactoryWindow_UpdateSelection(false);
	}

	if (drawDetails) GUI_FactoryWindow_DrawDetails();

	GUI_Widget_MakeNormal(w, false);

	return true;
}

static bool GUI_Purchase_CanAfford(void);

static void GUI_Purchase_ShowInvoice(void)
{
	Widget *w = g_widgetInvoiceTail;
	Screen oldScreenID;
	uint16 y = 48;
	uint16 total = 0;
	uint16 x;
	char textBuffer[12];

	oldScreenID = GFX_Screen_SetActive(SCREEN_1);

	GUI_DrawFilledRectangle(128, 48, 311, 159, 20);

	GUI_DrawText_Wrapper(String_Get_ByIndex(STR_ITEM_NAME_QTY_TOTAL), 128, y, 12, 0, 0x11);

	y += 7;

	GUI_DrawLine(129, y, 310, y, 12);

	y += 2;

	if (g_factoryWindowOrdered != 0) {
		uint16 i;

		for (i = 0; i < g_factoryWindowTotal; i++) {
			ObjectInfo *oi;
			uint16 amount;

			if (g_factoryWindowItems[i].amount == 0) continue;

			amount = g_factoryWindowItems[i].amount * g_factoryWindowItems[i].credits;
			total += amount;

			snprintf(textBuffer, sizeof(textBuffer), "%02d %5d", g_factoryWindowItems[i].amount, amount);

			oi = g_factoryWindowItems[i].objectInfo;
			GUI_DrawText_Wrapper(String_Get_ByIndex(oi->stringID_full), 128, y, 8, 0, 0x11);

			GUI_DrawText_Monospace(textBuffer, 311 - (short)strlen(textBuffer) * 6, y, 15, 0, 6);

			y += 8;
		}
	} else {
		GUI_DrawText_Wrapper(String_Get_ByIndex(STR_NO_UNITS_ON_ORDER), 220, 99, 6, 0, 0x112);
	}

	GUI_DrawLine(129, 148, 310, 148, 12);
	GUI_DrawLine(129, 150, 310, 150, 12);

	snprintf(textBuffer, sizeof(textBuffer), "%d", total);

	x = 311 - (short)strlen(textBuffer) * 6;

	/* "Total Cost :" -- in red once the basket has outrun the treasury, which
	 * is the same moment the order button stops being offered. */
	{
		uint8 colour = GUI_Purchase_CanAfford() ? 11 : 6;

		GUI_DrawText_Wrapper(GUI_String_Get_ByIndex(STR_TOTAL_COST_), x - 3, 152, colour, 0, 0x211);
		GUI_DrawText_Monospace(textBuffer, x, 152, colour, 0, 6);
	}

	GUI_Mouse_Hide_Safe();
	GUI_Screen_Copy(16, 48, 16, 48, 23, 112, SCREEN_1, SCREEN_0);
	GUI_Mouse_Show_Safe();

	GFX_Screen_SetActive(SCREEN_0);

	GUI_FactoryWindow_DrawCaption(String_Get_ByIndex(STR_INVOICE_OF_UNITS_ON_ORDER));

	Input_History_Clear();

	for (; GUI_Widget_HandleEvents(w) == 0; sleepIdle()) {
		GUI_DrawCredits(g_playerHouseID, 0);

		GUI_FactoryWindow_UpdateSelection(false);

		GUI_PaletteAnimate();
	}

	GFX_Screen_SetActive(oldScreenID);

	w = GUI_Widget_Get_ByIndex(w, 10);

	if (w != NULL && Mouse_InsideRegion(w->offsetX, w->offsetY, w->offsetX + w->width, w->offsetY + w->height) != 0) {
		while (Input_Test(0x41) != 0 || Input_Test(0x42) != 0) sleepIdle();
		Input_History_Clear();
	}

	if (g_factoryWindowResult == FACTORY_CONTINUE) GUI_FactoryWindow_DrawDetails();
}

/**
 * Handles Click event for the "Invoice" button in starport window.
 *
 * @return True, always.
 */
bool GUI_Purchase_Invoice_Click(Widget *w)
{
	GUI_Widget_MakeInvisible(w);
	GUI_Purchase_ShowInvoice();
	GUI_Widget_MakeVisible(w);
	GUI_Widget_MakeNormal(w, false);
	return true;
}

/**
 * Offer the order button only while the order can be paid for.
 *
 * Called from the window's own loop, because the treasury moves underneath a
 * window that is simply sitting there.  Taking the button away is the whole of
 * the block: an invisible widget takes neither the click nor its shortcut.
 */
void GUI_Purchase_UpdateOrderButton(void)
{
	Widget *w;

	if (!g_factoryWindowStarport) return;

	w = GUI_Widget_Get_ByIndex(g_widgetInvoiceTail, 58);
	if (w == NULL) return;

	if (g_factoryWindowOrdered != 0 && !GUI_Purchase_CanAfford()) {
		if (!w->flags.invisible) GUI_Widget_MakeInvisible(w);
	} else {
		if (w->flags.invisible) GUI_Widget_MakeVisible(w);
	}
}

/**
 * Handles Click event for the "Build this" button in production window.
 *
 * @return True, always.
 */
/** What the basket on the counter comes to. */
static uint16 GUI_Purchase_BasketTotal(void)
{
	uint16 total = 0;
	uint16 i;

	for (i = 0; i < g_factoryWindowTotal && i < 25; i++) {
		total += g_factoryWindowItems[i].amount * g_factoryWindowItems[i].credits;
	}

	return total;
}

/**
 * What the buyer may still spend.
 *
 * Outside a match the treasury drops as the basket fills, so what is left is
 * simply the treasury.  In a match the money does not move until the order
 * does, so the basket has to be subtracted here instead -- otherwise a player
 * could fill it past what they can pay and have the order refused at the far
 * end, which is a desync dressed up as a purchase.
 */
static uint16 GUI_Purchase_CreditsLeft(const House *h)
{
	uint16 basket;

	if (!MpTurn_IsActive()) return h->credits;

	basket = GUI_Purchase_BasketTotal();

	return (h->credits > basket) ? (uint16)(h->credits - basket) : 0;
}

/**
 * Whether the order on the counter can still be paid for.
 *
 * The "+" button refuses to add what the buyer cannot afford, which is enough
 * only while the treasury holds still.  It does not: in a match the money is
 * not taken until the order is sent, and the world keeps running while the
 * window is open, so power maintenance or a factory finishing can take the
 * treasury below a basket that was affordable when it was filled.  Ordering it
 * then would be refused at the far end and the click would simply vanish.
 */
static bool GUI_Purchase_CanAfford(void)
{
	const House *h = g_playerHouse;

	if (h == NULL) return false;
	if (!MpTurn_IsActive()) return true;

	return GUI_Purchase_BasketTotal() <= h->credits;
}

bool GUI_Production_BuildThis_Click(Widget *w)
{
	if (g_factoryWindowStarport) {
		if (g_factoryWindowOrdered == 0 || !GUI_Purchase_CanAfford()) {
			GUI_Widget_MakeInvisible(w);
			GUI_Purchase_ShowInvoice();
			GUI_Widget_MakeVisible(w);
		} else {
			g_factoryWindowResult = FACTORY_BUY;
		}
	} else {
		FactoryWindowItem *item;
		ObjectInfo *oi;

		item = GUI_FactoryWindow_GetItem(g_factoryWindowSelected);
		oi = item->objectInfo;

		if (oi->available > 0) {
			item->amount = 1;
			g_factoryWindowResult = FACTORY_BUY;
		}
	}

	if (w != NULL) GUI_Widget_MakeNormal(w, false);

	return true;
}

/**
 * Handles Click event for the "+" button in starport window.
 *
 * @return True, always.
 */
bool GUI_Purchase_Plus_Click(Widget *w)
{
	FactoryWindowItem *item = GUI_FactoryWindow_GetItem(g_factoryWindowSelected);
	ObjectInfo *oi = item->objectInfo;
	House *h = g_playerHouse;
	bool canCreateMore = true;
	uint16 type = item->objectType;

	GUI_Widget_MakeNormal(w, false);

	if (g_table_unitInfo[type].movementType != MOVEMENT_WINGER && g_table_unitInfo[type].movementType != MOVEMENT_SLITHER) {
		if (g_starPortEnforceUnitLimit && h->unitCount >= h->unitCountMax) canCreateMore = false;
	}

	if (item->amount < oi->available && item->credits <= GUI_Purchase_CreditsLeft(h) && canCreateMore) {
		item->amount++;

		GUI_FactoryWindow_UpdateDetails(item);

		g_factoryWindowOrdered++;

		/* In a match the money moves when the order does.  Taking it here would
		 * be one player's treasury dropping while a window nobody else can see
		 * is open, and putting it back on cancel would be a second such move --
		 * two chances to disagree, over a decision not yet made. */
		if (!MpTurn_IsActive()) h->credits -= item->credits;

		GUI_FactoryWindow_DrawCaption(NULL);
	}

	return true;
}

/**
 * Handles Click event for the "-" button in startport window.
 *
 * @return True, always.
 */
bool GUI_Purchase_Minus_Click(Widget *w)
{
	FactoryWindowItem *item;
	House *h = g_playerHouse;

	GUI_Widget_MakeNormal(w, false);

	item = GUI_FactoryWindow_GetItem(g_factoryWindowSelected);

	if (item->amount != 0) {
		item->amount--;

		GUI_FactoryWindow_UpdateDetails(item);

		g_factoryWindowOrdered--;

		if (!MpTurn_IsActive()) h->credits += item->credits;

		GUI_FactoryWindow_DrawCaption(NULL);
	}

	return true;
}
