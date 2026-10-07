/*
SWITCH_PROFILE.C

A sampling profiler, for finding where the game spends its CPU time. It runs
only when /switch/opence/profile.txt exists.

About 500 times a second it pauses each thread that runs game code
(switch_thread.c registers them), reads where it is (svcGetThreadContext3,
which the kernel allows on a paused thread of the process's own:
svcSetThreadActivity), and lets it go on. Every 30 seconds it writes the
places sampled most to the log: addresses in the game image, to be named
with its symbols (llvm-symbolizer --obj=halo_guest.elf), and in this
program (opence.elf, by offset). A thread waiting in the kernel shows as
the system call it waits in, under "waiting".
*/

#include "switch_host.h"

#include <switch.h>

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define PROFILE_FILE SWITCH_DATA_ROOT "/profile.txt"
#define SAMPLE_NANOSECONDS 2000000LL
#define REPORT_SECONDS 30
#define TOP_COUNT 30
/* the game image's code, in buckets of this many bytes */
#define BUCKET_SHIFT 4
#define MAXIMUM_THREADS 48

static Mutex thread_lock;
static Handle threads[MAXIMUM_THREADS];
static int thread_count;
static int profiling;

void host_profile_thread_started(Handle thread)
{
	if (!profiling)
		return;
	mutexLock(&thread_lock);
	if (thread_count < MAXIMUM_THREADS)
		threads[thread_count++] = thread;
	mutexUnlock(&thread_lock);
}

void host_profile_thread_ended(Handle thread)
{
	int index;

	if (!profiling)
		return;
	mutexLock(&thread_lock);
	for (index = 0; index < thread_count; index++)
	{
		if (threads[index] == thread)
		{
			threads[index] = threads[--thread_count];
			break;
		}
	}
	mutexUnlock(&thread_lock);
}

/* ---------- the counts */

static uint32_t *game_buckets;
static uint32_t game_bucket_count;
/* this program's code, more coarsely */
#define HOST_BUCKET_SHIFT 6
#define HOST_BUCKETS (0x1000000 >> HOST_BUCKET_SHIFT)
static uint32_t *host_buckets;
static uint64_t samples, game_samples, host_samples, other_samples;

static void count(uint64_t pc)
{
	uint64_t program = host_program_base();

	samples++;
	if (host_image.header && pc >= host_image.base && pc < host_image.end)
	{
		uint64_t bucket = (pc - host_image.base) >> BUCKET_SHIFT;

		if (bucket < game_bucket_count)
			game_buckets[bucket]++;
		game_samples++;
	}
	else if (pc >= program && pc - program < ((uint64_t)HOST_BUCKETS << HOST_BUCKET_SHIFT))
	{
		host_buckets[(pc - program) >> HOST_BUCKET_SHIFT]++;
		host_samples++;
	}
	else
	{
		other_samples++;
	}
}

struct top
{
	uint32_t count;
	uint64_t address;
	int game;
};

static void consider(struct top *tops, uint32_t value, uint64_t address, int game)
{
	int index, smallest = 0;

	for (index = 1; index < TOP_COUNT; index++)
	{
		if (tops[index].count < tops[smallest].count)
			smallest = index;
	}
	if (value > tops[smallest].count)
	{
		tops[smallest].count = value;
		tops[smallest].address = address;
		tops[smallest].game = game;
	}
}

static int by_count(const void *a, const void *b)
{
	const struct top *x = a, *y = b;

	return x->count < y->count ? 1 : x->count > y->count ? -1 : 0;
}

static void report(void)
{
	struct top tops[TOP_COUNT];
	uint64_t index;
	int rank;

	if (!samples)
		return;
	memset(tops, 0, sizeof(tops));
	for (index = 0; index < game_bucket_count; index++)
	{
		if (game_buckets[index])
			consider(tops, game_buckets[index], host_image.base + (index << BUCKET_SHIFT), 1);
	}
	for (index = 0; index < HOST_BUCKETS; index++)
	{
		if (host_buckets[index])
			consider(tops, host_buckets[index], index << HOST_BUCKET_SHIFT, 0);
	}
	qsort(tops, TOP_COUNT, sizeof(tops[0]), by_count);
	host_logf(HOST_LOG_INFO, "profile: %llu samples, %.1f%% in the game, %.1f%% in opence.elf (waiting there "
		"included), %.1f%% elsewhere", (unsigned long long)samples, 100.0 * (double)game_samples / (double)samples,
		100.0 * (double)host_samples / (double)samples, 100.0 * (double)other_samples / (double)samples);
	for (rank = 0; rank < TOP_COUNT && tops[rank].count; rank++)
	{
		if (tops[rank].game)
			host_logf(HOST_LOG_INFO, "profile %2d: %5.1f%% game %08llx", rank + 1,
				100.0 * (double)tops[rank].count / (double)samples, (unsigned long long)tops[rank].address);
		else
			host_logf(HOST_LOG_INFO, "profile %2d: %5.1f%% opence.elf +0x%llx", rank + 1,
				100.0 * (double)tops[rank].count / (double)samples, (unsigned long long)tops[rank].address);
	}
	memset(game_buckets, 0, game_bucket_count * sizeof(uint32_t));
	memset(host_buckets, 0, HOST_BUCKETS * sizeof(uint32_t));
	samples = game_samples = host_samples = other_samples = 0;
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

			if (R_FAILED(svcSetThreadActivity(threads[index], ThreadActivity_Paused)))
				continue;
			if (R_SUCCEEDED(svcGetThreadContext3(&context, threads[index])))
				count(context.pc.x);
			svcSetThreadActivity(threads[index], ThreadActivity_Runnable);
		}
		mutexUnlock(&thread_lock);
		if (armGetSystemTick() - last_report >= (uint64_t)REPORT_SECONDS * frequency)
		{
			report();
			last_report = armGetSystemTick();
		}
	}
}

void host_profile_start(void)
{
	static Thread thread;
	struct stat information;

	if (stat(PROFILE_FILE, &information) != 0 || !host_image.header)
		return;
	mutexInit(&thread_lock);
	game_bucket_count = (host_image.end - host_image.base) >> BUCKET_SHIFT;
	game_buckets = calloc(game_bucket_count, sizeof(uint32_t));
	host_buckets = calloc(HOST_BUCKETS, sizeof(uint32_t));
	if (!game_buckets || !host_buckets)
		return;
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
