/*
SWITCH_MAIN.C

Entry point of the Switch port (an NRO run from the Homebrew Menu).

It starts libnx's services, makes sure the game data is on the SD card
(copying the maps folder out of a disc image the player put there, the
first time), loads the guest image from the NRO's romfs, gives it an
environment describing where the data and saves live, and runs its
main() on a thread with its stack in guest memory (switch_thread.c). The
main thread then only waits for the game to end.

SD card layout (port/switch/README.md):

  /switch/opence/opence.nro      this program
  /switch/opence/<image>.iso     a disc image (first start only)
  /switch/opence/maps/           the game data, copied from the image
  /switch/opence/save/           saved games and profiles
  /switch/opence/config.toml     settings (port/linux/README.md)
  /switch/opence/debug.txt       the game's log
  /switch/opence/host.txt        this host's log
*/

#include "switch_host.h"

#include <switch.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "xiso.h"

#ifndef SWITCH_BUILD
#define SWITCH_BUILD "local"
#endif


uint64_t host_program_base(void)
{
	static uint64_t base;
	MemoryInfo information;
	u32 page_information;

	if (!base && R_SUCCEEDED(svcQueryMemory(&information, &page_information, (u64)(uintptr_t)host_program_base)))
		base = information.addr;
	return base;
}

/* ---------- logging and leaving */

/* The log is written with write() and synced at every line: the SD card's
file system keeps written data in its cache until the file is synced, so a
crash would otherwise lose the lines that say where it happened. */
static int log_fd = -1;
static Mutex log_lock;
static u64 start_tick;

static void log_write(int priority, const char *text)
{
	static const char *const names[] = { "", "", "", "", "info", "warn", "error" };
	const char *name = priority >= 4 && priority <= 6 ? names[priority] : "";
	char line[1100];
	u64 milliseconds = armTicksToNs(armGetSystemTick() - start_tick) / 1000000;
	int length = snprintf(line, sizeof(line), "%6llu.%03llu [%s] %s\n",
		(unsigned long long)(milliseconds / 1000), (unsigned long long)(milliseconds % 1000), name, text);

	if (length > (int)sizeof(line) - 1)
		length = (int)sizeof(line) - 1;
	if (log_fd >= 0 && length > 0)
	{
		write(log_fd, line, (size_t)length);
		fsync(log_fd);
	}
}

void host_log(int priority, const char *text)
{
	svcOutputDebugString(text, strlen(text));
	mutexLock(&log_lock);
	log_write(priority, text);
	mutexUnlock(&log_lock);
}

void host_log_crash(const char *text)
{
	/* (the crashed thread may hold the lock: write without it then) */
	int locked = mutexTryLock(&log_lock);

	svcOutputDebugString(text, strlen(text));
	log_write(HOST_LOG_ERROR, text);
	if (locked)
		mutexUnlock(&log_lock);
}

void host_logf(int priority, const char *format, ...)
{
	char text[1024];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(text, sizeof(text), format, arguments);
	va_end(arguments);
	host_log(priority, text);
}

/* ends the process: back to the Home menu (hbloader cannot reuse a process
whose memory is mapped below 4 GB for the guest) */
static void clocks_restore(void);
static void leave(int code) __attribute__((noreturn));
static void leave(int code)
{
	host_logf(HOST_LOG_INFO, "leaving (%d)", code);
	clocks_restore();
	mutexLock(&log_lock);
	if (log_fd >= 0)
	{
		close(log_fd);
		log_fd = -1;
	}
	mutexUnlock(&log_lock);
	svcExitProcess();
	__builtin_unreachable();
}

int host_show_error(const char *message)
{
	AppletType type = appletGetAppletType();

	/* the application error applet only under an application ([10.0.0+]
	makes it a fatal error otherwise); the system one under an applet */
	if (type == AppletType_Application || type == AppletType_SystemApplication)
	{
		ErrorApplicationConfig config;

		if (R_FAILED(errorApplicationCreate(&config, message, NULL)))
			return 0;
		return R_SUCCEEDED(errorApplicationShow(&config));
	}
	else
	{
		ErrorSystemConfig config;

		if (R_FAILED(errorSystemCreate(&config, message, NULL)))
			return 0;
		return R_SUCCEEDED(errorSystemShow(&config));
	}
}

void host_fatal(const char *format, ...)
{
	char message[1024];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(message, sizeof(message), format, arguments);
	va_end(arguments);
	host_log(HOST_LOG_ERROR, message);
	host_show_error(message);
	leave(1);
}

void host_abort(const char *reason)
{
	host_fatal("The game stopped (%s). The log is in " SWITCH_DATA_ROOT "/host.txt and debug.txt.", reason);
}

void host_exit(int code)
{
	host_logf(HOST_LOG_INFO, "the game exited (%d)", code);
	leave(code);
}

/* ---------- the first-start console */

static int console_open;

static void console_print(const char *format, ...)
{
	va_list arguments;

	if (!console_open)
	{
		consoleInit(NULL);
		console_open = 1;
	}
	va_start(arguments, format);
	vprintf(format, arguments);
	va_end(arguments);
	consoleUpdate(NULL);
}

/* shows the reason on the console until the player presses +, then
leaves */
static void console_fail(const char *format, ...) __attribute__((noreturn));
static void console_fail(const char *format, ...)
{
	char message[1024];
	PadState pad;
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(message, sizeof(message), format, arguments);
	va_end(arguments);
	host_log(HOST_LOG_ERROR, message);
	console_print("\n%s\n\nPress + to leave.\n", message);
	padConfigureInput(1, HidNpadStyleSet_NpadStandard);
	padInitializeDefault(&pad);
	while (appletMainLoop())
	{
		padUpdate(&pad);
		if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
			break;
		consoleUpdate(NULL);
		svcSleepThread(16000000);
	}
	consoleExit(NULL);
	leave(1);
}

/* ---------- the game data */

static int has_game_data(void)
{
	struct stat information;

	return stat(SWITCH_DATA_ROOT "/maps/ui.map", &information) == 0;
}

static int has_suffix(const char *name, const char *suffix)
{
	size_t length = strlen(name), suffix_length = strlen(suffix);

	return length > suffix_length && !strcasecmp(name + length - suffix_length, suffix);
}

static int find_disc_image(char *path, size_t size)
{
	DIR *directory = opendir(SWITCH_DATA_ROOT);
	struct dirent *entry;
	int found = 0;

	if (!directory)
		return 0;
	while (!found && (entry = readdir(directory)) != NULL)
	{
		if (has_suffix(entry->d_name, ".iso") || has_suffix(entry->d_name, ".xiso"))
		{
			snprintf(path, size, "%s/%s", SWITCH_DATA_ROOT, entry->d_name);
			found = 1;
		}
	}
	closedir(directory);
	return found;
}

static void extraction_progress(void *context, const char *file, unsigned long long done, unsigned long long total)
{
	static char last_file[256];
	static int last_percent = -1;
	int percent = total ? (int)(done * 100 / total) : 0;

	(void)context;
	if (strcmp(file, last_file) || percent != last_percent)
	{
		snprintf(last_file, sizeof(last_file), "%s", file);
		last_percent = percent;
		console_print("\r%3d%%  maps/%-40.40s", percent, file);
	}
}

int host_prepare_game_data(void)
{
	char image[600];
	char error[512];

	if (has_game_data())
		return 0;
	if (!find_disc_image(image, sizeof(image)))
	{
		console_fail("The game data was not found.\n\n"
			"Put a disc image of Halo: Combat Evolved for the original Xbox (.iso or .xiso)\n"
			"in %s on the SD card, or copy the maps folder from one there\n"
			"(%s/maps/ui.map must exist), then start the game again.", SWITCH_DATA_ROOT, SWITCH_DATA_ROOT);
	}
	console_print("First start: copying the game data out of\n%s\n\n", image);
	host_logf(HOST_LOG_INFO, "extracting the maps folder from %s", image);
	if (!xiso_extract_maps(image, SWITCH_DATA_ROOT, extraction_progress, NULL, error, sizeof(error)))
		console_fail("Could not copy the game data:\n%s", error);
	if (!has_game_data())
		console_fail("The disc image has no maps/ui.map: is it Halo: Combat Evolved for the original Xbox?");
	console_print("\n\nDone. You can delete the disc image now.\nStarting the game...\n");
	svcSleepThread(1500000000LL);
	consoleExit(NULL);
	console_open = 0;
	return 0;
}

/* ---------- the guest's environment */

#define ENVIRONMENT_MAXIMUM 64

struct environment
{
	char *entries[ENVIRONMENT_MAXIMUM];
	int count;
};

static void environment_set(struct environment *environment, const char *name, const char *value)
{
	size_t length = strlen(name);
	char *entry = malloc(length + strlen(value) + 2);

	sprintf(entry, "%s=%s", name, value);
	if (environment->count < ENVIRONMENT_MAXIMUM)
		environment->entries[environment->count++] = entry;
	else
		free(entry);
}

/* POSIX TZ for the console's offset from UTC (the guest's musl has no zone
database) */
static void time_zone(char *buffer, size_t size)
{
	time_t now = time(NULL);
	struct tm local, universal;
	long offset;

	localtime_r(&now, &local);
	gmtime_r(&now, &universal);
	local.tm_isdst = 0;
	universal.tm_isdst = 0;
	offset = (long)difftime(mktime(&universal), mktime(&local));
	snprintf(buffer, size, "<L>%s%ld:%02ld", offset < 0 ? "-" : "", labs(offset) / 3600, (labs(offset) / 60) % 60);
}

/* copies argv and the environment into guest memory */
static uint32_t make_boot(const struct environment *environment)
{
	size_t size = 0x10000;
	char *memory = host_low_map(size, GUEST_PROT_READ | GUEST_PROT_WRITE);
	struct halo_guest_boot *boot = (struct halo_guest_boot *)memory;
	uint32_t *argv;
	uint32_t *environ_list;
	char *strings;
	int index;

	if (!memory)
		host_fatal("cannot allocate the game's environment");
	argv = (uint32_t *)(memory + sizeof(*boot));
	environ_list = argv + 2;
	strings = (char *)(environ_list + ENVIRONMENT_MAXIMUM + 1);
	strcpy(strings, "halo");
	argv[0] = (uint32_t)(uintptr_t)strings;
	argv[1] = 0;
	strings += strlen(strings) + 1;
	for (index = 0; index < environment->count; index++)
	{
		size_t length = strlen(environment->entries[index]) + 1;

		if (strings + length > memory + size)
			break;
		memcpy(strings, environment->entries[index], length);
		environ_list[index] = (uint32_t)(uintptr_t)strings;
		strings += length;
	}
	environ_list[index] = 0;
	boot->argc = 1;
	boot->argv = (uint32_t)(uintptr_t)argv;
	boot->environment = (uint32_t)(uintptr_t)environ_list;
	boot->page_size = 0x1000;
	return (uint32_t)(uintptr_t)boot;
}

static void *read_whole_file(const char *path, size_t *size)
{
	FILE *file = fopen(path, "rb");
	long length;
	void *data;

	if (!file)
		return NULL;
	fseek(file, 0, SEEK_END);
	length = ftell(file);
	fseek(file, 0, SEEK_SET);
	data = length > 0 ? malloc((size_t)length) : NULL;
	if (data && fread(data, 1, (size_t)length, file) != (size_t)length)
	{
		free(data);
		data = NULL;
	}
	fclose(file);
	*size = data ? (size_t)length : 0;
	return data;
}

static void copy_file(const char *from, const char *to)
{
	size_t size = 0;
	void *data = read_whole_file(from, &size);
	FILE *file;

	if (!data)
		return;
	file = fopen(to, "wb");
	if (file)
	{
		fwrite(data, 1, size, file);
		fclose(file);
	}
	free(data);
}

/* ---------- the frame watchdog

Counts the frames the game presents (host_note_frame, from
switch_sdl.c), writes the frame rate to the log every ten seconds, and
notes when no frame has come for three seconds: a game stalled with its
sound still playing looks like controls that do nothing. */

static volatile uint64_t frames_presented;
static volatile uint64_t last_frame_tick;
/* ticks spent in the swap (waiting for the GPU and the display) */
static volatile uint64_t swap_ticks;

void host_note_frame(uint64_t swap_started)
{
	uint64_t now = armGetSystemTick();

	__atomic_add_fetch(&frames_presented, 1, __ATOMIC_RELAXED);
	__atomic_add_fetch(&swap_ticks, now - swap_started, __ATOMIC_RELAXED);
	last_frame_tick = now;
}

/* ---------- clocks

The clocks are read at start-up and when the console is docked or
undocked. With /switch/opence/boost.txt present, the CPU runs at 1785 MHz
(the rate of Nintendo's own CPU boost mode) and the GPU at the top of
Nintendo's normal range for the mode (460.8 MHz handheld, 768 MHz docked)
and the memory at 1600 MHz (the docked rate, which the console also gave
the game in handheld mode on some starts and 1331 MHz on others); the
clocks the game started with come back when it ends. */

#define BOOST_FILE SWITCH_DATA_ROOT "/boost.txt"
#define BOOST_CPU_HZ 1785000000u
#define BOOST_GPU_HANDHELD_HZ 460800000u
#define BOOST_GPU_DOCKED_HZ 768000000u
#define BOOST_MEMORY_HZ 1600000000u

static ClkrstSession cpu_clock, gpu_clock, memory_clock;
static int clocks_open, boosting;
static u32 original_cpu_hz, original_gpu_hz, original_memory_hz;

static void clocks_open_sessions(void)
{
	if (clocks_open)
		return;
	clocks_open = -1;
	if (R_FAILED(clkrstInitialize()))
	{
		host_logf(HOST_LOG_WARN, "clocks: the clock service is not available to this program");
		return;
	}
	if (R_FAILED(clkrstOpenSession(&cpu_clock, PcvModuleId_CpuBus, 3)) ||
		R_FAILED(clkrstOpenSession(&gpu_clock, PcvModuleId_GPU, 3)) ||
		R_FAILED(clkrstOpenSession(&memory_clock, PcvModuleId_EMC, 3)))
	{
		host_logf(HOST_LOG_WARN, "clocks: cannot open the clock sessions");
		return;
	}
	clocks_open = 1;
}

static void clocks_apply(void)
{
	struct stat information;
	u32 cpu = 0, gpu = 0, memory = 0;
	int docked = appletGetOperationMode() == AppletOperationMode_Console;

	clocks_open_sessions();
	if (clocks_open != 1)
		return;
	if (!boosting && stat(BOOST_FILE, &information) == 0)
	{
		clkrstGetClockRate(&cpu_clock, &original_cpu_hz);
		clkrstGetClockRate(&gpu_clock, &original_gpu_hz);
		clkrstGetClockRate(&memory_clock, &original_memory_hz);
		boosting = 1;
	}
	if (boosting)
	{
		clkrstSetClockRate(&cpu_clock, BOOST_CPU_HZ);
		clkrstSetClockRate(&gpu_clock, docked ? BOOST_GPU_DOCKED_HZ : BOOST_GPU_HANDHELD_HZ);
		clkrstSetClockRate(&memory_clock, BOOST_MEMORY_HZ);
	}
	clkrstGetClockRate(&cpu_clock, &cpu);
	clkrstGetClockRate(&gpu_clock, &gpu);
	clkrstGetClockRate(&memory_clock, &memory);
	host_logf(HOST_LOG_INFO, "clocks (%s%s): CPU %u MHz, GPU %.1f MHz, memory %u MHz", docked ? "docked" : "handheld",
		boosting ? ", boost.txt" : "", cpu / 1000000, gpu / 1000000.0, memory / 1000000);
}

static void clocks_restore(void)
{
	if (clocks_open == 1 && boosting)
	{
		clkrstSetClockRate(&cpu_clock, original_cpu_hz);
		clkrstSetClockRate(&gpu_clock, original_gpu_hz);
		clkrstSetClockRate(&memory_clock, original_memory_hz);
	}
}

static void watchdog(void *unused)
{
	uint64_t frequency = armGetSystemTickFreq();
	uint64_t window_start = armGetSystemTick(), window_frames = 0, window_swap = 0, window_faults = 0;
	uint64_t window_gpu = 0;
	AppletOperationMode mode = appletGetOperationMode();
	int stalled = 0;

	(void)unused;
	clocks_apply();
	for (;;)
	{
		uint64_t now, frames, last;

		svcSleepThread(1000000000LL);
		if (appletGetOperationMode() != mode)
		{
			mode = appletGetOperationMode();
			clocks_apply();
		}
		now = armGetSystemTick();
		frames = frames_presented;
		last = last_frame_tick;
		if (!frames)
			continue;
		if (now - last > 3 * frequency)
		{
			uint64_t seconds = (now - last) / frequency;

			if (!stalled)
				host_logf(HOST_LOG_WARN, "no frame drawn for %llu s (%llu so far): the game is stalled",
					(unsigned long long)seconds, (unsigned long long)frames);
			/* where its main thread is, at once and every 5 s after (a level
			loading shows its loading; a hang, where it hangs) */
			if ((!stalled || seconds % 5 == 3) && host_game_thread())
			{
				if (stalled)
					host_logf(HOST_LOG_WARN, "still no frame after %llu s", (unsigned long long)seconds);
				host_report_thread(host_game_thread(), "game");
			}
			stalled = 1;
		}
		else if (stalled)
		{
			host_logf(HOST_LOG_INFO, "frames again");
			stalled = 0;
		}
		if (now - window_start >= 10 * frequency)
		{
			double seconds = (double)(now - window_start) / (double)frequency;
			uint64_t count = frames - window_frames;
			uint64_t swap = swap_ticks, faults = host_memory_watch_faults(), gpu = host_gl_wait_ticks();
			double per_frame = count ? 1000.0 / (double)frequency / (double)count : 0.0;

			/* the frame's time, and the parts of it the CPU spent waiting:
			for the GPU to finish an earlier frame, and in the swap. The rest
			is the game's own work on the CPU: a GPU-bound game waits a lot,
			a CPU-bound one hardly at all */
			host_logf(HOST_LOG_INFO, "%.1f frames a second: %.1f ms a frame, %.1f ms of it waiting for the GPU, "
				"%.1f ms in the swap; %llu texture write faults",
				(double)count / seconds, count ? seconds * 1000.0 / (double)count : 0.0,
				(double)(gpu - window_gpu) * per_frame, (double)(swap - window_swap) * per_frame,
				(unsigned long long)(faults - window_faults));
			window_gpu = gpu;
			window_start = now;
			window_frames = frames;
			window_swap = swap;
			window_faults = faults;
		}
	}
}

static void start_watchdog(void)
{
	static Thread thread;

	if (R_FAILED(threadCreate(&thread, watchdog, NULL, NULL, 0x4000, 0x2c, -2)) || R_FAILED(threadStart(&thread)))
		host_logf(HOST_LOG_WARN, "no frame watchdog");
}

/* ---------- main */

#define MAIN_STACK_SIZE (16 * 1024 * 1024)

static uint32_t boot_block;

static void *game_main(void *unused)
{
	(void)unused;
	host_logf(HOST_LOG_INFO, "the game thread runs; entering the game");
	host_run_guest_main(boot_block);
}

static void check_memory(void)
{
	u64 total = 0;
	AppletType type = appletGetAppletType();

	svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
	host_logf(HOST_LOG_INFO, "applet type %d, %llu MB of memory", (int)type, (unsigned long long)(total >> 20));
	if (type != AppletType_Application && type != AppletType_SystemApplication && total < (1ULL << 30))
	{
		host_logf(HOST_LOG_WARN, "running as an applet with %llu MB: start the Homebrew Menu from a game "
			"(hold R while starting one) for the memory the game needs", (unsigned long long)(total >> 20));
	}
}

int main(int argc, char *argv[])
{
	struct environment environment = { { 0 }, 0 };
	char zone[64];
	size_t image_size = 0;
	void *image;

	(void)argc;
	(void)argv;
	mutexInit(&log_lock);
	mkdir("/switch", 0755);
	mkdir(SWITCH_DATA_ROOT, 0755);
	mkdir(SWITCH_SAVE_ROOT, 0755);
	chdir("sdmc:" SWITCH_DATA_ROOT);
	start_tick = armGetSystemTick();
	log_fd = open(SWITCH_DATA_ROOT "/host.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
	host_logf(HOST_LOG_INFO, "OpenCE for Nintendo Switch starting (build %s); program at %p",
		SWITCH_BUILD, (void *)(uintptr_t)host_program_base());
	check_memory();

	if (R_FAILED(socketInitializeDefault()))
		host_logf(HOST_LOG_WARN, "no network sockets");
	if (R_FAILED(nifmInitialize(NifmServiceType_User)))
		host_logf(HOST_LOG_WARN, "no network information service");
	if (R_FAILED(romfsInit()))
		host_fatal("cannot open the program's own files (romfs)");

	host_prepare_game_data();
	host_logf(HOST_LOG_INFO, "game data found; loading the game image");

	image = read_whole_file("romfs:/halo_guest.elf", &image_size);
	if (!image)
		host_fatal("cannot read the game image (romfs:/halo_guest.elf)");
	if (host_load_image(image, image_size) != 0)
	{
		host_memory_describe();
		host_fatal("Cannot set up the game's memory. If this happens again after starting it again, "
			"see " SWITCH_DATA_ROOT "/host.txt.");
	}
	free(image);
	host_logf(HOST_LOG_INFO, "game image loaded");
	/* internet play's MQTT brokers (network.brokers_file): written beside
	config.toml at each start, as the desktop builds ship it */
	copy_file("romfs:/brokers.txt", SWITCH_DATA_ROOT "/brokers.txt");

	environment_set(&environment, "HOME", SWITCH_SAVE_ROOT);
	environment_set(&environment, "HALO_DATA_ROOT", SWITCH_DATA_ROOT);
	environment_set(&environment, "HALO_SAVE_ROOT", SWITCH_SAVE_ROOT);
	/* 480 lines at 16:9 (the Android build's widescreen, port/android/README.md) */
	environment_set(&environment, "HALO_DISPLAY_WIDTH", "852");
	/* the updater installs Android packages: never here */
	environment_set(&environment, "HALO_UPDATE_AUTO", "false");
	time_zone(zone, sizeof(zone));
	environment_set(&environment, "TZ", zone);
	host_logf(HOST_LOG_INFO, "making the game's environment");
	boot_block = make_boot(&environment);
	host_memory_describe();

	start_watchdog();
	host_profile_start();
	host_logf(HOST_LOG_INFO, "starting the game thread");
	if (host_native_thread_create(game_main, NULL, MAIN_STACK_SIZE, "game") != 0)
		host_fatal("cannot start the game thread");
	/* the game ends the process itself (host_exit); SDL handles the
	applet's messages on the game's thread */
	for (;;)
		svcSleepThread(1000000000LL);
}
