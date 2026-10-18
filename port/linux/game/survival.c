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
#include "ai/ai_scenario_definitions.h"
#include "ai/ai_script.h"
#include "game/game.h"
#include "game/players.h"
#include "interface/hud_messaging.h"
#include "items/weapons.h"
#include "main/main.h"
#include "math/real_math.h"
#include "objects/objects.h"
#include "physics/collisions.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#include "tag_files/tag_groups.h"
#include "units/units.h"

#include "survival.h"

#include <stdarg.h>
#include <string.h>

void platform_log(char const *format, ...);
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
};

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
	{ "OVERSHIELD", 300, _survival_good_equipment, 'eqip', "powerups\\over shield" },
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
	{ "H2 SHOTGUN", 350, _survival_good_weapon, 'weap', "weapons\\h2 shotgun\\shotgun" },
	{ "SNIPER RIFLE", 450, _survival_good_weapon, 'weap', "weapons\\sniper rifle\\sniper rifle" },
	{ "FUEL ROD GUN", 500, _survival_good_weapon, 'weap', "weapons\\fuel rod gun\\fuel rod" },
	{ "ROCKET LAUNCHER", 600, _survival_good_weapon, 'weap', "weapons\\rocket launcher\\rocket launcher" },
	{ "WARTHOG", 1000, _survival_good_vehicle, 'vehi', "vehicles\\warthog\\warthog" },
};

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
	short marine_squad_count;
	struct survival_squad marine_squads[MAXIMUM_SURVIVAL_MARINE_SQUADS];
	short marine_encounter_count;
	long marine_encounters[MAXIMUM_SURVIVAL_MARINE_SQUADS];
	short fireteams;
	char shop_status[64];
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
			survival.points += (survival.tracked[i].living - now) * survival_kill_points(survival.tracked[i].actor_type);
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
			placed_by_squad[squad_index], squad->actor_palette_index);
		placed_by_squad[squad_index] += group;
		survival.wave_placed += placed;
		budget -= group;
	}
	survival.wave_start_living = survival.wave_living = survival_living_count();
	survival_score();
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
	survival.wave = 1;
	survival.kills = 0;
	/* (debug.survival_points: a test's) */
	survival.points = config_integer("debug.survival_points");
	survival.tracked_count = 0;
	survival.fireteams = 0;
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

		if (now - survival.state_time >= delay)
		{
			survival_message("WAVE %d (%ld POINTS)", survival.wave, survival.points);
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
			survival_score();
			if (living != survival.wave_living)
				platform_log("survival: wave %d: %d of %d left", survival.wave, living, survival.wave_start_living);
			survival.wave_living = living;
			if (living == 0 || (living <= SURVIVAL_STRAGGLERS && now - survival.state_time >= SURVIVAL_STRAGGLER_TIME) ||
				now - survival.state_time >= SURVIVAL_WAVE_TIME_LIMIT)
			{
				survival.kills += survival.wave_start_living - living;
				survival.points += SURVIVAL_WAVE_BONUS * survival.wave;
				platform_log("survival: wave %d cleared; %ld bodies and items cleaned up so far", survival.wave,
					survival.cleaned);
				survival_message("WAVE %d CLEARED: +%d, %ld POINTS. Next wave in %d seconds", survival.wave,
					SURVIVAL_WAVE_BONUS * survival.wave, survival.points, SURVIVAL_INTERMISSION / TICKS_PER_SECOND);
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
		case _survival_good_fireteam:
			stocked = survival.marine_squad_count > 0;
			break;
		case _survival_good_ammunition:
			stocked = TRUE;
			break;
		default:
			stocked = tag_loaded(good->group_tag, good->tag_name) != NONE;
			break;
		}
		if (stocked)
			survival.goods[survival.good_count++] = index;
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

	for (side = 0; side < 4; side++)
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
		for (corner = 0; corner < side; corner++)
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
		center.x += direction.i * (length * 0.5f + 1.2f);
		center.y += direction.j * (length * 0.5f + 1.2f);
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
		/* (its corners clear of walls) */
		for (corner = 0; corner < 4 && room; corner++)
		{
			real_point3d point = center;
			real along = (corner & 1 ? 0.5f : -0.5f) * length, beside = (corner & 2 ? 0.5f : -0.5f) * width;

			point.x += direction.i * along + across.i * beside;
			point.y += direction.j * along + across.j * beside;
			room = !survival_blocked(&center, &point);
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
	return VALID_INDEX(index, survival.good_count) ? survival_goods[survival.goods[index]].name : "";
}

long survival_shop_good_price(
	short index)
{
	return VALID_INDEX(index, survival.good_count) ? survival_goods[survival.goods[index]].price : 0;
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
	if (!unit)
	{
		survival_shop_say("NOT WHILE YOU ARE DEAD");
		return FALSE;
	}
	if (survival.points < good->price)
	{
		survival_shop_say("%s: %ld MORE POINTS NEEDED", good->name, good->price - survival.points);
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
	survival.points -= good->price;
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
