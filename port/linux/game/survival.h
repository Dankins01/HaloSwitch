/*
SURVIVAL.H

Survival: waves of enemies on a campaign level (survival.c).
*/

#ifndef __SURVIVAL_H
#define __SURVIVAL_H

/* ---------- constants */

/* the arenas the menus offer (survival.c's table) */
enum
{
	SURVIVAL_ARENA_SILENT_CARTOGRAPHER = 0,
	NUMBER_OF_SURVIVAL_ARENAS
};

/* ---------- prototypes/SURVIVAL.C */

/* the menus: the next game is a survival game in the arena (NONE: none) */
void survival_request(
	short arena);
short survival_arena_count(
	void);
char const *survival_arena_map_name(
	short arena);
/* its name, for the menus' list */
wchar_t const *survival_arena_display_name(
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
	short first);

#endif
