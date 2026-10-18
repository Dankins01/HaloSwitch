/*
SURVIVAL.C

Survival: a campaign level's ground held against waves of its enemies.

New Game's list offers SURVIVAL besides the campaign's levels and the
multiplayer maps (menu_functions.c). An arena is a level and the ground
around where its player starts: the game is that level with none of its
scripts (no cutscenes, no mission) and none of its actors placed. Instead,
after a few seconds, the first wave comes: actors of the level's own squads
whose starting locations are near the player, placed on free ground around
those locations (encounters.c's encounter_survival_place, which spreads
them as network co-op's extra enemies are spread), and told where the
player is. A wave killed, the player is healed, the weapons carried are
filled with ammunition, the grenades topped up, and after a pause the next,
larger wave comes. Grunts and Jackals come first; Elites from the second
wave, more of them as the waves go on; every fifth wave brings Hunters
where the level has them. The difficulty chosen sets the enemies' own
strength, as in the campaign, and adds to each wave's size.

The player has one life: dying ends the game, with the waves survived and
the enemies killed on the HUD, and goes back to the main menu. In split
screen, a player who dies comes back as in the campaign's co-op while the
other lives; both dead, the game is over.

Only a local game (one machine) plays survival.
*/

/* ---------- headers */

#include "cseries.h"
#include "ai/actor_types.h"
#include "ai/ai_scenario_definitions.h"
#include "ai/ai_script.h"
#include "game/game.h"
#include "game/players.h"
#include "interface/hud_messaging.h"
#include "items/weapons.h"
#include "main/main.h"
#include "math/real_math.h"
#include "objects/objects.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#include "units/units.h"

#include "survival.h"

#include <stdarg.h>
#include <string.h>

void platform_log(char const *format, ...);
char const *config_string(char const *name);
double config_real(char const *name);
void unit_scripting_set_current_vitality_of(long unit_index, real body_vitality, real shield_vitality);

/* ---------- constants */

enum
{
	/* the squads a game's waves can come from */
	MAXIMUM_SURVIVAL_SQUADS = 96,
	/* the encounters they belong to */
	MAXIMUM_SURVIVAL_ENCOUNTERS = 64,

	SURVIVAL_FIRST_WAVE_DELAY = 8 * TICKS_PER_SECOND,
	SURVIVAL_INTERMISSION = 12 * TICKS_PER_SECOND,
	/* how often the living are counted, and the player's place told */
	SURVIVAL_COUNT_PERIOD = TICKS_PER_SECOND / 2,
	SURVIVAL_REVEAL_PERIOD = 4 * TICKS_PER_SECOND,
	/* a wave whose last few survive this long is over anyway (hidden or
	stuck where nobody finds them), and any wave this long: the next comes,
	and those left fight on */
	SURVIVAL_STRAGGLER_TIME = 60 * TICKS_PER_SECOND,
	SURVIVAL_STRAGGLERS = 3,
	SURVIVAL_WAVE_TIME_LIMIT = 150 * TICKS_PER_SECOND,

	SURVIVAL_MAXIMUM_WAVE_SIZE = 30,
	SURVIVAL_MAXIMUM_GRENADES = 2,
};

/* what a squad sends: the waves' tiers */
enum
{
	_survival_tier_light = 0, /* grunts, jackals, infection forms */
	_survival_tier_heavy, /* elites, combat forms, sentinels */
	_survival_tier_boss, /* hunters */
	NUMBER_OF_SURVIVAL_TIERS
};

enum
{
	_survival_state_none = 0,
	_survival_state_waiting_for_player,
	_survival_state_intermission,
	_survival_state_wave,
	_survival_state_over,
};

/* ---------- structures */

struct survival_arena_definition
{
	char const *map_name;
	wchar_t const *display_name;
	/* how far from where the player starts a squad's starting locations can
	be (world units: about 3 metres), and the most it widens to when the
	level has too few squads so near */
	real radius;
	real maximum_radius;
};

struct survival_squad
{
	long encounter_index;
	short encounter_absolute_index;
	short squad_index;
	short actor_type;
	short tier;
	real distance;
};

/* ---------- globals */

static struct survival_arena_definition const survival_arenas[NUMBER_OF_SURVIVAL_ARENAS] =
{
	{
		"levels\\b30\\b30",
		/* (the beach where the Pelicans land) */
		L"THE SILENT CARTOGRAPHER",
		30.0f,
		120.0f,
	},
};

static struct
{
	short requested_arena;
	boolean active;
	/* the game restarted (the pause menu's Restart): the same arena again */
	boolean restart;
	short arena;
	short state;
	long state_time;
	long last_count_time;
	long last_reveal_time;
	short wave;
	short wave_placed;
	/* the living when the wave came (its own and any left from the last
	wave), and now: the wave's kills the difference */
	short wave_start_living;
	short wave_living;
	long kills;
	real_point3d center;
	unsigned long seed;
	short squad_count;
	struct survival_squad squads[MAXIMUM_SURVIVAL_SQUADS];
	short tier_counts[NUMBER_OF_SURVIVAL_TIERS];
	short encounter_count;
	long encounters[MAXIMUM_SURVIVAL_ENCOUNTERS];
	boolean lost;
} survival = { NONE };

/* ---------- private code */

static short survival_tier(
	short actor_type)
{
	switch (actor_type)
	{
	case _actor_grunt:
	case _actor_jackal:
	case _actor_infection_form:
		return _survival_tier_light;
	case _actor_elite:
	case _actor_assassin:
	case _actor_combat_form:
	case _actor_sentinel:
		return _survival_tier_heavy;
	case _actor_hunter:
		return _survival_tier_boss;
	default:
		/* (the player's side, and what isn't a fighter) */
		return NONE;
	}
}

/* the local players' living units (split screen's both): their number */
static short survival_player_units(
	long units[MAXIMUM_NUMBER_OF_LOCAL_PLAYERS])
{
	short count = 0;
	short local_player_index;

	for (local_player_index = 0; local_player_index < MAXIMUM_NUMBER_OF_LOCAL_PLAYERS; local_player_index++)
	{
		long player_index = local_player_get_player_index(local_player_index);

		if (player_index != NONE && player_get(player_index)->unit_index != NONE)
			units[count++] = player_get(player_index)->unit_index;
	}
	return count;
}

/* the first of them, NONE when every one is dead */
static long survival_player_unit(
	void)
{
	long units[MAXIMUM_NUMBER_OF_LOCAL_PLAYERS];

	return survival_player_units(units) > 0 ? units[0] : NONE;
}

/* a message on the players' HUDs (split screen's both), from ASCII */
static void survival_message(
	char const *format,
	...)
{
	char text[128];
	wchar_t wide[128];
	va_list arguments;
	short i;
	short local_player_index;

	va_start(arguments, format);
	_vsnprintf(text, sizeof(text) - 1, format, arguments);
	va_end(arguments);
	text[sizeof(text) - 1] = 0;
	for (i = 0; text[i] && i < NUMBEROF(wide) - 1; i++)
		wide[i] = (wchar_t)(unsigned char)text[i];
	wide[i] = 0;
	for (local_player_index = 0; local_player_index < MAXIMUM_NUMBER_OF_LOCAL_PLAYERS; local_player_index++)
	{
		if (local_player_get_player_index(local_player_index) != NONE)
			hud_print_message(local_player_index, wide);
	}
	platform_log("survival: %s", text);
}

static real survival_distance_squared(
	real_point3d const *a,
	real_point3d const *b)
{
	real dx = a->x - b->x, dy = a->y - b->y, dz = a->z - b->z;

	return dx * dx + dy * dy + dz * dz;
}

static void survival_add_encounter(
	long encounter_index)
{
	short i;

	for (i = 0; i < survival.encounter_count; i++)
	{
		if (survival.encounters[i] == encounter_index)
			return;
	}
	if (survival.encounter_count < MAXIMUM_SURVIVAL_ENCOUNTERS)
		survival.encounters[survival.encounter_count++] = encounter_index;
}

/* the level's squads of enemies whose starting locations are within
`radius` of the arena's center, in the structure the player is in */
static void survival_select_squads(
	real radius)
{
	struct scenario *scenario = global_scenario_get();
	short encounter_index;

	survival.squad_count = 0;
	survival.encounter_count = 0;
	csmemset(survival.tier_counts, 0, sizeof(survival.tier_counts));
	for (encounter_index = 0; encounter_index < scenario->ai_encounters.count; encounter_index++)
	{
		struct encounter_definition *encounter = TAG_BLOCK_GET_ELEMENT(&scenario->ai_encounters, encounter_index,
			struct encounter_definition);
		long datum_index;
		short squad_index;

		if (encounter->runtime_structure_bsp_reference_index != NONE &&
			encounter->runtime_structure_bsp_reference_index != global_structure_bsp_index)
		{
			continue;
		}
		datum_index = encounter_survival_index(encounter_index);
		if (datum_index == NONE)
			continue;
		for (squad_index = 0; squad_index < encounter->squads.count; squad_index++)
		{
			struct squad_definition *squad = TAG_BLOCK_GET_ELEMENT(&encounter->squads, squad_index,
				struct squad_definition);
			short actor_type = encounter_survival_squad_actor_type(encounter_index, squad_index);
			short tier = survival_tier(actor_type);
			real nearest = radius * radius;
			boolean near = FALSE;
			short location_index;

			if (tier == NONE || squad->starting_locations.count == 0)
				continue;
			for (location_index = 0; location_index < squad->starting_locations.count; location_index++)
			{
				struct actor_starting_location *location = TAG_BLOCK_GET_ELEMENT(&squad->starting_locations,
					location_index, struct actor_starting_location);
				real distance = survival_distance_squared(&location->position, &survival.center);

				if (distance <= nearest)
				{
					nearest = distance;
					near = TRUE;
				}
			}
			if (!near || survival.squad_count >= MAXIMUM_SURVIVAL_SQUADS)
				continue;
			{
				struct survival_squad *entry = &survival.squads[survival.squad_count++];

				entry->encounter_index = datum_index;
				entry->encounter_absolute_index = encounter_index;
				entry->squad_index = squad_index;
				entry->actor_type = actor_type;
				entry->tier = tier;
				entry->distance = square_root(nearest);
				survival.tier_counts[tier]++;
				survival_add_encounter(datum_index);
			}
		}
	}
}

/* a random squad of the tier (NONE: none) */
static short survival_random_squad(
	short tier)
{
	short count = survival.tier_counts[tier];
	short pick;
	short i;

	if (count == 0)
		return NONE;
	pick = seed_random_range(&survival.seed, 0, count);
	for (i = 0; i < survival.squad_count; i++)
	{
		if (survival.squads[i].tier == tier && pick-- == 0)
			return i;
	}
	return NONE;
}

static void survival_reveal_player(
	void)
{
	long units[MAXIMUM_NUMBER_OF_LOCAL_PLAYERS];
	short unit_count = survival_player_units(units);
	short i, j;

	for (i = 0; unit_count > 0 && i < survival.encounter_count; i++)
	{
		short absolute_index = (short)DATUM_INDEX_TO_ABSOLUTE_INDEX(survival.encounters[i]);

		/* (an AI reference to the whole encounter: type 0 in the top bits):
		the wave knows where the players are, may charge, and attacks rather
		than holding its ground */
		for (j = 0; j < unit_count; j++)
			ai_scripting_magically_see_unit((long)absolute_index, units[j]);
		ai_scripting_allow_charge((long)absolute_index, TRUE);
		ai_scripting_attack((long)absolute_index);
	}
}

static short survival_living_count(
	void)
{
	short count = 0;
	short i;

	for (i = 0; i < survival.encounter_count; i++)
		count += encounter_survival_living_count(survival.encounters[i]);
	return count;
}

static void survival_spawn_wave(
	void)
{
	short wave = survival.wave;
	short budget = (short)MIN(4 + 2 * wave, SURVIVAL_MAXIMUM_WAVE_SIZE);
	short attempts;
	short placed_by_squad[MAXIMUM_SURVIVAL_SQUADS];
	boolean boss_wave = wave % 5 == 0 && survival.tier_counts[_survival_tier_boss] > 0;
	real heavy_chance = wave < 2 ? 0.0f : MIN(0.15f * (real)wave, 0.6f);

	switch (game_difficulty_level_get())
	{
	case _game_difficulty_level_hard:
		budget = (short)MIN(budget + 2, SURVIVAL_MAXIMUM_WAVE_SIZE);
		break;
	case _game_difficulty_level_impossible:
		budget = (short)MIN(budget + 4, SURVIVAL_MAXIMUM_WAVE_SIZE);
		break;
	}
	csmemset(placed_by_squad, 0, sizeof(placed_by_squad));
	survival.wave_placed = 0;
	for (attempts = 0; budget > 0 && attempts < 40; attempts++)
	{
		short tier;
		short group;
		short squad_index;
		struct survival_squad *squad;
		short placed;

		if (boss_wave)
		{
			tier = _survival_tier_boss;
			boss_wave = FALSE;
		}
		else if (real_seed_random_range(&survival.seed, 0.0f, 1.0f) < heavy_chance &&
			survival.tier_counts[_survival_tier_heavy] > 0)
		{
			tier = _survival_tier_heavy;
		}
		else
		{
			tier = survival.tier_counts[_survival_tier_light] > 0 ? _survival_tier_light : _survival_tier_heavy;
		}
		squad_index = survival_random_squad(tier);
		if (squad_index == NONE)
			break;
		squad = &survival.squads[squad_index];
		switch (tier)
		{
		case _survival_tier_light:
			group = seed_random_range(&survival.seed, 3, 6);
			break;
		case _survival_tier_heavy:
			group = (short)MIN(1 + wave / 3, 4);
			break;
		default:
			group = 2;
			break;
		}
		group = (short)MIN(group, budget);
		placed = encounter_survival_place(squad->encounter_index, squad->squad_index, group,
			placed_by_squad[squad_index]);
		placed_by_squad[squad_index] += group;
		survival.wave_placed += placed;
		budget -= group;
	}
	survival.wave_start_living = survival.wave_living = survival_living_count();
	survival_reveal_player();
	platform_log("survival: wave %d: %d enemies placed", survival.wave, survival.wave_placed);
}

/* the players healed, the weapons carried filled, the grenades topped up */
static void survival_resupply(
	void)
{
	long units[MAXIMUM_NUMBER_OF_LOCAL_PLAYERS];
	short unit_count = survival_player_units(units);
	short player, i;

	for (player = 0; player < unit_count; player++)
	{
		struct unit_datum *unit = unit_try_and_get(units[player]);

		if (!unit)
			continue;
		unit_scripting_set_current_vitality_of(units[player], 1.0e6f, 1.0e6f);
		for (i = 0; i < MAXIMUM_WEAPONS_PER_UNIT; i++)
		{
			long weapon_index = unit->unit.weapon_object_indices[i];

			if (weapon_index != NONE)
			{
				short rounds[2] = { SHORT_MAX, SHORT_MAX };

				weapon_set_total_rounds(weapon_index, rounds);
			}
		}
		for (i = 0; i < NUMBER_OF_UNIT_GRENADE_TYPES; i++)
			unit->unit.grenade_counts[i] = (char)MAX(unit->unit.grenade_counts[i], SURVIVAL_MAXIMUM_GRENADES);
	}
}

static void survival_set_state(
	short state)
{
	survival.state = state;
	survival.state_time = game_time_get();
}

static void survival_begin(
	long unit_index)
{
	struct survival_arena_definition const *arena = &survival_arenas[survival.arena];
	real radius = arena->radius;
	short tier;

	survival.center = object_get(unit_index)->object.position;
	survival.seed = (unsigned long)game_time_get() * 2654435761u ^ 0x5eed5eedu;
	do
	{
		survival_select_squads(radius);
		if (survival.tier_counts[_survival_tier_light] + survival.tier_counts[_survival_tier_heavy] >= 4)
			break;
		radius *= 1.5f;
	}
	while (radius <= arena->maximum_radius);
	platform_log("survival: arena at %.1f %.1f %.1f, %d squads within %.0f (light %d, heavy %d, boss %d), %d encounters",
		survival.center.x, survival.center.y, survival.center.z, survival.squad_count, radius,
		survival.tier_counts[_survival_tier_light], survival.tier_counts[_survival_tier_heavy],
		survival.tier_counts[_survival_tier_boss], survival.encounter_count);
	for (tier = 0; tier < survival.squad_count; tier++)
	{
		struct survival_squad const *squad = &survival.squads[tier];
		struct encounter_definition *encounter = TAG_BLOCK_GET_ELEMENT(&global_scenario_get()->ai_encounters,
			squad->encounter_absolute_index, struct encounter_definition);
		struct squad_definition *squad_definition = TAG_BLOCK_GET_ELEMENT(&encounter->squads, squad->squad_index,
			struct squad_definition);

		platform_log("survival:   %s/%s: actor type %d, tier %d, %.0f away", encounter->name,
			squad_definition->name, squad->actor_type, squad->tier, squad->distance);
	}
	survival.wave = 1;
	survival.kills = 0;
	survival_set_state(_survival_state_intermission);
	if (survival.squad_count == 0)
	{
		survival_message("SURVIVAL: no enemies on this ground");
		survival_set_state(_survival_state_over);
		return;
	}
	survival_message("SURVIVAL: wave 1 in %d seconds", SURVIVAL_FIRST_WAVE_DELAY / TICKS_PER_SECOND);
}

/* ---------- public code */

void survival_request(
	short arena)
{
	survival.requested_arena = VALID_INDEX(arena, NUMBER_OF_SURVIVAL_ARENAS) ? arena : NONE;
}

short survival_arena_count(
	void)
{
	return NUMBER_OF_SURVIVAL_ARENAS;
}

char const *survival_arena_map_name(
	short arena)
{
	return VALID_INDEX(arena, NUMBER_OF_SURVIVAL_ARENAS) ? survival_arenas[arena].map_name : NULL;
}

wchar_t const *survival_arena_display_name(
	short arena)
{
	return VALID_INDEX(arena, NUMBER_OF_SURVIVAL_ARENAS) ? survival_arenas[arena].display_name : L"";
}

boolean survival_active(
	void)
{
	return survival.active;
}

boolean survival_requested(
	void)
{
	return survival.requested_arena != NONE;
}

boolean survival_initialize_for_new_map(
	char const *map_name)
{
	short arena = survival.requested_arena;

	if (arena == NONE && survival.restart)
		arena = survival.arena;
	survival.restart = FALSE;
	survival.active = FALSE;
	survival.state = _survival_state_none;
	/* (debug.survival: a test's, the arena whose map name ends so) */
	if (arena == NONE && map_name && config_string("debug.survival")[0])
	{
		char const *test = config_string("debug.survival");
		short index;

		for (index = 0; index < NUMBER_OF_SURVIVAL_ARENAS; index++)
		{
			char const *name = survival_arenas[index].map_name;
			size_t length = strlen(name), test_length = strlen(test);

			if (length >= test_length && !_stricmp(name + length - test_length, test) && !_stricmp(map_name, name))
				arena = index;
		}
	}
	if (arena == NONE || !map_name)
		return TRUE;
	/* (the main menu's map loads between the request and the level; any
	other map takes the request, played as survival only if it is the
	arena's, in a local game) */
	if (!_stricmp(map_name, "levels\\ui\\ui"))
		return TRUE;
	survival.requested_arena = NONE;
	if (_stricmp(map_name, survival_arenas[arena].map_name) != 0 || game_connection() != _game_connection_local)
		return TRUE;
	survival.active = TRUE;
	survival.arena = arena;
	survival.lost = FALSE;
	survival.squad_count = 0;
	survival.encounter_count = 0;
	/* (the game's clock is not yet running: no time taken) */
	survival.state = _survival_state_waiting_for_player;
	survival.state_time = 0;
	platform_log("survival: %s (no scripts, no actors of the level's)", map_name);
	return FALSE;
}

void survival_dispose_from_old_map(
	void)
{
	/* (the map's game restarted, main_reset_map's: the next is this one
	again; going to the main menu instead, it is the menu's map) */
	survival.restart = survival.active;
	survival.active = FALSE;
	survival.state = _survival_state_none;
}

void survival_update(
	void)
{
	long now = game_time_get();
	long unit_index;

	if (!survival.active || game_connection() != _game_connection_local)
		return;
	unit_index = survival_player_unit();
	/* (the players dead, split screen's both: the game is over, main.c's
	survival_player_lost going back to the main menu once the death has been
	seen) */
	if (unit_index == NONE && (survival.state == _survival_state_intermission || survival.state == _survival_state_wave))
	{
		if (!survival.lost)
		{
			survival.lost = TRUE;
			if (survival.state == _survival_state_wave)
				survival.kills += survival.wave_start_living - survival.wave_living;
			survival_message("YOU SURVIVED %d WAVE%s, %ld KILLS", survival.wave - 1,
				survival.wave - 1 == 1 ? "" : "S", survival.kills);
		}
		return;
	}
	switch (survival.state)
	{
	case _survival_state_waiting_for_player:
		if (unit_index != NONE)
			survival_begin(unit_index);
		break;

	case _survival_state_intermission:
	{
		long delay = survival.wave == 1 ? SURVIVAL_FIRST_WAVE_DELAY : SURVIVAL_INTERMISSION;

		if (now - survival.state_time >= delay)
		{
			survival_message("WAVE %d", survival.wave);
			survival_spawn_wave();
			survival.last_count_time = survival.last_reveal_time = now;
			survival_set_state(_survival_state_wave);
		}
		break;
	}

	case _survival_state_wave:
		if (now - survival.last_reveal_time >= SURVIVAL_REVEAL_PERIOD)
		{
			survival.last_reveal_time = now;
			survival_reveal_player();
		}
		/* (debug.survival_clear_after: a test's, the wave killed) */
		if (config_real("debug.survival_clear_after") > 0.0 &&
			now - survival.state_time >= (long)(config_real("debug.survival_clear_after") * TICKS_PER_SECOND) &&
			survival.wave_living > 0)
		{
			short i;

			for (i = 0; i < survival.encounter_count; i++)
				ai_scripting_kill((long)DATUM_INDEX_TO_ABSOLUTE_INDEX(survival.encounters[i]));
		}
		if (now - survival.last_count_time >= SURVIVAL_COUNT_PERIOD)
		{
			short living = survival_living_count();

			survival.last_count_time = now;
			if (living != survival.wave_living)
				platform_log("survival: wave %d: %d of %d left", survival.wave, living, survival.wave_start_living);
			survival.wave_living = living;
			if (living == 0 || (living <= SURVIVAL_STRAGGLERS && now - survival.state_time >= SURVIVAL_STRAGGLER_TIME) ||
				now - survival.state_time >= SURVIVAL_WAVE_TIME_LIMIT)
			{
				survival.kills += survival.wave_start_living - living;
				survival_message("WAVE %d CLEARED: next wave in %d seconds", survival.wave,
					SURVIVAL_INTERMISSION / TICKS_PER_SECOND);
				survival_resupply();
				survival.wave++;
				survival_set_state(_survival_state_intermission);
			}
		}
		break;
	}
}

boolean survival_player_lost(
	void)
{
	if (!survival.active)
		return FALSE;
	platform_log("survival: over, %d waves survived, %ld kills", MAX(survival.wave - 1, 0), survival.kills);
	survival.active = FALSE;
	survival.state = _survival_state_none;
	main_goto_main_menu();
	return TRUE;
}
