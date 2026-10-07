/*
SWITCH_THREAD.C

Threads that run guest code.

Guest (ILP32) code keeps stack addresses in 32-bit registers, so a thread
that runs it needs its stack in guest memory. A libnx thread's stack is
always mirrored into the stack region (threadCreate maps it there with
svcMapMemory), far above 4 GB, so each thread made here starts on a small
libnx stack and moves to a stack in guest memory before it calls the
function (switch_call_on_stack, switch_exception.s). The thread's libnx
state (its TLS, newlib's reentrancy data) stays where libnx put it; only
the stack pointer moves.

Horizon threads of equal priority on a core take turns only when one
blocks or yields, except at priority 0x3B on cores 0 to 2, where the kernel
preempts them in turn. The game busy-waits (the frame limiter spins until
the vertical blank thread advances its counter, main.c), and a spinning
thread that shares a core with the one it waits for would starve it: so
the game's threads run at 0x3B, spread over cores 0 to 2. Threads that must
run promptly (audio, switch_sdl.c) have a higher priority than that.

The guest's thread pointer (its musl struct pthread) is kept per thread in
host TLS. Guest stacks are freed by a reaper thread once their thread has
exited.
*/

#include "switch_host.h"

#include <switch.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define GUARD_SIZE 0x4000
#define HOST_STACK_SIZE 0x20000
/* the game's threads: preemptive (see above) */
#define THREAD_PRIORITY 0x3b
/* the host's own (the reaper), which only wake to do a little */
#define HOST_THREAD_PRIORITY 0x2c

static __thread uint32_t guest_tp;
static __thread int thread_number;
static int next_thread_number = 1;

uint32_t host_get_tp(void)
{
	return guest_tp;
}

void host_set_tp(uint32_t thread)
{
	guest_tp = thread;
}

int host_thread_number(void)
{
	if (!thread_number)
		thread_number = __atomic_fetch_add(&next_thread_number, 1, __ATOMIC_SEQ_CST);
	return thread_number;
}

/* ---------- cores */

static int pick_core(void)
{
	static int cores[4];
	static int core_count;
	static int next;

	if (!core_count)
	{
		u64 mask = 0;
		int core;

		svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
		/* (0x3B preempts on cores 0 to 2 only) */
		for (core = 0; core < 3; core++)
		{
			if (mask & (1ULL << core))
				cores[core_count++] = core;
		}
		if (!core_count)
			return -2;
	}
	return cores[__atomic_fetch_add(&next, 1, __ATOMIC_SEQ_CST) % core_count];
}

/* ---------- calling into the guest */

static int on_guest_stack(void)
{
	uint64_t sp = (uint64_t)__builtin_frame_address(0);

	return sp < 0x100000000ULL;
}

typedef uint32_t (*guest_function)(uint32_t, uint32_t, uint32_t, uint32_t);

uint32_t host_call_guest(uint32_t function, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
	if (!on_guest_stack())
		host_fatal("guest code called on a thread without a guest stack");
	if (!guest_tp)
		((guest_function)(uintptr_t)host_image.header->thread_attach)(0, 0, 0, 0);
	return ((guest_function)(uintptr_t)function)(a, b, c, d);
}

void host_run_guest_main(uint32_t boot)
{
	((void (*)(uint32_t))(uintptr_t)host_image.header->start)(boot);
	host_fatal("the guest returned from __guest_start");
}

/* ---------- threads */

struct thread_start
{
	Thread thread;
	void *(*function)(void *);
	void *argument;
	void *mapping;
	size_t mapping_size;
	void *stack_top;
	struct thread_start *next_finished;
};

static Mutex reaper_lock;
static CondVar reaper_condition;
static struct thread_start *finished_threads;
static int reaper_started;
static Thread reaper_thread;

static void reaper(void *unused)
{
	(void)unused;
	for (;;)
	{
		struct thread_start *finished;

		mutexLock(&reaper_lock);
		while (!finished_threads)
			condvarWait(&reaper_condition, &reaper_lock);
		finished = finished_threads;
		finished_threads = finished->next_finished;
		mutexUnlock(&reaper_lock);
		threadWaitForExit(&finished->thread);
		threadClose(&finished->thread);
		host_low_unmap(finished->mapping, finished->mapping_size);
		free(finished);
	}
}

static void on_guest_stack_main(void *context)
{
	struct thread_start *start = context;

	start->function(start->argument);
}

static void thread_main(void *context)
{
	struct thread_start *start = context;

	host_thread_number();
	switch_call_on_stack(on_guest_stack_main, start, start->stack_top);
	guest_tp = 0;
	mutexLock(&reaper_lock);
	start->next_finished = finished_threads;
	finished_threads = start;
	condvarWakeOne(&reaper_condition);
	mutexUnlock(&reaper_lock);
}

int host_native_thread_create(void *(*function)(void *), void *argument, size_t stack_size)
{
	struct thread_start *start = calloc(1, sizeof(*start));
	size_t total;
	char *base;
	Result result;

	if (!start)
		return ENOMEM;
	mutexLock(&reaper_lock);
	if (!reaper_started)
	{
		if (R_SUCCEEDED(threadCreate(&reaper_thread, reaper, NULL, NULL, 0x4000, HOST_THREAD_PRIORITY, -2)) &&
			R_SUCCEEDED(threadStart(&reaper_thread)))
		{
			reaper_started = 1;
		}
	}
	mutexUnlock(&reaper_lock);

	stack_size = (stack_size + 0xffff) & ~(size_t)0xffff;
	if (stack_size < 0x10000)
		stack_size = 0x10000;
	total = stack_size + GUARD_SIZE;
	base = host_low_map(total, GUEST_PROT_READ | GUEST_PROT_WRITE);
	if (!base)
	{
		free(start);
		return EAGAIN;
	}
	/* a guard at the bottom: an overflow faults rather than writing into
	the next allocation */
	host_low_protect((uintptr_t)base, GUARD_SIZE, GUEST_PROT_NONE);
	start->function = function;
	start->argument = argument;
	start->mapping = base;
	start->mapping_size = total;
	start->stack_top = base + total;
	result = threadCreate(&start->thread, thread_main, start, NULL, HOST_STACK_SIZE, THREAD_PRIORITY, pick_core());
	if (R_SUCCEEDED(result))
		result = threadStart(&start->thread);
	if (R_FAILED(result))
	{
		host_logf(HOST_LOG_ERROR, "cannot start a thread: 0x%x", result);
		host_low_unmap(base, total);
		free(start);
		return EAGAIN;
	}
	return 0;
}

static void *guest_thread_main(void *guest_thread)
{
	host_call_guest(host_image.header->thread_start, (uint32_t)(uintptr_t)guest_thread, 0, 0, 0);
	return NULL;
}

int host_thread_create(uint32_t guest_thread, uint32_t stack_size)
{
	return host_native_thread_create(guest_thread_main, (void *)(uintptr_t)guest_thread, stack_size);
}
