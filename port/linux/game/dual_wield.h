/*
DUAL_WIELD.H

Halo 2's dual wielding (port/linux/game/dual_wield.c).
*/

#ifndef __DUAL_WIELD_H
#define __DUAL_WIELD_H
#pragma once

/* ---------- prototypes */

void dual_wield_initialize_for_new_map(void);
void dual_wield_dispose_from_old_map(void);

/* a local player's controller this tick (player_control.c): the ticks the
weapon switch, the grenade trigger and the action have been held; what the
weapon switch, the grenade and the zoom do is changed for dual wielding */
void dual_wield_input(short local_player_index, long unit_index, short switch_ticks, short grenade_ticks,
	short action_ticks, boolean *rotate_weapons, boolean *throw_grenade, boolean *zoom);

/* each tick, before the objects update (game.c) */
void dual_wield_update(void);

/* the weapon in the unit's left hand; NONE: none */
long dual_wield_left_weapon(long unit_index);

#endif /* __DUAL_WIELD_H */
