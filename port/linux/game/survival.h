/*
SURVIVAL.H

Survival: waves of enemies on a campaign level (survival.c).
*/

#ifndef __SURVIVAL_H
#define __SURVIVAL_H

/* ---------- constants */

/* the arenas the menus offer (survival.c's table): one on each campaign
level, in the levels' order */
enum
{
	NUMBER_OF_SURVIVAL_ARENAS = 10
};

/* ---------- prototypes/SURVIVAL.C */

/* the menus: the next game is a survival game in the arena (NONE: none) */
void survival_request(
	short arena);
short survival_arena_count(
	void);
char const *survival_arena_map_name(
	short arena);

/* whether this game is a survival game */
boolean survival_active(
	void);
/* the requested one, before its map loads (main.c: no saved game is
resumed for it) */
boolean survival_requested(
	void);

/* game.c: a map's game begins (before its scripts and actors); FALSE when it
is a survival game, which places no actors of the level's and runs none of
its scripts */
boolean survival_initialize_for_new_map(
	char const *map_name);
void survival_dispose_from_old_map(
	void);
/* game.c, each tick (a local game's) */
void survival_update(
	void);
/* main.c: the player died; TRUE when survival took it (the game ends,
back to the main menu, rather than going back to a checkpoint) */
boolean survival_player_lost(
	void);

/* the shop (the pause menu's SURVIVAL SHOP): its goods (those the level
has), their prices, the points to buy them with, and what the last
purchase did; a good bought for the local player (FALSE: not, and why in
the status) */
short survival_shop_good_count(
	void);
char const *survival_shop_good_name(
	short index);
long survival_shop_good_price(
	short index);
long survival_points(
	void);
char const *survival_shop_status(
	void);
void survival_shop_opened(
	void);
boolean survival_shop_buy(
	short index,
	short local_player_index);
/* the shop's price column: points, or a skull's multiplier */
char const *survival_shop_good_price_text(
	short index);

/* the skulls' hooks: game_globals.c's difficulty values, actions.c's dive,
weapons.c's rounds picked up, damage.c's shield recharge and melee kills */
real survival_difficulty_scale(
	short value_type,
	short team_index);
real survival_dive_threshold_scale(
	void);
short survival_rounds_taken(
	long unit_index,
	short rounds);
boolean survival_shield_recharge_blocked(
	long object_index);
void survival_melee_kill(
	long killer_object_index);

/* port_hud.c: the line on top of the HUD (the wave's countdown or its
enemies left); FALSE for none */
boolean survival_hud_status(
	char *text,
	size_t size);
/* player_control.c: a local player's D-pad (the Health & Overshield
charges), and port_hud.c: their charges banked */
void survival_input(
	short local_player_index,
	long unit_index,
	short gamepad_index);
short survival_charges(
	short local_player_index);
/* New Game's SURVIVAL list: an arena's best run, its waves (0: none) */
short survival_arena_best(
	short arena,
	long *points);

/* menu_tags.c: the map loading is a survival game's arena (its own name:
"b30"), whose pause menu has the shop */
boolean survival_map_is_arena(
	char const *map_name);

/* ---------- encounters.c's, for the waves */

long encounter_survival_index(
	short absolute_index);
short encounter_survival_squad_actor_type(
	short absolute_index,
	short squad_index);
short encounter_survival_living_count(
	long encounter_index);
short encounter_survival_place(
	long encounter_index,
	short squad_index,
	short count,
	short first,
	short actor_palette_index);
short encounter_survival_actors(
	long encounter_index,
	long *actor_indices,
	short *actor_types,
	short *living,
	short maximum);

#endif
