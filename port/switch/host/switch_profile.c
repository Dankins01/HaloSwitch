/*
SWITCH_PROFILE.C

A sampling profiler, for finding where the game spends its CPU time. It runs
only when /switch/opence/profile.txt exists.

About 500 times a second it pauses each thread that runs game code
(switch_thread.c registers them), reads where it is (svcGetThreadContext3,
which the kernel allows on a paused thread of the process's own:
svcSetThreadActivity), and lets it go on. Every 30 seconds it writes, for
each thread, how much of the time it was busy rather than waiting in the
kernel, and the places it was sampled most:

- "game 88xxxxxx": an address in the game image, to be named with its
  symbols (llvm-symbolizer --obj=halo_guest.elf);
- "opence.elf +0x...": an offset into this program (mesa, SDL, libnx and
  the host), likewise with opence.elf;
- "waits in opence.elf +0x...": the thread was in a system call that
  blocks, called from there (the svc's caller: its link register).

Time in this program is also added up by the game function it was called
from ("for game 88xxxxxx"): the first game address among the thread's frame
records, found by walking them on its stack. That says which of the game's
own functions the GL driver's time, or a wait, is for.
*/

#include "switch_host.h"

#include <switch.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define PROFILE_FILE SWITCH_DATA_ROOT "/profile.txt"
#define SAMPLE_NANOSECONDS 2000000LL
#define REPORT_SECONDS 30
/* lines a thread gets in a report, of places and of game callers */
#define TOP_PLACES 12
#define TOP_CALLERS 8
#define MAXIMUM_THREADS 24
/* places in the game image are counted in buckets of 16 bytes, in this
program of 64 */
#define GAME_SHIFT 4
#define HOST_SHIFT 6
#define HOST_SPAN 0x2000000ULL
/* each thread's counts, an open hash table (a power of 2) */
#define TABLE_SIZE 4096
#define MAXIMUM_FRAMES 48

/* a key: the kind in the top bits, the address (bucket) below */
#define KIND_GAME 1ULL
#define KIND_HOST 2ULL
#define KIND_WAIT 3ULL
#define KIND_CALLER 4ULL
#define KEY(kind, address) (((kind) << 56) | (address))
#define KEY_KIND(key) ((key) >> 56)
#define KEY_ADDRESS(key) ((key) & ((1ULL << 56) - 1))

struct entry
{
	uint64_t key;
	uint32_t count;
};

struct profiled_thread
{
	Handle handle;
	int number;
	char name[16];
	uint64_t samples, busy, busy_game, busy_host;
	struct entry *table;
	uint32_t used;
	/* the game's main thread: also counted for profile.csv */
	int fine;
};

static Mutex thread_lock;
static struct profiled_thread threads[MAXIMUM_THREADS];
static int thread_count, next_number = 1;
static int profiling;

void host_profile_thread_started(Handle thread, const char *name)
{
	struct entry *table;

	if (!profiling)
		return;
	table = calloc(TABLE_SIZE, sizeof(struct entry));
	if (!table)
		return;
	mutexLock(&thread_lock);
	if (thread_count < MAXIMUM_THREADS)
	{
		struct profiled_thread *profiled = &threads[thread_count++];

		memset(profiled, 0, sizeof(*profiled));
		profiled->handle = thread;
		profiled->number = next_number++;
		strncpy(profiled->name, name ? name : "thread", sizeof(profiled->name) - 1);
		profiled->table = table;
		profiled->fine = name && !strcmp(name, "game");
		table = NULL;
	}
	mutexUnlock(&thread_lock);
	free(table);
}

void host_profile_thread_ended(Handle thread)
{
	struct entry *table = NULL;
	int index;

	if (!profiling)
		return;
	mutexLock(&thread_lock);
	for (index = 0; index < thread_count; index++)
	{
		if (threads[index].handle == thread)
		{
			table = threads[index].table;
			threads[index] = threads[--thread_count];
			break;
		}
	}
	mutexUnlock(&thread_lock);
	free(table);
}

/* ---------- the counts */

static void add(struct profiled_thread *thread, uint64_t key)
{
	uint32_t slot = (uint32_t)((key * 0x9e3779b97f4a7c15ULL) >> 52) & (TABLE_SIZE - 1);

	for (;;)
	{
		struct entry *entry = &thread->table[slot];

		if (entry->key == key)
		{
			entry->count++;
			return;
		}
		if (!entry->key)
		{
			/* (full enough: the rest is not counted by place) */
			if (thread->used >= TABLE_SIZE * 3 / 4)
				return;
			entry->key = key;
			entry->count = 1;
			thread->used++;
			return;
		}
		slot = (slot + 1) & (TABLE_SIZE - 1);
	}
}

static int in_game(uint64_t address)
{
	return host_image.header && address >= host_image.base && address < host_image.end;
}

static int in_host(uint64_t address)
{
	uint64_t program = host_program_base();

	return address >= program && address - program < HOST_SPAN;
}

/* the first game address among the frame records from fp up, within the
stack's memory (the block holding sp); 0 if there is none */
static uint64_t game_caller(uint64_t fp, uint64_t sp)
{
	MemoryInfo information;
	u32 page_information;
	uint64_t end;
	int depth;

	if (R_FAILED(svcQueryMemory(&information, &page_information, sp)) || !(information.perm & Perm_R))
		return 0;
	end = information.addr + information.size;
	for (depth = 0; depth < MAXIMUM_FRAMES; depth++)
	{
		const uint64_t *record;
		uint64_t return_address;

		if (fp < sp || fp + 16 > end || (fp & 7))
			return 0;
		record = (const uint64_t *)(uintptr_t)fp;
		return_address = record[1];
		if (in_game(return_address))
			return return_address;
		if (record[0] <= fp)
			return 0;
		fp = record[0];
	}
	return 0;
}

static int is_svc(uint64_t address)
{
	return (*(const uint32_t *)(uintptr_t)address & 0xffe0001f) == 0xd4000001;
}

/* ---------- the game thread's whole run, finely, for profile.csv

Every sample of the thread named "game", by its place (16-byte buckets in
both programs, or a wait by its caller) and the game function it was for
(its first game frame): written out with every report, to be read with the
symbols of the build (tools/switch_profile.py) */

#define FINE_SIZE 65536
#define FINE_FILE SWITCH_DATA_ROOT "/profile.csv"

struct fine_entry
{
	uint64_t where;
	uint32_t caller;
	uint32_t count;
};

static struct fine_entry *fine;
static uint32_t fine_used;
static uint64_t fine_samples, fine_lost;

static void fine_add(uint64_t where, uint32_t caller)
{
	uint64_t mixed = where * 0x9e3779b97f4a7c15ULL ^ (uint64_t)caller * 0xc2b2ae3d27d4eb4fULL;
	uint32_t slot = (uint32_t)(mixed >> 48) & (FINE_SIZE - 1);

	fine_samples++;
	for (;;)
	{
		struct fine_entry *entry = &fine[slot];

		if (entry->count && entry->where == where && entry->caller == caller)
		{
			entry->count++;
			return;
		}
		if (!entry->count)
		{
			if (fine_used >= FINE_SIZE * 7 / 8)
			{
				fine_lost++;
				return;
			}
			entry->where = where;
			entry->caller = caller;
			entry->count = 1;
			fine_used++;
			return;
		}
		slot = (slot + 1) & (FINE_SIZE - 1);
	}
}

static void fine_write(void)
{
	FILE *file;
	uint32_t index;

	if (!fine || !fine_samples || !(file = fopen(FINE_FILE, "w")))
		return;
	fprintf(file, "# build %s, game image %08x, %llu samples, %llu not counted by place\n", SWITCH_BUILD,
		(unsigned)host_image.base, (unsigned long long)fine_samples, (unsigned long long)fine_lost);
	fprintf(file, "kind,address,caller,count\n");
	for (index = 0; index < FINE_SIZE; index++)
	{
		const struct fine_entry *entry = &fine[index];

		if (!entry->count)
			continue;
		fprintf(file, "%c,%llx,%x,%u\n", (int)(entry->where >> 56), (unsigned long long)(entry->where & ((1ULL << 56) - 1)),
			(unsigned)entry->caller, (unsigned)entry->count);
	}
	fclose(file);
}

static void sample(struct profiled_thread *thread, const ThreadContext *context)
{
	uint64_t pc = context->pc.x;
	uint64_t program = host_program_base();
	uint64_t where;

	int fine_thread = fine && thread->fine;

	thread->samples++;
	if (in_game(pc))
	{
		thread->busy++;
		thread->busy_game++;
		add(thread, KEY(KIND_GAME, (pc - host_image.base) >> GAME_SHIFT));
		if (fine_thread)
			fine_add(((uint64_t)'g' << 56) | (pc & ~15ULL), 0);
		return;
	}
	if (!in_host(pc))
	{
		thread->busy++;
		if (fine_thread)
			fine_add((uint64_t)'o' << 56, 0);
		return;
	}
	/* a thread blocked in the kernel: its pc is at the svc instruction
	(Horizon reports that, not the instruction after it, as the first
	profile showed), or just past it */
	if ((pc & 3) == 0 && (is_svc(pc) || is_svc(pc - 4)))
	{
		uint64_t caller = context->lr;

		where = in_host(caller) ? ((uint64_t)'w' << 56) | (caller - program) : (uint64_t)'w' << 56;
		add(thread, in_host(caller) ? KEY(KIND_WAIT, caller - program) : KEY(KIND_WAIT, 0));
	}
	else
	{
		thread->busy++;
		thread->busy_host++;
		where = ((uint64_t)'h' << 56) | ((pc - program) & ~15ULL);
		add(thread, KEY(KIND_HOST, (pc - program) >> HOST_SHIFT));
	}
	{
		uint64_t caller = game_caller(context->fp, context->sp);

		if (caller)
			add(thread, KEY(KIND_CALLER, (caller - host_image.base) >> GAME_SHIFT));
		if (fine_thread)
			fine_add(where, (uint32_t)caller);
	}
}

/* ---------- reports */

static void top(const struct profiled_thread *thread, int callers, struct entry *tops, int count)
{
	uint32_t index;

	memset(tops, 0, (size_t)count * sizeof(*tops));
	for (index = 0; index < TABLE_SIZE; index++)
	{
		const struct entry *entry = &thread->table[index];
		int smallest = 0, slot;

		if (!entry->key || (KEY_KIND(entry->key) == KIND_CALLER) != callers)
			continue;
		for (slot = 1; slot < count; slot++)
		{
			if (tops[slot].count < tops[smallest].count)
				smallest = slot;
		}
		if (entry->count > tops[smallest].count)
			tops[smallest] = *entry;
	}
}

static int by_count(const void *a, const void *b)
{
	const struct entry *x = a, *y = b;

	return x->count < y->count ? 1 : x->count > y->count ? -1 : 0;
}

static void describe(char *buffer, size_t size, uint64_t key)
{
	uint64_t address = KEY_ADDRESS(key);

	switch (KEY_KIND(key))
	{
	case KIND_GAME:
		snprintf(buffer, size, "game %08llx", (unsigned long long)(host_image.base + (address << GAME_SHIFT)));
		break;
	case KIND_HOST:
		snprintf(buffer, size, "opence.elf +0x%llx", (unsigned long long)(address << HOST_SHIFT));
		break;
	case KIND_WAIT:
		snprintf(buffer, size, "waits in opence.elf +0x%llx", (unsigned long long)address);
		break;
	default:
		snprintf(buffer, size, "for game %08llx", (unsigned long long)(host_image.base + (address << GAME_SHIFT)));
		break;
	}
}

static void report_thread(struct profiled_thread *thread)
{
	struct entry places[TOP_PLACES], callers[TOP_CALLERS];
	double samples = (double)thread->samples;
	int index;
	s32 priority = 0, core = -1;
	u64 affinity = 0;

	if (!thread->samples)
		return;
	svcGetThreadPriority(&priority, thread->handle);
	svcGetThreadCoreMask(&core, &affinity, thread->handle);
	host_logf(HOST_LOG_INFO, "profile: thread %d (%s, core %d, priority 0x%x): busy %.1f%% (game %.1f%%, "
		"opence.elf %.1f%%), waiting %.1f%%", thread->number, thread->name, (int)core, (unsigned)priority,
		100.0 * (double)thread->busy / samples, 100.0 * (double)thread->busy_game / samples,
		100.0 * (double)thread->busy_host / samples, 100.0 * (double)(thread->samples - thread->busy) / samples);
	top(thread, 0, places, TOP_PLACES);
	qsort(places, TOP_PLACES, sizeof(places[0]), by_count);
	for (index = 0; index < TOP_PLACES && places[index].count; index++)
	{
		char text[64];

		/* (under half a percent: noise) */
		if (places[index].count * 200 < thread->samples)
			break;
		describe(text, sizeof(text), places[index].key);
		host_logf(HOST_LOG_INFO, "profile %d.%-2d %5.1f%% %s", thread->number, index + 1,
			100.0 * (double)places[index].count / samples, text);
	}
	top(thread, 1, callers, TOP_CALLERS);
	qsort(callers, TOP_CALLERS, sizeof(callers[0]), by_count);
	for (index = 0; index < TOP_CALLERS && callers[index].count; index++)
	{
		char text[64];

		if (callers[index].count * 100 < thread->samples)
			break;
		describe(text, sizeof(text), callers[index].key);
		host_logf(HOST_LOG_INFO, "profile %d.c%-2d %5.1f%% in opence.elf %s", thread->number, index + 1,
			100.0 * (double)callers[index].count / samples, text);
	}
	memset(thread->table, 0, TABLE_SIZE * sizeof(struct entry));
	thread->used = 0;
	thread->samples = thread->busy = thread->busy_game = thread->busy_host = 0;
}

static void report(void)
{
	int index;

	host_logf(HOST_LOG_INFO, "profile: the last %d s, %d threads", REPORT_SECONDS, thread_count);
	for (index = 0; index < thread_count; index++)
		report_thread(&threads[index]);
	fine_write();
}

/* ---------- sampling */

static void sampler(void *unused)
{
	uint64_t frequency = armGetSystemTickFreq(), last_report = armGetSystemTick();

	(void)unused;
	for (;;)
	{
		int index;

		svcSleepThread(SAMPLE_NANOSECONDS);
		mutexLock(&thread_lock);
		for (index = 0; index < thread_count; index++)
		{
			ThreadContext context;

			if (R_FAILED(svcSetThreadActivity(threads[index].handle, ThreadActivity_Paused)))
				continue;
			if (R_SUCCEEDED(svcGetThreadContext3(&context, threads[index].handle)))
				sample(&threads[index], &context);
			svcSetThreadActivity(threads[index].handle, ThreadActivity_Runnable);
		}
		if (armGetSystemTick() - last_report >= (uint64_t)REPORT_SECONDS * frequency)
		{
			report();
			last_report = armGetSystemTick();
		}
		mutexUnlock(&thread_lock);
	}
}

void host_profile_start(void)
{
	static Thread thread;
	struct stat information;

	if (stat(PROFILE_FILE, &information) != 0 || !host_image.header)
		return;
	mutexInit(&thread_lock);
	fine = calloc(FINE_SIZE, sizeof(*fine));
	profiling = 1;
	/* above the game's threads (0x3b), so it runs when they are busy */
	if (R_FAILED(threadCreate(&thread, sampler, NULL, NULL, 0x8000, 0x2a, -2)) || R_FAILED(threadStart(&thread)))
	{
		profiling = 0;
		host_logf(HOST_LOG_WARN, "profile: cannot start the sampler");
		return;
	}
	host_logf(HOST_LOG_INFO, "profile: sampling every %lld ms, a report every %d s (profile.txt)",
		SAMPLE_NANOSECONDS / 1000000, REPORT_SECONDS);
}
