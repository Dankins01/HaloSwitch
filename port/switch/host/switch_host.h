/*
SWITCH_HOST.H

Internals of the Nintendo Switch host (a libnx homebrew .nro). See
port/switch/README.md for the design.

The Switch port reuses the Android port's guest image unchanged: the game,
the Linux platform layer and a small musl runtime, compiled as ILP32
AArch64 code and linked to run in the low 4 GB (port/android/README.md,
"How the port operates"). This host plays the part of the Android host
(port/android/host): it maps the image, serves the guest's imports
(port/android/host_imports.list) and its Linux system calls, on top of
libnx, newlib, SDL2 and mesa instead of bionic, SDL3 and OpenGL ES drivers.

The differences that shape this host:

- Horizon has no lazy address-space reservation and no mmap. Memory the
  guest can address is heap memory mapped again below 4 GB with
  svcMapProcessCodeMemory, and protected with svcSetProcessMemoryPermission
  (switch_memory.c). Both need the process handle hbloader passes.
- There are no signals. Write faults on watched pages arrive as user-mode
  exceptions, which hbloader forwards to this NRO (switch_exception.s).
- libnx threads always run on a stack in the stack region, above 4 GB;
  threads that run guest code switch to a stack in guest memory
  (switch_thread.c).
- devkitPro has SDL2, not SDL3: the guest's SDL3 calls are translated
  (switch_sdl.c), and gamepads are read with libnx's pad API directly.
*/

#ifndef __HALO_SWITCH_HOST_H
#define __HALO_SWITCH_HOST_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "halo_android_abi.h"

/* ---------- logging (switch_main.c): /switch/opence/host.txt and the
debug output (svcOutputDebugString, visible with a debugger or nxlink) */

#define HOST_LOG_INFO 4
#define HOST_LOG_WARN 5
#define HOST_LOG_ERROR 6

void host_logf(int priority, const char *format, ...) __attribute__((format(printf, 2, 3)));
void host_log(int priority, const char *text);
/* the same, from the exception handler (does not wait for the log's lock) */
void host_log_crash(const char *text);
/* shows a message in the system's error dialog; 1 if it was shown */
int host_show_error(const char *message);
/* logs, shows the message to the player and leaves */
void host_fatal(const char *format, ...) __attribute__((format(printf, 1, 2), noreturn));
void host_exit(int code) __attribute__((noreturn));
void host_abort(const char *reason) __attribute__((noreturn));
int host_errno(void);

/* the address opence.elf was loaded at (its code's first page) */
uint64_t host_program_base(void);

/* where the game's files live (switch_main.c) */
#define SWITCH_DATA_ROOT "/switch/opence"
#define SWITCH_SAVE_ROOT "/switch/opence/save"

/* ---------- guest memory (switch_memory.c)

Protection values are Linux's (PROT_READ 1, PROT_WRITE 2, PROT_EXEC 4).
Outside the image's code, guest memory is never executable. */

#define GUEST_PROT_NONE 0
#define GUEST_PROT_READ 1
#define GUEST_PROT_WRITE 2
#define GUEST_PROT_EXEC 4

/* checks the address space, maps the Xbox window and checks that the
image's range is free (both fixed); 0 on success, else -1 with the reason
logged */
int host_memory_initialize(uint32_t image_base, uint32_t image_size);
/* zeroed heap pages to build the image in, before host_image_map aliases
them at the image's address: code cannot be written once it is mapped */
void *host_image_backing(uint32_t size);
/* maps the filled backing at the image's range with one Horizon permission
per page (Perm_R, Perm_Rx or Perm_Rw); 0 on success */
int host_image_map(void *backing, const uint8_t *page_permissions);
/* page-granular, zeroed allocations below 4 GB; NULL on failure */
void *host_low_map(size_t size, int protection);
void host_low_unmap(void *address, size_t size);
/* 1 if the range lies in memory this file mapped for the guest */
int host_low_owns(uintptr_t address, size_t size);
/* changes the protection of pages this file mapped; 0 or -errno */
int host_low_protect(uintptr_t address, size_t size, int protection);
/* the guest's mmap/munmap/mprotect (switch_syscall.c); Linux flags */
long host_guest_mmap(uint64_t address, uint64_t size, int protection, int flags, int fd, int64_t offset);
long host_guest_munmap(uint64_t address, uint64_t size);
long host_guest_mprotect(uint64_t address, uint64_t size, int protection);
/* a write fault at address (switch_exception.c): 1 if it was the first
write to a watched page, which is now writeable again */
int host_memory_watch_fault(uint64_t address);
/* a line of text describing the process's memory, for the log */
void host_memory_describe(void);

/* ---------- the guest image (switch_loader.c) */

struct host_guest_image
{
	const struct halo_guest_header *header;
	uint32_t base, end;
};

extern struct host_guest_image host_image;

int host_load_image(const void *elf, size_t size);

/* ---------- threads (switch_thread.c) */

uint32_t host_call_guest(uint32_t function, uint32_t a, uint32_t b, uint32_t c, uint32_t d);
/* starts a thread running function(argument) on a stack in guest memory;
0 or an errno value */
int host_native_thread_create(void *(*function)(void *), void *argument, size_t stack_size);
void host_run_guest_main(uint32_t boot) __attribute__((noreturn));
/* a small number for this thread (gettid) */
int host_thread_number(void);

/* calls function(argument) with the stack pointer at stack_top, and comes
back to the caller's stack (switch_exception.s) */
void switch_call_on_stack(void (*function)(void *), void *argument, void *stack_top);

/* ---------- import table (generated host_import_table.c) */

void *host_resolve_import(const char *name);

/* ---------- SDL2 / GL / input (switch_sdl.c, switch_gl.c) */

void *host_gl_resolve(const char *name);
/* the GL functions switch_gl.c calls itself, once a context exists */
void host_gl_bind_functions(void);

/* a frame was presented, its swap begun at swap_started (switch_sdl.c;
the watchdog in switch_main.c) */
void host_note_frame(uint64_t swap_started);
/* ticks spent waiting for the GPU so far (switch_gl.c) */
uint64_t host_gl_wait_ticks(void);
/* texture write faults handled so far (switch_memory.c) */
uint64_t host_memory_watch_faults(void);

/* ---------- the game data (switch_main.c, xiso.c) */

/* copies <data>/maps out of a disc image in the data folder if the game
data is missing; 0 if there is game data afterwards */
int host_prepare_game_data(void);

#endif
