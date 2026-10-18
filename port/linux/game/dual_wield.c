/*
DUAL_WIELD.C

Halo 2's dual wielding, for a game on one machine. A player holding a
one-handed weapon (the pistol, the plasma pistol, the needler, the plasma
rifle) who holds the weapon switch (Y) over another takes it in the left
hand; tapping it there still switches weapons. With two, the grenade
trigger fires the left one (no grenades are thrown, and there is no zoom),
the action reloads both, and tapping the switch drops the left one. It is
dropped too when the player dies, gets into a vehicle, or puts away or
swaps the right one for one that is not one-handed.

The left weapon is attached to the unit's left hand and owned by it, as
the weapon in hand is to its right, but is none of its inventory's: the
game's own weapon code updates it with the triggers given here
(weapon_owner_update), and first_person_weapons.c draws it in first person,
the right weapon's animations mirrored. The unit's children are looked at
each tick, so that a saved game brought back finds its left weapon again.
*/

#include "cseries.h"
#include "game/game.h"
#include "game/players.h"
#include "items/items.h"
#include "items/weapons.h"
#include "items/weapon_definitions.h"
#include "objects/objects.h"
#include "objects/object_types.h"
#include "tag_files/tag_groups.h"
#include "units/units.h"

#include "dual_wield.h"

#include <string.h>

/* the platform layer's (port/linux/src) */
void platform_log(char const *format, ...);

/* the game's */
char *tag_get_name(long tag_index);

/* ---------- constants */

enum
{
	/* how long the switch is held to take a weapon in the left hand */
	DUAL_WIELD_HOLD_TICKS = 12,
};

/* how near the player's feet the weapon to take lies (world units) */
#define DUAL_WIELD_REACH 1.2f
#define LEFT_HAND_MARKER "left hand"

/* the one-handed weapons */
static char const *const dual_wield_weapons[] =
{
	"weapons\\pistol\\pistol",
	"weapons\\plasma pistol\\plasma pistol",
	"weapons\\needler\\needler",
	"weapons\\plasma rifle\\plasma rifle",
};

/* ---------- globals */

static struct
{
	long unit_index;
	long left_weapon_index;
	/* the controller's, this tick */
	boolean left_trigger;
	boolean reload;
	/* the switch held over a weapon to take, and the weapon */
	short held_ticks;
	long take_weapon_index;
	boolean take;
	boolean drop;
} dual_wield[MAXIMUM_NUMBER_OF_LOCAL_PLAYERS];

/* ---------- private code */

static boolean dual_wield_active(
	void)
{
	return game_connection() == _game_connection_local;
}

static boolean dual_wield_weapon(
	long weapon_index)
{
	struct weapon_datum *weapon = weapon_index != NONE ? weapon_try_and_get(weapon_index) : NULL;
	char const *name;
	short index;

	if (!weapon || weapon->definition_index == NONE)
		return FALSE;
	name = tag_get_name(weapon->definition_index);
	for (index = 0; index < NUMBEROF(dual_wield_weapons); index++)
	{
		if (!strcmp(name, dual_wield_weapons[index]))
			return TRUE;
	}
	return FALSE;
}

static long dual_wield_current_weapon(
	long unit_index)
{
	struct unit_datum *unit = unit_try_and_get(unit_index);

	if (!unit || unit->unit.current_weapon_index < 0 || unit->unit.current_weapon_index >= MAXIMUM_WEAPONS_PER_UNIT)
		return NONE;
	return unit->unit.weapon_object_indices[unit->unit.current_weapon_index];
}

/* whether the unit can hold a second weapon now: alive, on foot, a
one-handed one in its right hand */
static boolean dual_wield_unit_can(
	long unit_index)
{
	struct unit_datum *unit = unit_index != NONE ? unit_try_and_get(unit_index) : NULL;

	return unit && !TEST_FLAG(unit->object.damage_flags, _object_dead_bit) &&
		unit->object.parent_object_index == NONE && dual_wield_weapon(dual_wield_current_weapon(unit_index));
}

/* a one-handed weapon on the ground at the unit's feet, nearest first */
static long dual_wield_weapon_near(
	long unit_index)
{
	struct unit_datum *unit = unit_get(unit_index);
	struct object_iterator iterator;
	struct weapon_datum *weapon;
	long nearest = NONE;
	real nearest_distance = DUAL_WIELD_REACH * DUAL_WIELD_REACH;

	object_iterator_new(&iterator, _object_mask_weapon, 0);
	while ((weapon = object_iterator_next(&iterator)) != NULL)
	{
		real_vector3d offset;
		real distance;

		if (weapon->object.parent_object_index != NONE || !dual_wield_weapon(iterator.index))
			continue;
		offset.i = weapon->object.position.x - unit->object.position.x;
		offset.j = weapon->object.position.y - unit->object.position.y;
		offset.k = weapon->object.position.z - unit->object.position.z;
		distance = offset.i * offset.i + offset.j * offset.j + offset.k * offset.k;
		if (distance < nearest_distance)
		{
			nearest_distance = distance;
			nearest = iterator.index;
		}
	}
	return nearest;
}

/* the unit's attached weapon that is none of its inventory's: its left */
static long dual_wield_find_left(
	long unit_index)
{
	struct unit_datum *unit = unit_try_and_get(unit_index);
	long child_index;

	if (!unit)
		return NONE;
	for (child_index = unit->object.first_child_object_index; child_index != NONE;
		child_index = object_get(child_index)->object.next_object_index)
	{
		struct object_datum *child = object_get(child_index);
		short slot;
		boolean inventory = FALSE;

		if (child->object.type != _object_type_weapon)
			continue;
		for (slot = 0; slot < MAXIMUM_WEAPONS_PER_UNIT && !inventory; slot++)
			inventory = unit->unit.weapon_object_indices[slot] == child_index;
		if (!inventory)
			return child_index;
	}
	return NONE;
}

static void dual_wield_take(
	short local_player_index,
	long unit_index,
	long weapon_index)
{
	struct weapon_datum *weapon = weapon_try_and_get(weapon_index);

	if (!weapon || weapon->object.parent_object_index != NONE)
		return;
	object_set_garbage(weapon_index, FALSE);
	object_reconnect_to_map(weapon_index, NULL);
	object_set_visibility(weapon_index, TRUE);
	object_attach_to_marker(unit_index, LEFT_HAND_MARKER, weapon_index, "");
	item_in_unit_inventory(weapon_index, unit_index);
	weapon_ready(weapon_index);
	dual_wield[local_player_index].left_weapon_index = weapon_index;
	platform_log("dual wield: %s in the left hand", tag_get_name(weapon->definition_index));
}

static void dual_wield_drop(
	short local_player_index)
{
	long weapon_index = dual_wield[local_player_index].left_weapon_index;
	struct weapon_datum *weapon = weapon_index != NONE ? weapon_try_and_get(weapon_index) : NULL;

	dual_wield[local_player_index].left_weapon_index = NONE;
	if (!weapon)
		return;
	weapon_owner_update(weapon_index, 0, 0.0f);
	item_in_unit_inventory(weapon_index, NONE);
	if (weapon->object.parent_object_index != NONE)
		object_detach(weapon_index);
	weapon->object.translational_velocity = *global_zero_vector3d;
	weapon->object.angular_velocity = *global_zero_vector3d;
	object_set_garbage(weapon_index, TRUE);
	platform_log("dual wield: the left hand's dropped");
}

/* ---------- public code */

void dual_wield_initialize_for_new_map(
	void)
{
	short index;

	memset(dual_wield, 0, sizeof(dual_wield));
	for (index = 0; index < MAXIMUM_NUMBER_OF_LOCAL_PLAYERS; index++)
	{
		dual_wield[index].unit_index = NONE;
		dual_wield[index].left_weapon_index = NONE;
		dual_wield[index].take_weapon_index = NONE;
	}
}

void dual_wield_dispose_from_old_map(
	void)
{
	dual_wield_initialize_for_new_map();
}

void dual_wield_input(
	short local_player_index,
	long unit_index,
	short switch_ticks,
	short grenade_ticks,
	short action_ticks,
	boolean *rotate_weapons,
	boolean *throw_grenade,
	boolean *zoom)
{
	if (local_player_index < 0 || local_player_index >= MAXIMUM_NUMBER_OF_LOCAL_PLAYERS || !dual_wield_active())
		return;
	if (unit_index == NONE || dual_wield[local_player_index].unit_index != unit_index)
	{
		dual_wield[local_player_index].held_ticks = 0;
		dual_wield[local_player_index].left_trigger = FALSE;
		return;
	}
	if (dual_wield[local_player_index].left_weapon_index != NONE)
	{
		/* (two weapons: the grenade trigger the left's, no zoom, the switch
		drops the left) */
		dual_wield[local_player_index].left_trigger = grenade_ticks > 0;
		dual_wield[local_player_index].reload = action_ticks == 1;
		*throw_grenade = FALSE;
		*zoom = FALSE;
		if (switch_ticks == 1)
			dual_wield[local_player_index].drop = TRUE;
		*rotate_weapons = FALSE;
		dual_wield[local_player_index].held_ticks = 0;
		return;
	}
	dual_wield[local_player_index].left_trigger = FALSE;
	/* (one: the switch held over a one-handed weapon takes it; let go
	sooner, it switches weapons as it would have) */
	if (switch_ticks > 0)
	{
		if (switch_ticks == 1)
		{
			dual_wield[local_player_index].take_weapon_index =
				dual_wield_unit_can(unit_index) ? dual_wield_weapon_near(unit_index) : NONE;
		}
		if (dual_wield[local_player_index].take_weapon_index != NONE)
		{
			*rotate_weapons = FALSE;
			dual_wield[local_player_index].held_ticks = switch_ticks;
			if (switch_ticks == DUAL_WIELD_HOLD_TICKS)
				dual_wield[local_player_index].take = TRUE;
		}
	}
	else
	{
		if (dual_wield[local_player_index].take_weapon_index != NONE &&
			dual_wield[local_player_index].held_ticks > 0 &&
			dual_wield[local_player_index].held_ticks < DUAL_WIELD_HOLD_TICKS)
		{
			*rotate_weapons = TRUE;
		}
		dual_wield[local_player_index].held_ticks = 0;
		dual_wield[local_player_index].take_weapon_index = NONE;
	}
}

void dual_wield_update(
	void)
{
	short local_player_index;

	for (local_player_index = 0; local_player_index < MAXIMUM_NUMBER_OF_LOCAL_PLAYERS; local_player_index++)
	{
		long player_index = local_player_get_player_index(local_player_index);
		long unit_index = player_index != NONE && dual_wield_active() ? player_get(player_index)->unit_index : NONE;

		/* (another unit, or none: the last one's left weapon dropped) */
		if (dual_wield[local_player_index].unit_index != unit_index)
		{
			if (dual_wield[local_player_index].left_weapon_index != NONE)
				dual_wield_drop(local_player_index);
			dual_wield[local_player_index].unit_index = unit_index;
			dual_wield[local_player_index].take = FALSE;
			dual_wield[local_player_index].drop = FALSE;
		}
		if (unit_index == NONE)
			continue;
		/* (the unit's own children say which it holds: a saved game's) */
		dual_wield[local_player_index].left_weapon_index = dual_wield_find_left(unit_index);
		if (dual_wield[local_player_index].left_weapon_index != NONE &&
			(dual_wield[local_player_index].drop || !dual_wield_unit_can(unit_index)))
		{
			dual_wield_drop(local_player_index);
		}
		dual_wield[local_player_index].drop = FALSE;
		if (dual_wield[local_player_index].take)
		{
			long weapon_index = dual_wield[local_player_index].take_weapon_index;

			dual_wield[local_player_index].take = FALSE;
			if (dual_wield[local_player_index].left_weapon_index == NONE && dual_wield_unit_can(unit_index) &&
				dual_wield_weapon(weapon_index))
			{
				dual_wield_take(local_player_index, unit_index, weapon_index);
			}
		}
		if (dual_wield[local_player_index].left_weapon_index != NONE)
		{
			long weapon_index = dual_wield[local_player_index].left_weapon_index;
			struct weapon_datum *weapon = weapon_get(weapon_index);
			word flags = 0;

			if (dual_wield[local_player_index].left_trigger)
				SET_FLAG(flags, _weapon_control_primary_trigger_bit, TRUE);
			/* (the action reloads it, as an empty one being fired does) */
			if (dual_wield[local_player_index].reload ||
				(dual_wield[local_player_index].left_trigger && weapon->weapon.magazines[0].rounds_loaded == 0))
			{
				SET_FLAG(flags, _weapon_control_reload_bit, TRUE);
			}
			weapon_owner_update(weapon_index, flags, dual_wield[local_player_index].left_trigger ? 1.0f : 0.0f);
		}
		dual_wield[local_player_index].reload = FALSE;
	}
}

long dual_wield_left_weapon(
	long unit_index)
{
	short local_player_index;

	if (unit_index == NONE)
		return NONE;
	for (local_player_index = 0; local_player_index < MAXIMUM_NUMBER_OF_LOCAL_PLAYERS; local_player_index++)
	{
		if (dual_wield[local_player_index].unit_index == unit_index)
			return dual_wield[local_player_index].left_weapon_index;
	}
	return NONE;
}
