/*
SURVIVAL.C

Survival: a campaign level's ground held against waves of its enemies.

New Game's list offers SURVIVAL besides the campaign's levels and the
multiplayer maps (menu_functions.c): an arena on each campaign level. An
arena is a level and a piece of its ground, where the players are moved to
(the structure BSP it is on loaded first), or where the level starts them:
the game is that level with none of its scripts (no cutscenes, no mission)
and none of its actors placed. Instead, after a few seconds, the first wave
comes: actors of the level's own squads whose starting locations are near
the players, placed on free ground around those locations (encounters.c's
encounter_survival_place, which spreads them as network co-op's extra
enemies are spread), and told where the players are. A wave killed, the player is healed, the weapons carried are
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
#include "ai/actors.h"
#include "ai/ai_scenario_definitions.h"
#include "ai/ai_script.h"
#include "effects/effects.h"
#include "game/game.h"
#include "input/input.h"
#include "game/game_allegiance.h"
#include "game/game_globals.h"
#include "game/players.h"
#include "interface/hud_messaging.h"
#include "items/weapons.h"
#include "main/main.h"
#include "math/real_math.h"
#include "objects/damage.h"
#include "objects/objects.h"
#include "physics/collisions.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#include "tag_files/tag_groups.h"
#include "units/unit_definitions.h"
#include "units/units.h"

#include "survival.h"

#include <stdarg.h>
#include <string.h>

void platform_log(char const *format, ...);
#include <stdio.h>
#include <stdlib.h>
char const *config_string(char const *name);
double config_real(char const *name);
long config_integer(char const *name);
void unit_scripting_set_current_vitality_of(long unit_index, real body_vitality, real shield_vitality);
char *tag_get_name(long tag_index);
short objects_port_collect_old_garbage(short keep, short maximum);

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

	/* the longest a structure BSP switch is waited for */
	SURVIVAL_MOVE_TIME_LIMIT = 5 * TICKS_PER_SECOND,

	SURVIVAL_MAXIMUM_WAVE_SIZE = 30,
	SURVIVAL_MAXIMUM_GRENADES = 2,

	/* the enemies whose deaths are scored (each wave's and those left) */
	MAXIMUM_SURVIVAL_TRACKED_ACTORS = 160,
	/* the Marine squads the shop's fireteams come from */
	MAXIMUM_SURVIVAL_MARINE_SQUADS = 16,
	/* the waves' vehicles kept: past these, the oldest empty one out of
	sight goes as another comes */
	MAXIMUM_SURVIVAL_VEHICLES = 4,
	SURVIVAL_FIRETEAM_SIZE = 3,
	/* the points for clearing a wave, times its number */
	SURVIVAL_WAVE_BONUS = 25,
	/* the most grenades of a kind the shop's fill to */
	SURVIVAL_SHOP_MAXIMUM_GRENADES = 4,

	/* the cleanup: how often, the newest bodies and dropped items kept, and
	the most of the older (unseen) let go each time */
	SURVIVAL_CLEANUP_PERIOD = 2 * TICKS_PER_SECOND,
	SURVIVAL_GARBAGE_KEPT = 12,
	SURVIVAL_GARBAGE_COLLECTED = 3,
};

/* the nearest a squad's starting location can be to the arena's center
(world units): its enemies are not placed on top of the players */
#define SURVIVAL_MINIMUM_DISTANCE 4.0f
/* how near the arena's center one of an encounter's firing positions must
be for its squads to be chosen (its actors move only between its firing
positions: from farther, they would never come to the players), where the
level has enough such */
#define SURVIVAL_FIRING_POSITION_RANGE 8.0f

/* what a squad sends: the waves' tiers */
enum
{
	_survival_tier_light = 0, /* grunts, jackals, infection and carrier forms */
	_survival_tier_heavy, /* elites, combat forms, sentinels */
	_survival_tier_boss, /* hunters */
	NUMBER_OF_SURVIVAL_TIERS
};

enum
{
	_survival_state_none = 0,
	_survival_state_waiting_for_player,
	/* waiting for the arena's structure BSP, to move the players there */
	_survival_state_moving,
	_survival_state_intermission,
	_survival_state_wave,
	_survival_state_over,
};

/* ---------- structures */

struct survival_arena_definition
{
	char const *map_name;
	/* where the players stand: on this structure BSP, at this point (above
	the ground they drop to); else (FALSE) where the level starts them */
	boolean moved;
	short structure_bsp_index;
	real_point3d position;
	/* how far from there a squad's starting locations can be (world units:
	about 3 metres), the most it widens to when the level has too few squads
	so near, and how far above or below (another floor's, else) */
	real radius;
	real maximum_radius;
	real maximum_height;
};

struct survival_squad
{
	long encounter_index;
	short encounter_absolute_index;
	short squad_index;
	short actor_type;
	short tier;
	real distance;
	/* its starting location nearest the arena's center */
	real_point3d nearest;
	/* the actor palette's entry its actors are placed as (forced Hunters:
	another squad's places), NONE for its own */
	short actor_palette_index;
};

/* the shop's goods (survival_shop_buy) */
enum
{
	_survival_good_weapon = 0, /* dropped at the player's feet */
	_survival_good_equipment, /* dropped on the player: a powerup taken at once */
	_survival_good_frag_grenades,
	_survival_good_plasma_grenades,
	_survival_good_ammunition,
	_survival_good_fireteam,
	_survival_good_vehicle, /* put down in front, where there is room */
	/* the run's options, before its first wave: a skull turned on or off
	(group_tag: which), the Covenant's vehicles allowed or not */
	_survival_good_skull,
	_survival_good_vehicles_option,
	/* a charge banked, used with the D-pad's up: full health and an
	overshield */
	_survival_good_health_charge,
};

/* the most Health & Overshield charges banked */
#define SURVIVAL_MAXIMUM_CHARGES 3

/* the skulls: each makes the run harder, its points worth more */
enum
{
	_survival_skull_black_eye = 0, /* shields recharge only on a melee kill */
	_survival_skull_famine, /* half the ammunition from what is picked up, none between waves */
	_survival_skull_tough_luck, /* the enemies dive away more */
	_survival_skull_mythic, /* the enemies twice as tough */
	_survival_skull_catch, /* the enemies throw many more grenades */
	NUMBER_OF_SURVIVAL_SKULLS
};

static struct
{
	char const *name;
	real multiplier;
} const survival_skulls[NUMBER_OF_SURVIVAL_SKULLS] =
{
	{ "BLACK EYE", 1.5f },
	{ "FAMINE", 1.5f },
	{ "TOUGH LUCK", 1.25f },
	{ "MYTHIC", 1.5f },
	{ "CATCH", 1.25f },
};

/* a testing build's: the weapons free (the prices in survival_goods kept
for when it is FALSE again) */
#define SURVIVAL_WEAPONS_FREE_FOR_TESTING TRUE

/* the shop's favourites: the last goods bought, first in its list */
#define SURVIVAL_FAVOURITES 3
/* the profile (the save folder's): the skulls and options chosen, the
favourites, each arena's best run */
#define SURVIVAL_PROFILE_NAME "survival.txt"

struct survival_good
{
	char const *name;
	long price;
	short kind;
	unsigned long group_tag;
	char const *tag_name;
};

/* ---------- globals */

/* the shop's goods, those the level has (its weapons and powerups) offered */
static struct survival_good const survival_goods[] =
{
	{ "MARINE FIRETEAM", 400, _survival_good_fireteam, 0, NULL },
	{ "HEALTH & OVERSHIELD", 300, _survival_good_health_charge, 0, NULL },
	{ "ACTIVE CAMOUFLAGE", 350, _survival_good_equipment, 'eqip', "powerups\\active camouflage" },
	{ "FRAG GRENADES", 75, _survival_good_frag_grenades, 'eqip', "weapons\\frag grenade\\frag grenade" },
	{ "PLASMA GRENADES", 75, _survival_good_plasma_grenades, 'eqip', "weapons\\plasma grenade\\plasma grenade" },
	{ "AMMUNITION", 100, _survival_good_ammunition, 0, NULL },
	{ "PISTOL", 100, _survival_good_weapon, 'weap', "weapons\\pistol\\pistol" },
	{ "ASSAULT RIFLE", 150, _survival_good_weapon, 'weap', "weapons\\assault rifle\\assault rifle" },
	/* (Halo 2's: weapon_tags.c, from the player's h2weapons.pak) */
	{ "SMG", 175, _survival_good_weapon, 'weap', "weapons\\h2 smg\\smg" },
	{ "BATTLE RIFLE", 250, _survival_good_weapon, 'weap', "weapons\\h2 battle rifle\\battle rifle" },
	{ "PLASMA PISTOL", 100, _survival_good_weapon, 'weap', "weapons\\plasma pistol\\plasma pistol" },
	{ "PLASMA RIFLE", 200, _survival_good_weapon, 'weap', "weapons\\plasma rifle\\plasma rifle" },
	{ "NEEDLER", 200, _survival_good_weapon, 'weap', "weapons\\needler\\needler" },
	{ "SHOTGUN", 300, _survival_good_weapon, 'weap', "weapons\\shotgun\\shotgun" },
	{ "SNIPER RIFLE", 450, _survival_good_weapon, 'weap', "weapons\\sniper rifle\\sniper rifle" },
	{ "FUEL ROD GUN", 500, _survival_good_weapon, 'weap', "weapons\\fuel rod gun\\fuel rod" },
	{ "ROCKET LAUNCHER", 600, _survival_good_weapon, 'weap', "weapons\\rocket launcher\\rocket launcher" },
	{ "H2 ROCKET LAUNCHER", 650, _survival_good_weapon, 'weap', "weapons\\h2 rocket launcher\\rocket launcher" },
	{ "ENERGY SWORD", 500, _survival_good_weapon, 'weap', "weapons\\h2 energy sword\\energy sword" },
	{ "WARTHOG", 1000, _survival_good_vehicle, 'vehi', "vehicles\\warthog\\warthog" },
	/* (before the first wave) */
	{ "COVENANT VEHICLES", 0, _survival_good_vehicles_option, 0, NULL },
	{ "BLACK EYE", 0, _survival_good_skull, _survival_skull_black_eye, NULL },
	{ "FAMINE", 0, _survival_good_skull, _survival_skull_famine, NULL },
	{ "TOUGH LUCK", 0, _survival_good_skull, _survival_skull_tough_luck, NULL },
	{ "MYTHIC", 0, _survival_good_skull, _survival_skull_mythic, NULL },
	{ "CATCH", 0, _survival_good_skull, _survival_skull_catch, NULL },
};

/* the profile: kept from run to run (survival_profile_load and _save) */
static struct
{
	boolean loaded;
	unsigned long skulls;
	boolean no_vehicles;
	short favourites[SURVIVAL_FAVOURITES];
	struct
	{
		short wave;
		long points;
		long kills;
	} best[NUMBER_OF_SURVIVAL_ARENAS];
} survival_profile;

/* (the places: a ground squad's starting location where the level's
encounters fight, chosen for their squads' numbers and kinds; in the
levels' order, as New Game lists them) */
static struct survival_arena_definition const survival_arenas[NUMBER_OF_SURVIVAL_ARENAS] =
{
	/* the Pillar of Autumn's cafeteria: Elites and Grunts */
	{ "levels\\a10\\a10", TRUE, 1, { -17.5f, -6.8f, 0.0f }, 25.0f, 50.0f, 2.5f },
	/* the first Covenant camp, over the ridge from the lifeboat */
	{ "levels\\a30\\a30", TRUE, 0, { 53.9f, -14.1f, 87.45f }, 35.0f, 90.0f, 15.0f },
	/* the canyon below the Truth and Reconciliation: Grunts, Jackals,
	Elites and a Hunter */
	{ "levels\\a50\\a50", TRUE, 0, { 3.4f, 12.4f, 98.46f }, 30.0f, 80.0f, 12.0f },
	/* the beach where the Pelicans land */
	{ "levels\\b30\\b30", FALSE, 0, { 0.0f, 0.0f, 0.0f }, 30.0f, 120.0f, 100.0f },
	/* the zigzag canyon before the second bridge: Elites and Hunters */
	{ "levels\\b40\\b40", TRUE, 2, { 301.9f, -652.3f, 62.14f }, 35.0f, 90.0f, 12.0f },
	/* the swamp facility's halls: the Flood's first attack */
	{ "levels\\c10\\c10", TRUE, 2, { -14.4f, -23.9f, 0.0f }, 30.0f, 60.0f, 3.0f },
	/* the Library's floor: the Flood and its carriers */
	{ "levels\\c20\\c20", TRUE, 0, { 1.8f, 15.1f, -4.6f }, 30.0f, 60.0f, 3.0f },
	/* the Covenant and the Flood at once, with a Hunter */
	{ "levels\\c40\\c40", TRUE, 0, { 38.4f, -61.6f, -0.3f }, 30.0f, 60.0f, 4.0f },
	/* the Truth and Reconciliation's halls: the Covenant against the Flood */
	{ "levels\\d20\\d20", TRUE, 3, { 0.5f, -48.6f, -80.8f }, 30.0f, 60.0f, 3.0f },
	/* the Pillar of Autumn's halls: the Covenant's Hunters, the Flood and
	Sentinels */
	{ "levels\\d40\\d40", TRUE, 1, { -3.2f, -2.3f, 0.0f }, 30.0f, 60.0f, 3.0f },
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
	long last_cleanup_time;
	long cleaned;
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

	/* the points: each enemy's death scored, by its kind (survival_score) */
	long points;
	short tracked_count;
	struct
	{
		long actor_index;
		short actor_type;
		short living;
	} tracked[MAXIMUM_SURVIVAL_TRACKED_ACTORS];

	/* the shop: the goods offered (survival_goods' indices), the Marines'
	squads, and what the last purchase did */
	short good_count;
	short goods[NUMBEROF(survival_goods)];
	/* the options, then the favourites, first in the list */
	short option_count;
	short favourite_count;
	short marine_squad_count;
	struct survival_squad marine_squads[MAXIMUM_SURVIVAL_MARINE_SQUADS];
	short marine_encounter_count;
	long marine_encounters[MAXIMUM_SURVIVAL_MARINE_SQUADS];
	short fireteams;
	/* each local player's Health & Overshield charges */
	short charges[MAXIMUM_NUMBER_OF_LOCAL_PLAYERS];
	/* debug.survival_buy's goods bought (a test's) */
	boolean test_bought;
	char shop_status[64];

	/* the wave the run began at, whether its first has come (the options
fixed then), and its points' multiplier (the skulls') */
	short first_wave;
	boolean begun;
	real multiplier;
	/* the wave's theme (a boss wave's), its name */
	char const *theme;
	/* the Phantom bringing some of the wave: where it flies from, hovers
	and leaves to, how far along, and the squad it brings */
	long phantom_index;
	short phantom_phase;
	long phantom_time;
	real_point3d phantom_from;
	real_point3d phantom_hover;
	real_point3d phantom_to;
	real_vector3d phantom_forward;
	short phantom_squad;
	boolean phantom_tried;
	/* the wave's enemies still to come, one at a time (each teleported in),
	and what comes once they all have: the wave's vehicles and turrets */
	short pending_count;
	short pending_next;
	struct
	{
		short squad_index;
		short extra;
	} pending[SURVIVAL_MAXIMUM_WAVE_SIZE];
	long last_arrival_time;
	boolean extras_pending;
	short wing_kind;
	/* the waves' vehicles, oldest first */
	short vehicle_count;
	long vehicles[MAXIMUM_SURVIVAL_VEHICLES];
} survival = { NONE };

/* ---------- private code */

static void survival_shop_stock(
	void);

static short survival_tier(
	short actor_type)
{
	switch (actor_type)
	{
	case _actor_grunt:
	case _actor_jackal:
	case _actor_infection_form:
	case _actor_carrier_form:
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

/* what a good costs now */
static long survival_good_cost(
	struct survival_good const *good)
{
	if (SURVIVAL_WEAPONS_FREE_FOR_TESTING && good->kind == _survival_good_weapon)
		return 0;
	return good->price;
}

/* ---------- the profile */

static void survival_profile_path(
	char *path,
	size_t size)
{
	/* (the game's paths are the Xbox's: its saves' drive, the save folder's) */
	snprintf(path, size, "u:\\%s", SURVIVAL_PROFILE_NAME);
}

static void survival_profile_load(
	void)
{
	char path[1024], line[256];
	FILE *file;
	short i;

	if (survival_profile.loaded)
		return;
	csmemset(&survival_profile, 0, sizeof(survival_profile));
	survival_profile.loaded = TRUE;
	for (i = 0; i < SURVIVAL_FAVOURITES; i++)
		survival_profile.favourites[i] = NONE;
	survival_profile_path(path, sizeof(path));
	file = fopen(path, "r");
	if (!file)
		return;
	while (fgets(line, sizeof(line), file))
	{
		unsigned long value;
		int a, b, c, d;
		long points, kills;
		char name[64];

		if (sscanf(line, "skulls %lu", &value) == 1)
			survival_profile.skulls = value & ((1UL << NUMBER_OF_SURVIVAL_SKULLS) - 1);
		else if (sscanf(line, "vehicles %d", &a) == 1)
			survival_profile.no_vehicles = !a;
		else if (sscanf(line, "favourites %d %d %d", &a, &b, &c) == 3)
		{
			int list[SURVIVAL_FAVOURITES] = { a, b, c };

			for (i = 0; i < SURVIVAL_FAVOURITES; i++)
				survival_profile.favourites[i] = (short)(VALID_INDEX(list[i], NUMBEROF(survival_goods)) ? list[i] : NONE);
		}
		else if (sscanf(line, "best %63s %d %ld %ld", name, &d, &points, &kills) == 4)
		{
			for (i = 0; i < NUMBER_OF_SURVIVAL_ARENAS; i++)
			{
				if (!_stricmp(survival_arenas[i].map_name, name))
				{
					survival_profile.best[i].wave = (short)d;
					survival_profile.best[i].points = points;
					survival_profile.best[i].kills = kills;
				}
			}
		}
	}
	fclose(file);
}

static void survival_profile_save(
	void)
{
	char path[1024];
	FILE *file;
	short i;

	survival_profile_path(path, sizeof(path));
	file = fopen(path, "w");
	if (!file)
	{
		platform_log("survival: could not write %s", path);
		return;
	}
	fprintf(file, "skulls %lu\nvehicles %d\nfavourites %d %d %d\n", survival_profile.skulls,
		!survival_profile.no_vehicles, survival_profile.favourites[0], survival_profile.favourites[1],
		survival_profile.favourites[2]);
	for (i = 0; i < NUMBER_OF_SURVIVAL_ARENAS; i++)
	{
		if (survival_profile.best[i].wave > 0)
		{
			fprintf(file, "best %s %d %ld %ld\n", survival_arenas[i].map_name, survival_profile.best[i].wave,
				survival_profile.best[i].points, survival_profile.best[i].kills);
		}
	}
	fclose(file);
}

static boolean survival_skull(
	short skull)
{
	return survival.active && TEST_FLAG(survival_profile.skulls, skull);
}

/* the points for the skulls on */
static real survival_skulls_multiplier(
	void)
{
	real multiplier = 1.0f;
	short skull;

	for (skull = 0; skull < NUMBER_OF_SURVIVAL_SKULLS; skull++)
	{
		if (TEST_FLAG(survival_profile.skulls, skull))
			multiplier *= survival_skulls[skull].multiplier;
	}
	return multiplier;
}

static long survival_scaled(
	long points)
{
	return (long)((real)points * survival.multiplier + 0.5f);
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

static void survival_add_marine_encounter(
	long encounter_index)
{
	short i;

	for (i = 0; i < survival.marine_encounter_count; i++)
	{
		if (survival.marine_encounters[i] == encounter_index)
			return;
	}
	if (survival.marine_encounter_count < MAXIMUM_SURVIVAL_MARINE_SQUADS)
		survival.marine_encounters[survival.marine_encounter_count++] = encounter_index;
}

/* the actor palette's entry of the actor variant named (its tag's name ends
so), NONE if the level has none */
static short survival_actor_palette_find(
	char const *ending)
{
	struct tag_block *palette = &global_scenario_get()->ai_actor_palette;
	size_t ending_length = strlen(ending);
	short index;

	for (index = 0; index < palette->count; index++)
	{
		struct tag_reference *reference = TAG_BLOCK_GET_ELEMENT(palette, index, struct tag_reference);
		char const *name = reference->index != NONE ? tag_get_name(reference->index) : NULL;
		size_t length = name ? strlen(name) : 0;

		if (name && length >= ending_length && !_stricmp(name + length - ending_length, ending))
			return index;
	}
	return NONE;
}

/* every fifth wave's Hunters where none of the level's are near: a pair
placed in a Covenant squad's places, as Hunters (the level must have them,
somewhere: their tags are in its map only then) */
static void survival_force_hunters(
	void)
{
	short palette_index = survival_actor_palette_find("characters\\hunter\\hunter");
	short added = 0;
	short i;

	if (survival.tier_counts[_survival_tier_boss] > 0 || palette_index == NONE)
		return;
	/* (two hosts at most: a heavy squad's, else a light one's, of the
	Covenant: Elites, Grunts or Jackals) */
	for (i = 0; i < survival.squad_count && added < 2 && survival.squad_count < MAXIMUM_SURVIVAL_SQUADS; i++)
	{
		struct survival_squad const *host = &survival.squads[i];

		if (host->actor_palette_index == NONE &&
			(host->actor_type == _actor_elite || host->actor_type == _actor_grunt || host->actor_type == _actor_jackal))
		{
			struct survival_squad *entry = &survival.squads[survival.squad_count++];

			*entry = *host;
			entry->actor_type = _actor_hunter;
			entry->tier = _survival_tier_boss;
			entry->actor_palette_index = palette_index;
			survival.tier_counts[_survival_tier_boss]++;
			added++;
		}
	}
	if (added)
		platform_log("survival: Hunters forced into %d squads' places", added);
}

/* the level's squads of enemies whose starting locations are within
`radius` of the arena's center (but not on top of it) and within
`maximum_height` above or below it, in the structure the player is in */
/* whether one of the encounter's firing positions is near the arena's
center */
static boolean survival_encounter_fights_near(
	struct encounter_definition *encounter,
	real maximum_height)
{
	short index;

	for (index = 0; index < encounter->firing_positions.count; index++)
	{
		struct firing_position_definition *position = TAG_BLOCK_GET_ELEMENT(&encounter->firing_positions, index,
			struct firing_position_definition);
		real height = position->position.z - survival.center.z;

		if (survival_distance_squared(&position->position, &survival.center) <=
				SURVIVAL_FIRING_POSITION_RANGE * SURVIVAL_FIRING_POSITION_RANGE &&
			height <= maximum_height && height >= -maximum_height)
		{
			return TRUE;
		}
	}
	return FALSE;
}

static void survival_select_squads(
	real radius,
	real maximum_height,
	boolean fighting_near)
{
	struct scenario *scenario = global_scenario_get();
	short encounter_index;

	survival.squad_count = 0;
	survival.encounter_count = 0;
	survival.marine_squad_count = 0;
	survival.marine_encounter_count = 0;
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
		/* (an enemy encounter must fight near; a Marines' may come from
		farther) */
		if (fighting_near && !survival_encounter_fights_near(encounter, maximum_height))
		{
			boolean marines = FALSE;

			for (squad_index = 0; squad_index < encounter->squads.count && !marines; squad_index++)
				marines = encounter_survival_squad_actor_type(encounter_index, squad_index) == _actor_marine;
			if (!marines)
				continue;
		}
		for (squad_index = 0; squad_index < encounter->squads.count; squad_index++)
		{
			struct squad_definition *squad = TAG_BLOCK_GET_ELEMENT(&encounter->squads, squad_index,
				struct squad_definition);
			short actor_type = encounter_survival_squad_actor_type(encounter_index, squad_index);
			short tier = survival_tier(actor_type);
			real nearest = radius * radius;
			boolean near = FALSE;
			real_point3d nearest_point = { 0 };
			short location_index;

			if (actor_type == _actor_marine && squad->starting_locations.count > 0 &&
				survival.marine_squad_count < MAXIMUM_SURVIVAL_MARINE_SQUADS)
			{
				/* (the shop's fireteams: the Marines' squads that are near,
				widely) */
				short location_index;

				for (location_index = 0; location_index < squad->starting_locations.count; location_index++)
				{
					struct actor_starting_location *location = TAG_BLOCK_GET_ELEMENT(&squad->starting_locations,
						location_index, struct actor_starting_location);
					real height = location->position.z - survival.center.z;

					if (survival_distance_squared(&location->position, &survival.center) <= 4.0f * radius * radius &&
						height <= maximum_height && height >= -maximum_height)
					{
						struct survival_squad *entry = &survival.marine_squads[survival.marine_squad_count++];

						csmemset(entry, 0, sizeof(*entry));
						entry->encounter_index = datum_index;
						entry->encounter_absolute_index = encounter_index;
						entry->squad_index = squad_index;
						entry->actor_type = actor_type;
						entry->actor_palette_index = NONE;
						entry->nearest = location->position;
						survival_add_marine_encounter(datum_index);
						break;
					}
				}
				continue;
			}
			if (tier == NONE || squad->starting_locations.count == 0)
				continue;
			for (location_index = 0; location_index < squad->starting_locations.count; location_index++)
			{
				struct actor_starting_location *location = TAG_BLOCK_GET_ELEMENT(&squad->starting_locations,
					location_index, struct actor_starting_location);
				real distance = survival_distance_squared(&location->position, &survival.center);
				real height = location->position.z - survival.center.z;

				if (distance <= nearest && height <= maximum_height && height >= -maximum_height &&
					distance >= SURVIVAL_MINIMUM_DISTANCE * SURVIVAL_MINIMUM_DISTANCE)
				{
					nearest = distance;
					nearest_point = location->position;
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
				entry->nearest = nearest_point;
				entry->actor_palette_index = NONE;
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

/* the points for one of a kind of enemy's deaths */
static long survival_kill_points(
	short actor_type)
{
	switch (actor_type)
	{
	case _actor_infection_form: return 3;
	case _actor_grunt: return 10;
	case _actor_carrier_form: return 10;
	case _actor_jackal: return 15;
	case _actor_combat_form: return 30;
	case _actor_sentinel: return 30;
	case _actor_elite: return 50;
	case _actor_assassin: return 60;
	case _actor_hunter: return 150;
	default: return 0;
	}
}

/* the enemies' deaths since the last look, scored by their kinds: each of
the encounters' actors' living units (a swarm's) against the last look's,
an actor gone counting as all of its dead; the new ones looked at from now
on */
static void survival_score(
	void)
{
	long actor_indices[MAXIMUM_SURVIVAL_TRACKED_ACTORS];
	short actor_types[MAXIMUM_SURVIVAL_TRACKED_ACTORS];
	short living[MAXIMUM_SURVIVAL_TRACKED_ACTORS];
	short count = 0;
	short i, j;

	for (i = 0; i < survival.encounter_count && count < MAXIMUM_SURVIVAL_TRACKED_ACTORS; i++)
	{
		count += encounter_survival_actors(survival.encounters[i], actor_indices + count, actor_types + count,
			living + count, (short)(MAXIMUM_SURVIVAL_TRACKED_ACTORS - count));
	}
	for (i = 0; i < survival.tracked_count; i++)
	{
		short now = 0;

		for (j = 0; j < count; j++)
		{
			if (actor_indices[j] == survival.tracked[i].actor_index)
			{
				now = living[j];
				break;
			}
		}
		if (now < survival.tracked[i].living)
			survival.points += survival_scaled((survival.tracked[i].living - now) * survival_kill_points(survival.tracked[i].actor_type));
		survival.tracked[i].living = now;
	}
	/* (the dead let go; the new looked at) */
	for (i = j = 0; i < survival.tracked_count; i++)
	{
		if (survival.tracked[i].living > 0)
			survival.tracked[j++] = survival.tracked[i];
	}
	survival.tracked_count = j;
	for (j = 0; j < count; j++)
	{
		boolean known = FALSE;

		if (living[j] <= 0)
			continue;
		for (i = 0; i < survival.tracked_count && !known; i++)
			known = survival.tracked[i].actor_index == actor_indices[j];
		if (!known && survival.tracked_count < MAXIMUM_SURVIVAL_TRACKED_ACTORS)
		{
			survival.tracked[survival.tracked_count].actor_index = actor_indices[j];
			survival.tracked[survival.tracked_count].actor_type = actor_types[j];
			survival.tracked[survival.tracked_count].living = living[j];
			survival.tracked_count++;
		}
	}
}

static boolean survival_vehicle_place(struct unit_datum *unit, real length, real width, real_point3d *place,
	real_vector3d *forward);
static boolean survival_blocked(real_point3d const *a, real_point3d const *b);

/* the Covenant's vehicles a wave can bring, from its first wave on, on an
arena open enough (its height), each driven by one of the wave's Elites */
static struct
{
	char const *name;
	char const *tag_name;
	short first_wave;
	real length;
	real width;
} const survival_vehicles[] =
{
	{ "GHOST", "vehicles\\ghost\\ghost", 3, 2.6f, 1.6f },
	{ "BANSHEE", "vehicles\\banshee\\banshee", 5, 3.2f, 3.0f },
	{ "WRAITH", "vehicles\\wraith\\wraith", 7, 5.0f, 3.4f },
};

/* the Covenant's turrets, manned by a wave's Grunts or Jackals (any arena,
from the fourth wave, every other) */
#define SURVIVAL_TURRET_TAG "vehicles\\c gun turret\\c gun turret"
#define SURVIVAL_TURRET_FIRST_WAVE 4

/* the wave's enemies come one at a time, this often, each in the co-op
teleport's flash */
#define SURVIVAL_ARRIVAL_PERIOD (TICKS_PER_SECOND * 2 / 5)
#define SURVIVAL_ARRIVAL_EFFECT "effects\\coop teleport"

/* the Phantom: its tag, how long it flies in, hovers once the wave has
come and flies off, how high it hovers above its squad's place and how far
off it comes from (world units) */
#define SURVIVAL_PHANTOM_TAG "vehicles\\c_dropship\\c_dropship"
#define SURVIVAL_PHANTOM_ARRIVAL (7 * TICKS_PER_SECOND)
#define SURVIVAL_PHANTOM_HOVER (6 * TICKS_PER_SECOND)
#define SURVIVAL_PHANTOM_DEPARTURE (8 * TICKS_PER_SECOND)
#define SURVIVAL_PHANTOM_HEIGHT 4.5f
#define SURVIVAL_PHANTOM_DISTANCE 45.0f
#define SURVIVAL_PHANTOM_CLIMB 22.0f

enum
{
	_survival_phantom_none = 0,
	_survival_phantom_arriving,
	_survival_phantom_hovering,
	_survival_phantom_leaving,
};

/* the arenas with room for vehicles: the open ones */
#define SURVIVAL_VEHICLE_ARENA_HEIGHT 10.0f
/* how far from any player a vehicle comes */
#define SURVIVAL_VEHICLE_PLAYER_DISTANCE 8.0f

/* the vehicle kept with the waves': room made by letting go of the gone,
then of the oldest empty one no player sees (else the oldest kept is just
forgotten) */
static void survival_vehicle_keep(
	long vehicle_index)
{
	short i, j;

	for (i = j = 0; i < survival.vehicle_count; i++)
	{
		if (object_try_and_get_and_verify_type(survival.vehicles[i], _object_mask_vehicle))
			survival.vehicles[j++] = survival.vehicles[i];
	}
	survival.vehicle_count = j;
	if (survival.vehicle_count == MAXIMUM_SURVIVAL_VEHICLES)
	{
		for (i = 0; i < survival.vehicle_count; i++)
		{
			struct object_datum *vehicle = object_get(survival.vehicles[i]);

			if (vehicle->object.first_child_object_index == NONE && !object_visible_to_any_player(survival.vehicles[i]))
			{
				object_delete(survival.vehicles[i]);
				break;
			}
		}
		/* (none: the oldest forgotten, left where it is) */
		for (i = MIN(i, survival.vehicle_count - 1); i + 1 < survival.vehicle_count; i++)
			survival.vehicles[i] = survival.vehicles[i + 1];
		survival.vehicle_count--;
	}
	survival.vehicles[survival.vehicle_count++] = vehicle_index;
}

/* the Elites of the wave without a vehicle, put into some the level has: on
odd waves from the third, one (two from the seventh), the kind picked from
those the wave has reached */
static void survival_spawn_vehicles(
	short only_kind,
	short wanted_override)
{
	struct survival_arena_definition const *arena = &survival_arenas[survival.arena];
	long actor_indices[MAXIMUM_SURVIVAL_TRACKED_ACTORS];
	short actor_types[MAXIMUM_SURVIVAL_TRACKED_ACTORS];
	short living[MAXIMUM_SURVIVAL_TRACKED_ACTORS];
	long kinds[NUMBEROF(survival_vehicles)];
	short kind_count = 0, count = 0, wanted, made = 0;
	unsigned long used_kinds = 0;
	short i;

	if (arena->maximum_height < SURVIVAL_VEHICLE_ARENA_HEIGHT || survival_profile.no_vehicles)
		return;
	if (only_kind == NONE && (survival.wave < 3 || !(survival.wave & 1)))
		return;
	for (i = 0; i < NUMBEROF(survival_vehicles); i++)
	{
		kinds[i] = (only_kind == NONE ? survival.wave >= survival_vehicles[i].first_wave : i == only_kind) ?
			tag_loaded('vehi', survival_vehicles[i].tag_name) : NONE;
		kind_count += kinds[i] != NONE;
	}
	if (!kind_count)
		return;
	wanted = wanted_override > 0 ? wanted_override : survival.wave >= 7 ? 2 : 1;
	for (i = 0; i < survival.encounter_count && count < MAXIMUM_SURVIVAL_TRACKED_ACTORS; i++)
	{
		count += encounter_survival_actors(survival.encounters[i], actor_indices + count, actor_types + count,
			living + count, (short)(MAXIMUM_SURVIVAL_TRACKED_ACTORS - count));
	}
	for (i = 0; i < count && made < wanted; i++)
	{
		struct actor_datum *actor;
		struct unit_datum *unit;
		struct unit_definition *definition;
		struct object_placement_data data;
		real_point3d place;
		real_vector3d forward;
		long vehicle_index;
		short pick, kind, seat;

		if (actor_types[i] != _actor_elite || living[i] <= 0)
			continue;
		actor = actor_get(actor_indices[i]);
		unit = actor->meta.unit_index != NONE ? unit_try_and_get(actor->meta.unit_index) : NULL;
		if (!unit || unit->object.parent_object_index != NONE)
			continue;
		/* (a kind the level has, the newest reached the likelier) */
		pick = seed_random_range(&survival.seed, 0, kind_count);
		for (kind = (short)NUMBEROF(survival_vehicles) - 1; kind >= 0; kind--)
		{
			if (kinds[kind] != NONE && pick-- == 0)
				break;
		}
		/* (a second of another kind, if the level has another; never two of
		a kind) */
		if (kind >= 0 && TEST_FLAG(used_kinds, kind) && only_kind == NONE)
		{
			short other;

			for (other = 0; other < NUMBEROF(survival_vehicles); other++)
			{
				if (kinds[other] != NONE && !TEST_FLAG(used_kinds, other))
					break;
			}
			if (other == NUMBEROF(survival_vehicles))
				break;
			kind = other;
		}
		if (kind < 0)
			continue;
		if (!survival_vehicle_place(unit, survival_vehicles[kind].length, survival_vehicles[kind].width, &place, &forward))
		{
			platform_log("survival: no room for a %s by an Elite at %.1f %.1f %.1f", survival_vehicles[kind].name,
				unit->object.position.x, unit->object.position.y, unit->object.position.z);
			continue;
		}
		/* (not on top of a player) */
		{
			long units[MAXIMUM_NUMBER_OF_LOCAL_PLAYERS];
			short unit_count = survival_player_units(units), player;
			boolean near = FALSE;

			for (player = 0; player < unit_count && !near; player++)
			{
				near = survival_distance_squared(&place, &unit_get(units[player])->object.position) <
					SURVIVAL_VEHICLE_PLAYER_DISTANCE * SURVIVAL_VEHICLE_PLAYER_DISTANCE;
			}
			if (near)
				continue;
		}
		object_placement_data_new(&data, kinds[kind], NONE);
		data.position = place;
		data.forward = forward;
		data.up.i = 0.0f;
		data.up.j = 0.0f;
		data.up.k = 1.0f;
		vehicle_index = object_new(&data);
		if (vehicle_index == NONE)
			continue;
		definition = unit_definition_get(unit_get(vehicle_index)->definition_index);
		for (seat = 0; seat < definition->unit.seats.count; seat++)
		{
			if (unit_seat_is_driver(vehicle_index, seat))
				break;
		}
		if (seat >= definition->unit.seats.count || !unit_enter_seat(actor->meta.unit_index, vehicle_index, seat))
		{
			platform_log("survival: an Elite could not drive a %s", survival_vehicles[kind].name);
			object_delete(vehicle_index);
			continue;
		}
		made++;
		SET_FLAG(used_kinds, kind, TRUE);
		survival_vehicle_keep(vehicle_index);
		if (only_kind == NONE)
			survival_message("A %s IS COMING", survival_vehicles[kind].name);
		platform_log("survival: wave %d: a %s at %.1f %.1f %.1f", survival.wave, survival_vehicles[kind].name,
			place.x, place.y, place.z);
	}
}

/* the wave's Grunts and Jackals manning turrets the level has: on even
waves from the fourth, one (two from the eighth), put down by them */
static void survival_spawn_turrets(
	void)
{
	long actor_indices[MAXIMUM_SURVIVAL_TRACKED_ACTORS];
	short actor_types[MAXIMUM_SURVIVAL_TRACKED_ACTORS];
	short living[MAXIMUM_SURVIVAL_TRACKED_ACTORS];
	long turret_tag = tag_loaded('vehi', SURVIVAL_TURRET_TAG);
	short count = 0, wanted, made = 0, i;

	if (turret_tag == NONE || survival_profile.no_vehicles || survival.wave < SURVIVAL_TURRET_FIRST_WAVE ||
		(survival.wave & 1))
	{
		return;
	}
	wanted = survival.wave >= 8 ? 2 : 1;
	for (i = 0; i < survival.encounter_count && count < MAXIMUM_SURVIVAL_TRACKED_ACTORS; i++)
	{
		count += encounter_survival_actors(survival.encounters[i], actor_indices + count, actor_types + count,
			living + count, (short)(MAXIMUM_SURVIVAL_TRACKED_ACTORS - count));
	}
	for (i = 0; i < count && made < wanted; i++)
	{
		struct actor_datum *actor;
		struct unit_datum *unit;
		struct unit_definition *definition;
		struct object_placement_data data;
		real_point3d place;
		real_vector3d forward;
		long turret_index;
		short seat;

		if ((actor_types[i] != _actor_grunt && actor_types[i] != _actor_jackal) || living[i] <= 0)
			continue;
		actor = actor_get(actor_indices[i]);
		unit = actor->meta.unit_index != NONE ? unit_try_and_get(actor->meta.unit_index) : NULL;
		if (!unit || unit->object.parent_object_index != NONE || !survival_vehicle_place(unit, 1.6f, 1.6f, &place, &forward))
			continue;
		object_placement_data_new(&data, turret_tag, NONE);
		data.position = place;
		/* (facing the arena's center) */
		data.forward.i = survival.center.x - place.x;
		data.forward.j = survival.center.y - place.y;
		data.forward.k = 0.0f;
		if (normalize3d(&data.forward) == 0.0f)
			data.forward = forward;
		data.up.i = 0.0f;
		data.up.j = 0.0f;
		data.up.k = 1.0f;
		turret_index = object_new(&data);
		if (turret_index == NONE)
			continue;
		definition = unit_definition_get(unit_get(turret_index)->definition_index);
		for (seat = 0; seat < definition->unit.seats.count && !unit_seat_is_driver(turret_index, seat); seat++)
			;
		if (seat >= definition->unit.seats.count)
			seat = 0;
		if (definition->unit.seats.count == 0 || !unit_enter_seat(actor->meta.unit_index, turret_index, seat))
		{
			object_delete(turret_index);
			continue;
		}
		made++;
		survival_vehicle_keep(turret_index);
		platform_log("survival: wave %d: a turret at %.1f %.1f %.1f", survival.wave, place.x, place.y, place.z);
	}
}

/* the Phantom's place along its flight (eased in and out) */
static void survival_phantom_place(
	real_point3d const *from,
	real_point3d const *to,
	real t)
{
	real_point3d position;
	real eased;

	t = PIN(t, 0.0f, 1.0f);
	eased = t * t * (3.0f - 2.0f * t);
	position.x = from->x + (to->x - from->x) * eased;
	position.y = from->y + (to->y - from->y) * eased;
	position.z = from->z + (to->z - from->z) * eased;
	if (object_try_and_get_and_verify_type(survival.phantom_index, _object_mask_vehicle))
	{
		struct object_datum *phantom = object_get(survival.phantom_index);
		real_vector3d up = { 0.0f, 0.0f, 1.0f };

		object_set_position(survival.phantom_index, &position, &survival.phantom_forward, &up);
		csmemset(&phantom->object.translational_velocity, 0, sizeof(phantom->object.translational_velocity));
		csmemset(&phantom->object.angular_velocity, 0, sizeof(phantom->object.angular_velocity));
	}
}

/* a Phantom flies in over one of the light squads' places as the next
wave's wait ends, on the open arenas whose level has one, every other wave
from the second: that squad's group comes from it */
static void survival_phantom_begin(
	void)
{
	struct object_placement_data data;
	real_vector3d out;
	long tag_index = tag_loaded('vehi', SURVIVAL_PHANTOM_TAG);
	short squad_index;

	survival.phantom_squad = NONE;
	if (tag_index == NONE || survival.wave < 2 || (survival.wave & 1) ||
		survival_arenas[survival.arena].maximum_height < SURVIVAL_VEHICLE_ARENA_HEIGHT ||
		survival.phantom_phase != _survival_phantom_none)
	{
		return;
	}
	squad_index = survival_random_squad(_survival_tier_light);
	if (squad_index == NONE)
		return;
	survival.phantom_hover = survival.squads[squad_index].nearest;
	survival.phantom_hover.z += SURVIVAL_PHANTOM_HEIGHT;
	/* (in from the far side of its place, the way it goes back) */
	out.i = survival.phantom_hover.x - survival.center.x;
	out.j = survival.phantom_hover.y - survival.center.y;
	out.k = 0.0f;
	if (normalize3d(&out) == 0.0f)
	{
		out.i = 1.0f;
		out.j = 0.0f;
	}
	survival.phantom_from.x = survival.phantom_hover.x + out.i * SURVIVAL_PHANTOM_DISTANCE;
	survival.phantom_from.y = survival.phantom_hover.y + out.j * SURVIVAL_PHANTOM_DISTANCE;
	survival.phantom_from.z = survival.phantom_hover.z + SURVIVAL_PHANTOM_CLIMB;
	survival.phantom_to = survival.phantom_from;
	survival.phantom_to.z += SURVIVAL_PHANTOM_CLIMB;
	survival.phantom_forward.i = -out.i;
	survival.phantom_forward.j = -out.j;
	survival.phantom_forward.k = 0.0f;
	object_placement_data_new(&data, tag_index, NONE);
	data.position = survival.phantom_from;
	data.forward = survival.phantom_forward;
	data.up.i = 0.0f;
	data.up.j = 0.0f;
	data.up.k = 1.0f;
	survival.phantom_index = object_new(&data);
	if (survival.phantom_index == NONE)
		return;
	survival.phantom_squad = squad_index;
	survival.phantom_phase = _survival_phantom_arriving;
	survival.phantom_time = game_time_get();
	survival_message("A PHANTOM IS INBOUND");
	platform_log("survival: wave %d: a Phantom to %.1f %.1f %.1f", survival.wave, survival.phantom_hover.x,
		survival.phantom_hover.y, survival.phantom_hover.z);
}

/* each tick: the Phantom flown along (kept where it is meant to be, its
physics undone), and let go once it has left */
static void survival_phantom_update(
	void)
{
	long now = game_time_get();
	long elapsed = now - survival.phantom_time;

	if (survival.phantom_phase == _survival_phantom_none)
		return;
	if (!object_try_and_get_and_verify_type(survival.phantom_index, _object_mask_vehicle))
	{
		survival.phantom_phase = _survival_phantom_none;
		survival.phantom_index = NONE;
		return;
	}
	switch (survival.phantom_phase)
	{
	case _survival_phantom_arriving:
		survival_phantom_place(&survival.phantom_from, &survival.phantom_hover, (real)elapsed / SURVIVAL_PHANTOM_ARRIVAL);
		/* (it hovers until its wave has come and a while after) */
		if (elapsed >= SURVIVAL_PHANTOM_ARRIVAL && survival.state == _survival_state_wave)
		{
			survival.phantom_phase = _survival_phantom_hovering;
			survival.phantom_time = now;
		}
		break;
	case _survival_phantom_hovering:
		survival_phantom_place(&survival.phantom_hover, &survival.phantom_hover, 1.0f);
		if (elapsed >= SURVIVAL_PHANTOM_HOVER)
		{
			real_point3d const *at = &object_get(survival.phantom_index)->object.position;

			platform_log("survival: the Phantom leaves from %.1f %.1f %.1f", at->x, at->y, at->z);
			survival.phantom_phase = _survival_phantom_leaving;
			survival.phantom_time = now;
		}
		break;
	case _survival_phantom_leaving:
		survival_phantom_place(&survival.phantom_hover, &survival.phantom_to, (real)elapsed / SURVIVAL_PHANTOM_DEPARTURE);
		if (elapsed >= SURVIVAL_PHANTOM_DEPARTURE)
		{
			object_delete(survival.phantom_index);
			survival.phantom_index = NONE;
			survival.phantom_phase = _survival_phantom_none;
		}
		break;
	}
}

static void survival_spawn_wave(
	void)
{
	short wave = survival.wave;
	short budget = (short)MIN(4 + 2 * wave, SURVIVAL_MAXIMUM_WAVE_SIZE);
	short attempts;
	short placed_by_squad[MAXIMUM_SURVIVAL_SQUADS];
	boolean boss_wave = FALSE;
	real heavy_chance = wave < 2 ? 0.0f : MIN(0.15f * (real)wave, 0.6f);
	short wing_kind = NONE;

	/* (every fifth wave a themed one, in turn: the Hunters with Elites, the
	heaviest the level has (its Elites, else its Flood's combat forms), a
	wing of Banshees where they fly) */
	survival.theme = NULL;
	if (wave % 5 == 0)
	{
		short theme = (short)((wave / 5 - 1) % 3);
		boolean flood = FALSE;
		short i;

		for (i = 0; i < survival.squad_count; i++)
		{
			if (survival.squads[i].tier == _survival_tier_heavy && survival.squads[i].actor_type == _actor_combat_form)
				flood = TRUE;
		}
		if (theme == 2)
		{
			wing_kind = 1; /* (survival_vehicles' BANSHEE) */
			if (survival_arenas[survival.arena].maximum_height < SURVIVAL_VEHICLE_ARENA_HEIGHT ||
				survival_profile.no_vehicles || tag_loaded('vehi', survival_vehicles[wing_kind].tag_name) == NONE ||
				survival.tier_counts[_survival_tier_heavy] == 0)
			{
				wing_kind = NONE;
				theme = 1;
			}
			else
			{
				survival.theme = "BANSHEE WING";
				heavy_chance = 0.8f;
			}
		}
		if (theme == 0 && survival.tier_counts[_survival_tier_boss] > 0)
		{
			survival.theme = "HUNTERS";
			boss_wave = TRUE;
			heavy_chance = 0.75f;
		}
		else if (theme != 2 && survival.tier_counts[_survival_tier_heavy] > 0)
		{
			survival.theme = flood ? "FLOOD SWARM" : "ELITE ASSAULT";
			heavy_chance = 1.0f;
			if (flood)
				budget = (short)MIN(budget + budget / 2, SURVIVAL_MAXIMUM_WAVE_SIZE);
		}
	}

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
	survival.pending_count = survival.pending_next = 0;
	for (attempts = 0; budget > 0 && attempts < 40; attempts++)
	{
		short tier;
		short group;
		short squad_index;
		struct survival_squad *squad;
		short placed;

		/* (the Phantom's squad first: its group under it) */
		if (survival.phantom_squad != NONE && attempts == 0)
		{
			tier = _survival_tier_light;
		}
		else if (boss_wave)
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
		squad_index = survival.phantom_squad != NONE && attempts == 0 ? survival.phantom_squad :
			survival_random_squad(tier);
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
		/* (the Phantom's group at once, under it; the others to come one
		at a time) */
		if (survival.phantom_squad != NONE && attempts == 0)
		{
			placed = encounter_survival_place(squad->encounter_index, squad->squad_index, group,
				placed_by_squad[squad_index], squad->actor_palette_index);
			survival.wave_placed += placed;
		}
		else
		{
			short k;

			for (k = 0; k < group && survival.pending_count < SURVIVAL_MAXIMUM_WAVE_SIZE; k++)
			{
				survival.pending[survival.pending_count].squad_index = squad_index;
				survival.pending[survival.pending_count].extra = (short)(placed_by_squad[squad_index] + k);
				survival.pending_count++;
			}
		}
		placed_by_squad[squad_index] += group;
		budget -= group;
	}
	survival.phantom_squad = NONE;
	survival.wing_kind = wing_kind;
	survival.extras_pending = TRUE;
	survival.last_arrival_time = game_time_get() - SURVIVAL_ARRIVAL_PERIOD;
	survival.wave_start_living = survival.wave_living = (short)(survival_living_count() + survival.pending_count);
	survival_score();
	survival_reveal_player();
	platform_log("survival: wave %d: %d enemies placed, %d to come", survival.wave, survival.wave_placed,
		survival.pending_count);
}

/* the next of the wave's enemies to come, in the teleport's flash; once all
have, the wave's vehicles and turrets */
static void survival_arrivals_update(
	void)
{
	long now = game_time_get();

	if (survival.pending_next < survival.pending_count)
	{
		struct survival_squad *squad;
		long before[MAXIMUM_SURVIVAL_TRACKED_ACTORS], after[MAXIMUM_SURVIVAL_TRACKED_ACTORS];
		short types[MAXIMUM_SURVIVAL_TRACKED_ACTORS], living[MAXIMUM_SURVIVAL_TRACKED_ACTORS];
		short before_count, after_count, i, j;

		if (now - survival.last_arrival_time < SURVIVAL_ARRIVAL_PERIOD)
			return;
		survival.last_arrival_time = now;
		squad = &survival.squads[survival.pending[survival.pending_next].squad_index];
		before_count = encounter_survival_actors(squad->encounter_index, before, types, living,
			MAXIMUM_SURVIVAL_TRACKED_ACTORS);
		if (encounter_survival_place(squad->encounter_index, squad->squad_index, 1,
			survival.pending[survival.pending_next].extra, squad->actor_palette_index) > 0)
		{
			long effect_tag = tag_loaded('effe', SURVIVAL_ARRIVAL_EFFECT);

			survival.wave_placed++;
			/* (the newcomer's flash) */
			after_count = encounter_survival_actors(squad->encounter_index, after, types, living,
				MAXIMUM_SURVIVAL_TRACKED_ACTORS);
			for (i = 0; effect_tag != NONE && i < after_count; i++)
			{
				boolean known = FALSE;

				for (j = 0; j < before_count && !known; j++)
					known = before[j] == after[i];
				if (!known)
				{
					struct actor_datum *actor = actor_get(after[i]);

					if (actor->meta.unit_index != NONE)
						effect_new_from_object(effect_tag, NONE, actor->meta.unit_index, NONE, 1.0f, 1.0f, NULL, NULL);
				}
			}
			survival_reveal_player();
		}
		else
		{
			/* (no room for it: one fewer in the wave) */
			survival.wave_start_living--;
		}
		survival.pending_next++;
		return;
	}
	if (survival.extras_pending)
	{
		survival.extras_pending = FALSE;
		survival_spawn_vehicles(survival.wing_kind, survival.wing_kind != NONE ? 3 : 0);
		survival_spawn_turrets();
		platform_log("survival: wave %d: all %d come", survival.wave, survival.wave_placed);
	}
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
		/* (Famine: no ammunition between waves) */
		for (i = 0; i < MAXIMUM_WEAPONS_PER_UNIT && !survival_skull(_survival_skull_famine); i++)
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

/* the run over: the arena's best kept if this beat it (the waves
survived, then the points) */
static void survival_record_best(
	void)
{
	short waves = (short)MAX(survival.wave - 1, 0);

	if (!VALID_INDEX(survival.arena, NUMBER_OF_SURVIVAL_ARENAS))
		return;
	survival_profile_load();
	if (waves > survival_profile.best[survival.arena].wave ||
		(waves == survival_profile.best[survival.arena].wave && survival.points > survival_profile.best[survival.arena].points))
	{
		survival_profile.best[survival.arena].wave = waves;
		survival_profile.best[survival.arena].points = survival.points;
		survival_profile.best[survival.arena].kills = survival.kills;
		survival_profile_save();
		if (waves > 0)
			survival_message("NEW BEST HERE: %d WAVE%s, %ld POINTS", waves, waves == 1 ? "" : "S", survival.points);
	}
	else if (survival_profile.best[survival.arena].wave > 0)
	{
		survival_message("BEST HERE: %d WAVES, %ld POINTS", survival_profile.best[survival.arena].wave,
			survival_profile.best[survival.arena].points);
	}
}

/* every third wave cleared, goods fall from the sky by the players: one of
the shop's weapons and a powerup or grenades, the level's */
#define SURVIVAL_SUPPLY_PERIOD 3
static void survival_supply_drop(
	void)
{
	long units[MAXIMUM_NUMBER_OF_LOCAL_PLAYERS];
	short unit_count = survival_player_units(units);
	short weapons[NUMBEROF(survival_goods)], extras[NUMBEROF(survival_goods)];
	short weapon_count = 0, extra_count = 0, i, dropped = 0;
	struct unit_datum *unit;
	short picks[2];

	if (survival.wave % SURVIVAL_SUPPLY_PERIOD != 0 || unit_count == 0 || !(unit = unit_try_and_get(units[0])))
		return;
	for (i = 0; i < NUMBEROF(survival_goods); i++)
	{
		struct survival_good const *good = &survival_goods[i];

		if ((good->kind == _survival_good_weapon || good->kind == _survival_good_equipment ||
			good->kind == _survival_good_frag_grenades || good->kind == _survival_good_plasma_grenades) &&
			tag_loaded(good->group_tag, good->tag_name) != NONE)
		{
			if (good->kind == _survival_good_weapon)
				weapons[weapon_count++] = i;
			else
				extras[extra_count++] = i;
		}
	}
	picks[0] = weapon_count ? weapons[seed_random_range(&survival.seed, 0, weapon_count)] : NONE;
	picks[1] = extra_count ? extras[seed_random_range(&survival.seed, 0, extra_count)] : NONE;
	for (i = 0; i < 2; i++)
	{
		struct survival_good const *good;
		struct object_placement_data data;
		real angle = real_seed_random_range(&survival.seed, 0.0f, 2.0f * (real)M_PI);
		real_point3d from, above;
		long tag_index;

		if (picks[i] == NONE)
			continue;
		good = &survival_goods[picks[i]];
		tag_index = tag_loaded(good->group_tag, good->tag_name);
		/* (2 to 3 units away, dropped from as high as there is room) */
		from = unit->object.position;
		from.z += 0.5f;
		above = from;
		above.x += (real)cos(angle) * (2.0f + (real)i);
		above.y += (real)sin(angle) * (2.0f + (real)i);
		if (survival_blocked(&from, &above))
			above = from;
		from = above;
		above.z += 4.0f;
		if (survival_blocked(&from, &above))
			above = from;
		object_placement_data_new(&data, tag_index, NONE);
		data.position = above;
		dropped += object_new(&data) != NONE;
	}
	if (dropped)
		survival_message("SUPPLY DROP NEARBY");
}

static void survival_set_state(
	short state)
{
	survival.state = state;
	survival.state_time = game_time_get();
	survival.phantom_tried = FALSE;
}

/* the ground under the point (a little above the point's own height, so a
point on the ground stays): a little above it */
static void survival_ground(
	real_point3d *point)
{
	struct collision_result collision;
	real_point3d from = *point;
	real_vector3d down = { 0.0f, 0.0f, -6.0f };

	from.z += 0.5f;
	if (collision_test_vector(FLAG(_collision_test_front_facing_surfaces_bit) | FLAG(_collision_test_structure_bit) |
		FLAG(_collision_test_objects_bit) | FLAG(_collision_test_objects_scenery_bit), &from, &down, NONE, &collision))
	{
		point->z = collision.point.z + 0.2f;
	}
}

/* the players to the arena's ground (split screen's side by side), facing
the point given */
static void survival_move_players(
	real_point3d const *position,
	real_point3d const *facing_point)
{
	long units[MAXIMUM_NUMBER_OF_LOCAL_PLAYERS];
	short unit_count = survival_player_units(units);
	real_vector3d forward;
	short i;

	forward.i = facing_point->x - position->x;
	forward.j = facing_point->y - position->y;
	forward.k = 0.0f;
	if (normalize3d(&forward) == 0.0f)
	{
		forward.i = 1.0f;
		forward.j = 0.0f;
	}
	for (i = 0; i < unit_count; i++)
	{
		struct unit_datum *unit = unit_get(units[i]);
		long player_index = player_index_from_unit_index(units[i]);
		real_point3d place = *position;

		/* (the second to the first's left) */
		place.x -= forward.j * 1.0f * (real)i;
		place.y += forward.i * 1.0f * (real)i;
		survival_ground(&place);
		if (unit->object.parent_object_index != NONE)
			unit_exit_seat_end(units[i]);
		object_reset(units[i]);
		unit->unit.desired_facing_vector = forward;
		unit->unit.desired_aiming_vector = forward;
		unit->unit.desired_looking_vector = forward;
		if (player_index != NONE)
		{
			struct player_datum *player = player_get(player_index);

			player_teleport(player_index, NONE, &place);
			if (player->local_player_index != NONE)
				player_control_set_facing(player->local_player_index, &forward);
		}
		else
		{
			object_set_position(units[i], &place, &forward, NULL);
		}
	}
}

static void survival_begin(
	long unit_index)
{
	struct survival_arena_definition const *arena = &survival_arenas[survival.arena];
	real radius = arena->radius;
	short tier;

	survival.center = arena->moved ? arena->position : object_get(unit_index)->object.position;
	survival.seed = (unsigned long)game_time_get() * 2654435761u ^ 0x5eed5eedu;
	/* (the squads of encounters that fight near first; failing enough, any
	near) */
	do
	{
		survival_select_squads(radius, arena->maximum_height, TRUE);
		if (survival.tier_counts[_survival_tier_light] + survival.tier_counts[_survival_tier_heavy] >= 4)
			break;
		radius *= 1.5f;
	}
	while (radius <= arena->maximum_radius);
	if (survival.tier_counts[_survival_tier_light] + survival.tier_counts[_survival_tier_heavy] < 4)
	{
		radius = arena->radius;
		do
		{
			survival_select_squads(radius, arena->maximum_height, FALSE);
			if (survival.tier_counts[_survival_tier_light] + survival.tier_counts[_survival_tier_heavy] >= 4)
				break;
			radius *= 1.5f;
		}
		while (radius <= arena->maximum_radius);
	}
	survival_force_hunters();
	survival_shop_stock();
	if (arena->moved)
	{
		/* (facing where the squads are, on average) */
		real_point3d facing = survival.center;

		if (survival.squad_count > 0)
		{
			facing.x = facing.y = facing.z = 0.0f;
			for (tier = 0; tier < survival.squad_count; tier++)
			{
				facing.x += survival.squads[tier].nearest.x / (real)survival.squad_count;
				facing.y += survival.squads[tier].nearest.y / (real)survival.squad_count;
				facing.z += survival.squads[tier].nearest.z / (real)survival.squad_count;
			}
		}
		survival_move_players(&arena->position, &facing);
	}
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
	/* (debug.survival_wave: a test's) */
	survival.wave = (short)PIN(config_integer("debug.survival_wave"), 1, 99);
	survival.kills = 0;
	/* (debug.survival_points: a test's) */
	survival.points = config_integer("debug.survival_points");
	survival.tracked_count = 0;
	survival.fireteams = 0;
	csmemset(survival.charges, 0, sizeof(survival.charges));
	/* (the player and the Marines allies, as the level's scripts make
	them, which survival leaves out: without it, a fireteam fights the
	player; a betrayal still turns them, as in the campaign) */
	ai_scripting_allegiance(_game_team_player, _game_team_human);
	survival.test_bought = FALSE;
	survival.first_wave = survival.wave;
	survival.begun = FALSE;
	survival.phantom_index = NONE;
	survival.phantom_phase = _survival_phantom_none;
	survival.phantom_squad = NONE;
	survival.multiplier = 1.0f;
	survival.theme = NULL;
	survival_profile_load();
	survival.shop_status[0] = 0;
	survival.last_cleanup_time = 0;
	survival.cleaned = 0;
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
			survival_score();
			survival_message("YOU SURVIVED %d WAVE%s: %ld KILLS, %ld POINTS", survival.wave - 1,
				survival.wave - 1 == 1 ? "" : "S", survival.kills, survival.points);
			survival_record_best();
		}
		return;
	}
	/* (the bodies and the dropped weapons of the waves before let go,
	those out of sight, the oldest first, a few at a time) */
	if ((survival.state == _survival_state_intermission || survival.state == _survival_state_wave) &&
		now - survival.last_cleanup_time >= SURVIVAL_CLEANUP_PERIOD)
	{
		survival.last_cleanup_time = now;
		survival.cleaned += objects_port_collect_old_garbage(SURVIVAL_GARBAGE_KEPT, SURVIVAL_GARBAGE_COLLECTED);
	}
	survival_phantom_update();
	switch (survival.state)
	{
	case _survival_state_waiting_for_player:
		if (unit_index == NONE)
			break;
		if (survival_arenas[survival.arena].moved &&
			global_structure_bsp_index != survival_arenas[survival.arena].structure_bsp_index)
		{
			main_switch_structure_bsp(survival_arenas[survival.arena].structure_bsp_index);
			survival_set_state(_survival_state_moving);
			break;
		}
		survival_begin(unit_index);
		break;

	case _survival_state_moving:
		if (global_structure_bsp_index == survival_arenas[survival.arena].structure_bsp_index ||
			now - survival.state_time >= SURVIVAL_MOVE_TIME_LIMIT)
		{
			survival_begin(unit_index);
		}
		break;

	case _survival_state_intermission:
	{
		long delay = survival.wave == 1 ? SURVIVAL_FIRST_WAVE_DELAY : SURVIVAL_INTERMISSION;

		/* (the Phantom on its way to arrive as the wave comes) */
		if (survival.phantom_phase == _survival_phantom_none && survival.phantom_squad == NONE &&
			now - survival.state_time >= delay - SURVIVAL_PHANTOM_ARRIVAL && !survival.phantom_tried)
		{
			survival.phantom_tried = TRUE;
			survival_phantom_begin();
		}

		/* (debug.survival_buy: a test's goods, bought by the first player) */
		if (!survival.test_bought && config_string("debug.survival_buy")[0] &&
			now - survival.state_time >= 2 * TICKS_PER_SECOND)
		{
			char names[256];
			char *name;

			survival.test_bought = TRUE;
			strncpy(names, config_string("debug.survival_buy"), sizeof(names) - 1);
			names[sizeof(names) - 1] = 0;
			for (name = strtok(names, ","); name; name = strtok(NULL, ","))
			{
				short i;

				for (i = 0; i < survival.good_count; i++)
				{
					if (!_stricmp(survival_goods[survival.goods[i]].name, name))
					{
						platform_log("survival: test buys %s: %s", name, survival_shop_buy(i, 0) ? "bought" : survival.shop_status);
						break;
					}
				}
				if (i == survival.good_count)
					platform_log("survival: test buys %s: not stocked", name);
			}
		}

		if (now - survival.state_time >= delay)
		{
			if (!survival.begun)
			{
				short skull;
				char skulls[96] = "";

				survival.begun = TRUE;
				survival.multiplier = survival_skulls_multiplier();
				for (skull = 0; skull < NUMBER_OF_SURVIVAL_SKULLS; skull++)
				{
					if (TEST_FLAG(survival_profile.skulls, skull))
					{
						strncat(skulls, skulls[0] ? ", " : "", sizeof(skulls) - strlen(skulls) - 1);
						strncat(skulls, survival_skulls[skull].name, sizeof(skulls) - strlen(skulls) - 1);
					}
				}
				if (skulls[0])
					survival_message("SKULLS: %s (POINTS x%.2f)", skulls, survival.multiplier);
			}
			survival_spawn_wave();
			if (survival.theme)
				survival_message("WAVE %d: %s (%ld POINTS)", survival.wave, survival.theme, survival.points);
			else
				survival_message("WAVE %d (%ld POINTS)", survival.wave, survival.points);
			survival.last_count_time = survival.last_reveal_time = now;
			survival_set_state(_survival_state_wave);
		}
		break;
	}

	case _survival_state_wave:
		survival_arrivals_update();
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
			short living = (short)(survival_living_count() + survival.pending_count - survival.pending_next);

			survival.last_count_time = now;
			survival_score();
			if (living != survival.wave_living)
				platform_log("survival: wave %d: %d of %d left", survival.wave, living, survival.wave_start_living);
			survival.wave_living = living;
			if (survival.pending_next >= survival.pending_count && !survival.extras_pending &&
				(living == 0 || (living <= SURVIVAL_STRAGGLERS && now - survival.state_time >= SURVIVAL_STRAGGLER_TIME) ||
				now - survival.state_time >= SURVIVAL_WAVE_TIME_LIMIT))
			{
				survival.kills += survival.wave_start_living - living;
				survival.points += survival_scaled(SURVIVAL_WAVE_BONUS * survival.wave);
				platform_log("survival: wave %d cleared; %ld bodies and items cleaned up so far", survival.wave,
					survival.cleaned);
				survival_message("WAVE %d CLEARED: +%ld, %ld POINTS. Next wave in %d seconds", survival.wave,
					survival_scaled(SURVIVAL_WAVE_BONUS * survival.wave), survival.points,
					SURVIVAL_INTERMISSION / TICKS_PER_SECOND);
				survival_resupply();
				survival_supply_drop();
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
	platform_log("survival: over, %d waves survived, %ld kills, %ld points", MAX(survival.wave - 1, 0), survival.kills,
		survival.points);
	survival.active = FALSE;
	survival.state = _survival_state_none;
	main_goto_main_menu();
	return TRUE;
}

/* ---------- the shop (menu_functions.c's SURVIVAL SHOP, the pause menu's) */

/* the goods the level has */
static void survival_shop_stock(
	void)
{
	short index;

	survival.good_count = 0;
	for (index = 0; index < NUMBEROF(survival_goods); index++)
	{
		struct survival_good const *good = &survival_goods[index];
		boolean stocked;

		switch (good->kind)
		{
		case _survival_good_skull:
		case _survival_good_vehicles_option:
			/* (listed first, below) */
			stocked = FALSE;
			break;
		case _survival_good_fireteam:
			stocked = survival.marine_squad_count > 0;
			break;
		case _survival_good_ammunition:
		case _survival_good_health_charge:
			stocked = TRUE;
			break;
		default:
			stocked = tag_loaded(good->group_tag, good->tag_name) != NONE;
			break;
		}
		if (stocked)
			survival.goods[survival.good_count++] = index;
	}
	/* (the favourites first: the last bought, the latest first) */
	{
		short favourite, placed = 0;

		for (favourite = 0; favourite < SURVIVAL_FAVOURITES; favourite++)
		{
			short good = survival_profile.favourites[favourite], i;

			for (i = placed; good != NONE && i < survival.good_count; i++)
			{
				if (survival.goods[i] == good)
				{
					memmove(&survival.goods[placed + 1], &survival.goods[placed], (i - placed) * sizeof(short));
					survival.goods[placed++] = good;
					break;
				}
			}
		}
		survival.favourite_count = placed;
	}
	/* (before the first wave, the run's options above all) */
	if (!survival.begun)
	{
		short options[NUMBEROF(survival_goods)], option_count = 0;

		for (index = 0; index < NUMBEROF(survival_goods); index++)
		{
			if (survival_goods[index].kind == _survival_good_vehicles_option)
			{
				if (survival_arenas[survival.arena].maximum_height >= SURVIVAL_VEHICLE_ARENA_HEIGHT)
					options[option_count++] = index;
			}
			else if (survival_goods[index].kind == _survival_good_skull)
			{
				options[option_count++] = index;
			}
		}
		option_count = (short)MIN(option_count, NUMBEROF(survival.goods) - survival.good_count);
		memmove(&survival.goods[option_count], &survival.goods[0], survival.good_count * sizeof(short));
		memcpy(&survival.goods[0], options, option_count * sizeof(short));
		survival.good_count += option_count;
		survival.option_count = option_count;
	}
	else
	{
		survival.option_count = 0;
	}
}

static void survival_shop_say(
	char const *format,
	...)
{
	va_list arguments;

	va_start(arguments, format);
	_vsnprintf(survival.shop_status, sizeof(survival.shop_status) - 1, format, arguments);
	va_end(arguments);
	survival.shop_status[sizeof(survival.shop_status) - 1] = 0;
	platform_log("survival: shop: %s", survival.shop_status);
}

/* whether the line from a to b crosses the level's structure */
static boolean survival_blocked(
	real_point3d const *a,
	real_point3d const *b)
{
	struct collision_result collision;
	real_vector3d vector;

	vector.i = b->x - a->x;
	vector.j = b->y - a->y;
	vector.k = b->z - a->z;
	return collision_test_vector(FLAG(_collision_test_front_facing_surfaces_bit) |
		FLAG(_collision_test_back_facing_surfaces_bit) | FLAG(_collision_test_structure_bit) |
		FLAG(_collision_test_objects_bit) | FLAG(_collision_test_objects_scenery_bit) |
		FLAG(_collision_test_objects_vehicles_bit), a, &vector, NONE, &collision);
}

/* a place for a vehicle of about `length` by `width` world units in front of
the unit (else to a side, else behind), on the ground, with room around
and above it and nothing between it and the unit; its facing the unit's
or the side's direction; FALSE if none */
static boolean survival_vehicle_place(
	struct unit_datum *unit,
	real length,
	real width,
	real_point3d *place,
	real_vector3d *forward)
{
	short side;

	/* (each side near, then each a little farther) */
	for (side = 0; side < 8; side++)
	{
		real_vector3d direction = unit->object.forward;
		real_vector3d across;
		real_point3d center, eye = unit->object.position, above;
		struct collision_result collision;
		real_vector3d down = { 0.0f, 0.0f, -4.0f };
		boolean room = TRUE;
		short corner;
		real turned_i;

		direction.k = 0.0f;
		if (normalize3d(&direction) == 0.0f)
		{
			direction.i = 1.0f;
			direction.j = 0.0f;
		}
		/* (a quarter turn each time) */
		for (corner = 0; corner < (side & 3); corner++)
		{
			turned_i = -direction.j;
			direction.j = direction.i;
			direction.i = turned_i;
		}
		across.i = -direction.j;
		across.j = direction.i;
		across.k = 0.0f;
		eye.z += 0.5f;
		center = eye;
		center.x += direction.i * (length * 0.5f + (side < 4 ? 1.2f : 3.5f));
		center.y += direction.j * (length * 0.5f + (side < 4 ? 1.2f : 3.5f));
		if (survival_blocked(&eye, &center))
			continue;
		/* (the ground under it, and room above) */
		if (!collision_test_vector(FLAG(_collision_test_front_facing_surfaces_bit) | FLAG(_collision_test_structure_bit),
			&center, &down, NONE, &collision))
		{
			continue;
		}
		center.z = collision.point.z + 0.6f;
		above = center;
		above.z += 1.5f;
		if (survival_blocked(&center, &above))
			continue;
		/* (its corners clear of walls, at its body's height: not the ground's
		slope under them) */
		for (corner = 0; corner < 4 && room; corner++)
		{
			real_point3d body = center, point;
			real along = (corner & 1 ? 0.5f : -0.5f) * length, beside = (corner & 2 ? 0.5f : -0.5f) * width;

			body.z += 0.6f;
			point = body;
			point.x += direction.i * along + across.i * beside;
			point.y += direction.j * along + across.j * beside;
			room = !survival_blocked(&body, &point);
		}
		if (!room)
			continue;
		*place = center;
		*forward = direction;
		return TRUE;
	}
	return FALSE;
}

/* a fireteam of the level's Marines, from the next of their squads near,
following the players; FALSE if none could be placed */
static boolean survival_shop_fireteam(
	void)
{
	short tried;

	for (tried = 0; tried < survival.marine_squad_count; tried++)
	{
		struct survival_squad const *squad =
			&survival.marine_squads[(survival.fireteams + tried) % survival.marine_squad_count];
		short placed = encounter_survival_place(squad->encounter_index, squad->squad_index, SURVIVAL_FIRETEAM_SIZE,
			(short)(survival.fireteams / survival.marine_squad_count * SURVIVAL_FIRETEAM_SIZE), NONE);

		if (placed > 0)
		{
			long reference = (long)DATUM_INDEX_TO_ABSOLUTE_INDEX(squad->encounter_index);

			ai_scripting_follow_target_players(reference);
			ai_scripting_follow_distance(reference, 1.5f);
			survival.fireteams++;
			return TRUE;
		}
	}
	return FALSE;
}

short survival_shop_good_count(
	void)
{
	return survival.active ? survival.good_count : 0;
}

char const *survival_shop_good_name(
	short index)
{
	static char name[64];
	struct survival_good const *good;

	if (!VALID_INDEX(index, survival.good_count))
		return "";
	good = &survival_goods[survival.goods[index]];
	switch (good->kind)
	{
	case _survival_good_skull:
		snprintf(name, sizeof(name), "SKULL %s: %s", good->name,
			TEST_FLAG(survival_profile.skulls, good->group_tag) ? "ON" : "OFF");
		break;
	case _survival_good_vehicles_option:
		snprintf(name, sizeof(name), "%s: %s", good->name, survival_profile.no_vehicles ? "OFF" : "ON");
		break;
	default:
		snprintf(name, sizeof(name), "%s%s", index >= survival.option_count &&
			index < survival.option_count + survival.favourite_count ? "* " : "", good->name);
		break;
	}
	return name;
}

long survival_shop_good_price(
	short index)
{
	return VALID_INDEX(index, survival.good_count) ? survival_good_cost(&survival_goods[survival.goods[index]]) : 0;
}

/* the shop's price column: the points, or a skull's points multiplier */
char const *survival_shop_good_price_text(
	short index)
{
	static char text[16];
	struct survival_good const *good;

	if (!VALID_INDEX(index, survival.good_count))
		return "";
	good = &survival_goods[survival.goods[index]];
	if (good->kind == _survival_good_skull)
		snprintf(text, sizeof(text), "x%.2f", survival_skulls[good->group_tag].multiplier);
	else if (good->kind == _survival_good_vehicles_option)
		text[0] = 0;
	else
		snprintf(text, sizeof(text), "%ld", survival_good_cost(good));
	return text;
}

long survival_points(
	void)
{
	return survival.points;
}

char const *survival_shop_status(
	void)
{
	return survival.shop_status;
}

void survival_shop_opened(
	void)
{
	survival.shop_status[0] = 0;
	if (survival.active)
	{
		survival_profile_load();
		survival_shop_stock();
		if (!survival.begun)
			survival_shop_say("BEFORE WAVE %d: SKULLS AND OPTIONS AT THE TOP", survival.wave);
	}
}

boolean survival_shop_buy(
	short index,
	short local_player_index)
{
	struct survival_good const *good;
	long player_index = local_player_get_player_index(local_player_index);
	long unit_index = player_index != NONE ? player_get(player_index)->unit_index : NONE;
	struct unit_datum *unit = unit_index != NONE ? unit_try_and_get(unit_index) : NULL;
	boolean bought = FALSE;

	if (!survival.active || !VALID_INDEX(index, survival.good_count) ||
		(survival.state != _survival_state_intermission && survival.state != _survival_state_wave))
	{
		survival_shop_say("THE SHOP IS CLOSED");
		return FALSE;
	}
	good = &survival_goods[survival.goods[index]];
	if (good->kind == _survival_good_skull || good->kind == _survival_good_vehicles_option)
	{
		if (good->kind == _survival_good_skull)
		{
			if (survival.begun)
			{
				survival_shop_say("SKULLS ARE CHOSEN BEFORE THE FIRST WAVE");
				return FALSE;
			}
			survival_profile.skulls ^= FLAG(good->group_tag);
			survival_shop_say("%s %s: POINTS x%.2f IN ALL", good->name,
				TEST_FLAG(survival_profile.skulls, good->group_tag) ? "ON" : "OFF", survival_skulls_multiplier());
		}
		else
		{
			survival_profile.no_vehicles = !survival_profile.no_vehicles;
			survival_shop_say("COVENANT VEHICLES %s", survival_profile.no_vehicles ? "OFF" : "ON");
		}
		survival_profile_save();
		return TRUE;
	}
	if (!unit)
	{
		survival_shop_say("NOT WHILE YOU ARE DEAD");
		return FALSE;
	}
	if (survival.points < survival_good_cost(good))
	{
		survival_shop_say("%s: %ld MORE POINTS NEEDED", good->name, survival_good_cost(good) - survival.points);
		return FALSE;
	}
	switch (good->kind)
	{
	case _survival_good_weapon:
	case _survival_good_equipment:
	{
		long tag_index = tag_loaded(good->group_tag, good->tag_name);
		struct object_placement_data data;

		if (tag_index == NONE)
			break;
		object_placement_data_new(&data, tag_index, NONE);
		data.position = unit->object.position;
		/* (a weapon in front, to be picked up; a powerup on the player, taken
		at once) */
		if (good->kind == _survival_good_weapon)
		{
			data.position.x += unit->object.forward.i * 0.4f;
			data.position.y += unit->object.forward.j * 0.4f;
			data.position.z += 0.1f;
		}
		bought = object_new(&data) != NONE;
		if (bought)
		{
			survival_shop_say(good->kind == _survival_good_weapon ? "%s: AT YOUR FEET" : "%s: YOURS", good->name);
		}
		break;
	}
	case _survival_good_frag_grenades:
	case _survival_good_plasma_grenades:
	{
		short kind = good->kind == _survival_good_frag_grenades ? 0 : 1;

		if (unit->unit.grenade_counts[kind] >= SURVIVAL_SHOP_MAXIMUM_GRENADES)
		{
			survival_shop_say("%s: YOU HAVE %d", good->name, SURVIVAL_SHOP_MAXIMUM_GRENADES);
			return FALSE;
		}
		unit->unit.grenade_counts[kind] = (char)MIN(unit->unit.grenade_counts[kind] + 2, SURVIVAL_SHOP_MAXIMUM_GRENADES);
		survival_shop_say("%s: YOU HAVE %d", good->name, unit->unit.grenade_counts[kind]);
		bought = TRUE;
		break;
	}
	case _survival_good_ammunition:
	{
		short i;

		for (i = 0; i < MAXIMUM_WEAPONS_PER_UNIT; i++)
		{
			if (unit->unit.weapon_object_indices[i] != NONE)
			{
				short rounds[2] = { SHORT_MAX, SHORT_MAX };

				weapon_set_total_rounds(unit->unit.weapon_object_indices[i], rounds);
			}
		}
		survival_shop_say("AMMUNITION: YOUR WEAPONS ARE FULL");
		bought = TRUE;
		break;
	}
	case _survival_good_health_charge:
		if (!VALID_INDEX(local_player_index, MAXIMUM_NUMBER_OF_LOCAL_PLAYERS) ||
			survival.charges[local_player_index] >= SURVIVAL_MAXIMUM_CHARGES)
		{
			survival_shop_say("%s: YOU HAVE %d (THE MOST)", good->name, SURVIVAL_MAXIMUM_CHARGES);
			return FALSE;
		}
		survival.charges[local_player_index]++;
		survival_shop_say("%s: %d BANKED, D-PAD UP TO USE", good->name, survival.charges[local_player_index]);
		bought = TRUE;
		break;
	case _survival_good_fireteam:
		bought = survival_shop_fireteam();
		survival_shop_say(bought ? "MARINE FIRETEAM: ON THE WAY" : "MARINE FIRETEAM: NONE CAN COME");
		break;
	case _survival_good_vehicle:
	{
		long tag_index = tag_loaded(good->group_tag, good->tag_name);
		struct object_placement_data data;
		real_point3d place;
		real_vector3d forward;

		if (tag_index == NONE)
			break;
		if (!survival_vehicle_place(unit, 2.4f, 1.4f, &place, &forward))
		{
			survival_shop_say("%s: NO ROOM FOR ONE HERE", good->name);
			return FALSE;
		}
		object_placement_data_new(&data, tag_index, NONE);
		data.position = place;
		data.forward = forward;
		data.up.i = 0.0f;
		data.up.j = 0.0f;
		data.up.k = 1.0f;
		bought = object_new(&data) != NONE;
		if (bought)
			survival_shop_say("%s: READY", good->name);
		break;
	}
	}
	if (!bought)
		return FALSE;
	survival.points -= survival_good_cost(good);
	/* (a favourite now, the latest first; the list as it was until the shop
	opens again) */
	{
		short good_index = survival.goods[index], i;

		for (i = 0; i < SURVIVAL_FAVOURITES - 1 && survival_profile.favourites[i] != good_index; i++)
			;
		memmove(&survival_profile.favourites[1], &survival_profile.favourites[0], i * sizeof(short));
		survival_profile.favourites[0] = good_index;
		survival_profile_save();
	}
	return TRUE;
}

/* the map loading is an arena's: the menus' request's, the restart's, or a
test's (debug.survival); map_name is the map's own name ("b30") */
boolean survival_map_is_arena(
	char const *map_name)
{
	short arena = survival.requested_arena;
	short index;

	if (arena == NONE && survival.restart)
		arena = survival.arena;
	for (index = 0; index < NUMBER_OF_SURVIVAL_ARENAS; index++)
	{
		char const *name = survival_arenas[index].map_name;
		char const *short_name = strrchr(name, '\\') ? strrchr(name, '\\') + 1 : name;
		boolean tested = arena == NONE && config_string("debug.survival")[0] &&
			!_stricmp(config_string("debug.survival"), short_name);

		if ((index == arena || tested) && map_name && !_stricmp(map_name, short_name))
			return TRUE;
	}
	return FALSE;
}

/* ---------- the skulls' (the engine's hooks) */

/* game_globals.c: the difficulty's value for a team, scaled for the
skulls: Mythic's enemies' vitality and shields, Catch's grenades */
real survival_difficulty_scale(
	short value_type,
	short team_index)
{
	if (!survival.active || !game_team_is_enemy(_game_team_player, team_index) || team_index == _game_team_player)
		return 1.0f;
	switch (value_type)
	{
	case _game_difficulty_value_enemy_vitality:
	case _game_difficulty_value_enemy_shield:
		return survival_skull(_survival_skull_mythic) ? 2.0f : 1.0f;
	case _game_difficulty_value_grenade_chance_scale:
		return survival_skull(_survival_skull_catch) ? 3.0f : 1.0f;
	default:
		return 1.0f;
	}
}

/* actions.c: Tough Luck's enemies dive away from much less danger */
real survival_dive_threshold_scale(
	void)
{
	return survival_skull(_survival_skull_tough_luck) ? 0.35f : 1.0f;
}

/* weapons.c: the rounds a player's unit takes from what it picks up
(Famine: half) */
short survival_rounds_taken(
	long unit_index,
	short rounds)
{
	if (survival_skull(_survival_skull_famine) && rounds > 1 && player_index_from_unit_index(unit_index) != NONE)
		return (short)((rounds + 1) / 2);
	return rounds;
}

/* damage.c: whether a player's shield recharges (Black Eye: only from a
melee kill) */
boolean survival_shield_recharge_blocked(
	long object_index)
{
	return survival_skull(_survival_skull_black_eye) && player_index_from_unit_index(object_index) != NONE;
}

/* damage.c: something killed by a melee: Black Eye's shield back to its
killer, a player */
void survival_melee_kill(
	long killer_object_index)
{
	if (survival_skull(_survival_skull_black_eye) && killer_object_index != NONE &&
		player_index_from_unit_index(killer_object_index) != NONE)
	{
		struct object_datum *killer = object_try_and_get_and_verify_type(killer_object_index, _object_mask_unit);

		if (killer && killer->object.shield_vitality < 1.0f)
		{
			killer->object.shield_vitality = 1.0f;
			killer->object.shield_stun_ticks = 0;
		}
	}
}

/* ---------- the HUD's (port_hud.c) and the menus' */

/* the line on top of the HUD: the next wave's countdown, else the wave's
enemies left; FALSE for none */
boolean survival_hud_status(
	char *text,
	size_t size)
{
	long now = game_time_get();

	if (!survival.active || survival.lost)
		return FALSE;
	switch (survival.state)
	{
	case _survival_state_intermission:
	{
		long delay = survival.wave == survival.first_wave && !survival.begun ? SURVIVAL_FIRST_WAVE_DELAY :
			SURVIVAL_INTERMISSION;
		long left = MAX(delay - (now - survival.state_time), 0);

		snprintf(text, size, "WAVE %d IN %ld   POINTS %ld", survival.wave,
			(left + TICKS_PER_SECOND - 1) / TICKS_PER_SECOND, survival.points);
		return TRUE;
	}
	case _survival_state_wave:
		snprintf(text, size, "WAVE %d%s%s   ENEMIES %d   POINTS %ld", survival.wave, survival.theme ? ": " : "",
			survival.theme ? survival.theme : "", survival.wave_living, survival.points);
		return TRUE;
	default:
		return FALSE;
	}
}

/* the arena's best run (New Game's SURVIVAL list): its waves, 0 for none */
short survival_arena_best(
	short arena,
	long *points)
{
	survival_profile_load();
	if (!VALID_INDEX(arena, NUMBER_OF_SURVIVAL_ARENAS))
		return 0;
	if (points)
		*points = survival_profile.best[arena].points;
	return survival_profile.best[arena].wave;
}

/* player_control.c, each tick a local player controls a unit: the D-pad's
up uses a Health & Overshield charge */
void survival_input(
	short local_player_index,
	long unit_index,
	short gamepad_index)
{
	struct gamepad_state const *gamepad;

	if (!survival.active || !VALID_INDEX(local_player_index, MAXIMUM_NUMBER_OF_LOCAL_PLAYERS) || unit_index == NONE ||
		gamepad_index == NONE || !(gamepad = input_get_gamepad_state(gamepad_index)))
	{
		return;
	}
	if (gamepad->buttons[_gamepad_binary_button_dpad_up] != 1)
		return;
	if (survival.charges[local_player_index] <= 0)
	{
		survival_message("NO HEALTH & OVERSHIELD BANKED (THE SHOP SELLS THEM)");
		return;
	}
	survival.charges[local_player_index]--;
	unit_scripting_set_current_vitality_of(unit_index, 1.0e6f, 1.0e6f);
	object_double_charge_shield(unit_index);
	survival_message("HEALTH & OVERSHIELD: %d LEFT", survival.charges[local_player_index]);
}

/* port_hud.c: a local player's charges banked */
short survival_charges(
	short local_player_index)
{
	return survival.active && VALID_INDEX(local_player_index, MAXIMUM_NUMBER_OF_LOCAL_PLAYERS) ?
		survival.charges[local_player_index] : 0;
}
