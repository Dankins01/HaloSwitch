/*
PLATFORM.H (Switch host shim)

port/linux/src/xiso.c belongs to the desktop platform layer, which is
built with the game's MSVC-compatible ABI. The Switch host compiles it
with the host ABI instead, for the first start's copy of the game data
(switch_main.c); this header stands in for port/linux/src/platform.h with
the one thing xiso.c takes from it. The Makefile compiles a copy of xiso.c
so that this header, not the platform layer's beside it, is found.
*/

#ifndef __HALO_SWITCH_PLATFORM_SHIM_H
#define __HALO_SWITCH_PLATFORM_SHIM_H

#include "posix.h"

void host_logf(int priority, const char *format, ...) __attribute__((format(printf, 2, 3)));

#define platform_log(...) host_logf(4, __VA_ARGS__)

#endif
