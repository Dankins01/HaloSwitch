/*
PORT_HUD.C

Lines the port adds to the HUD (hud.c's hud_draw_screen calls in here):
a survival game's line on top (the next wave's countdown, else the wave's
enemies left, and the points: survival.c's) and its Health & Overshield
charges banked, and, dual wielding, the left
hand's weapon's ammunition under the shields (the right hand's is the
weapon's own HUD's: dual_wield.c).
*/

#include "cseries.h"
#include "game/players.h"
#include "items/weapon_definitions.h"
#include "items/weapons.h"
#include "objects/objects.h"
#include "rasterizer/rasterizer.h"
#include "render/render.h"
#include "text/draw_string.h"
#include "text/font_group.h"
#include "text/unicode.h"
#include "units/units.h"

#include "dual_wield.h"
#include "survival.h"

#include <stdio.h>

long hud_get_font_index(void);

/* ---------- constants */

/* draw_string's justifications */
enum
{
	_justify_left = 0,
	_justify_right,
	_justify_center,
};

/* ---------- private code */

/* a line of ASCII in the HUD's font, its top this fraction of the view's
height down, justified so */
static void port_hud_line(
	char const *ascii,
	real top,
	short justification,
	real_argb_color const *color)
{
	long font_index = hud_get_font_index();
	struct font_header *font;
	rectangle2d bounds;
	wchar_t text[128];
	short height, i;

	if (font_index == NONE)
		return;
	for (i = 0; ascii[i] && i < NUMBEROF(text) - 1; i++)
		text[i] = (wchar_t)(unsigned char)ascii[i];
	text[i] = 0;
	font = font_definition_get(font_index);
	height = (short)(font->ascending_height + font->descending_height);
	bounds = render.camera.window_bounds;
	bounds.y0 = (short)(bounds.y0 + top * (real)(bounds.y1 - bounds.y0));
	bounds.y1 = (short)(bounds.y0 + 2 * height);
	/* (a margin at the sides, as the HUD's own) */
	bounds.x0 = (short)(bounds.x0 + (bounds.x1 - bounds.x0) / 32);
	bounds.x1 = (short)(bounds.x1 - (bounds.x1 - bounds.x0) / 32);
	draw_string_set_draw_mode(font_index, NONE, justification, 0, color);
	rasterizer_draw_unicode_string(&bounds, NULL, NULL, 0, text);
}

/* the left hand's weapon's rounds (loaded, then spare), else its battery
and heat */
static void port_hud_left_weapon(
	long unit_index)
{
	real_argb_color color = { 1.0f, 0.55f, 0.78f, 1.0f };
	long weapon_index = dual_wield_left_weapon(unit_index);
	struct weapon_datum *weapon;
	struct weapon_definition *definition;
	char text[64];

	if (weapon_index == NONE || !(weapon = object_try_and_get_and_verify_type(weapon_index, _object_mask_weapon)))
		return;
	definition = weapon_definition_get(weapon->definition_index);
	if (definition->weapon.magazines.count > 0)
	{
		struct weapon_magazine const *magazine = &weapon->weapon.magazines[0];

		snprintf(text, sizeof(text), "LEFT   %d | %d", magazine->rounds_loaded,
			MAX(magazine->rounds_total - magazine->rounds_loaded, 0));
	}
	else
	{
		snprintf(text, sizeof(text), "LEFT   BATTERY %d%%   HEAT %d%%",
			(int)(100.0f * PIN(1.0f - weapon->weapon.age, 0.0f, 1.0f) + 0.5f),
			(int)(100.0f * PIN(weapon->weapon.heat, 0.0f, 1.0f) + 0.5f));
	}
	port_hud_line(text, 0.13f, _justify_right, &color);
}

/* ---------- public code */

/* hud.c's hud_draw_screen, the HUD shown and the player's unit alive */
void port_hud_draw(
	short local_player_index)
{
	real_argb_color white = { 1.0f, 1.0f, 1.0f, 1.0f };
	long player_index = local_player_get_player_index(local_player_index);
	long unit_index = player_index != NONE ? player_get(player_index)->unit_index : NONE;
	char text[128];

	if (unit_index == NONE)
		return;
	if (survival_hud_status(text, sizeof(text)))
		port_hud_line(text, 0.035f, _justify_center, &white);
	if (survival_charges(local_player_index) > 0)
	{
		real_argb_color green = { 1.0f, 0.55f, 1.0f, 0.55f };

		snprintf(text, sizeof(text), "D-PAD UP: HEALTH & OVERSHIELD x%d", survival_charges(local_player_index));
		port_hud_line(text, 0.075f, _justify_center, &green);
	}
	port_hud_left_weapon(unit_index);
}
