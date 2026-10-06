/*
SWITCH_MEMORY.C

Guest address space for the Switch port.

Everything the guest touches must lie below 4 GB: its pointers are 32-bit,
the game data holds Xbox addresses in the contiguous window at 0x80000000,
and the image is linked at 0x88000000 (port/android/include/
halo_android_abi.h). An NRO's own memory (its heap, its stacks) is
wherever the kernel put the heap and stack regions, normally far above that.

Horizon offers no mmap. The way to put memory at a chosen address is the
one hbloader uses to load an NRO: take pages from the heap and alias them
at the address with svcMapProcessCodeMemory (which needs the process
handle hbloader passes in its environment; Atmosphère's hbloader permits
all system calls). What the pages may do then follows the kernel's memory
states (mesosphère, kern_k_page_table_base.cpp):

- an alias starts as AliasCode, readable. svcSetProcessMemoryPermission can
  make it read-execute (still AliasCode) or read-write, which turns it into
  AliasCodeData for good: that call only accepts AliasCode (FlagCode);
- AliasCodeData pages can be reprotected with the ordinary
  svcSetMemoryPermission (FlagCanReprotect), between none, read and
  read-write, as often as needed.

So code is written into its heap pages before they are aliased, and made
read-execute once (the image, switch_loader.c); all other guest memory is
made read-write once when it is aliased, and protected afterwards with
svcSetMemoryPermission. Guest memory is never executable otherwise (the
game generates no code).

The kernel accepts an alias anywhere in the address space except the
heap, alias and shadow stack regions, and only over unmapped addresses.
Those regions are placed at random at each launch; if one covers the fixed
guest ranges the game cannot start, and says so (relaunching gives
another layout).

There is no reserving without backing either, so where the Android host
reserves address space and commits it on demand, this file commits whole
pools of memory: the window (128 MB) and the image at start-up, then
64 MB pools as the guest's allocations need them. Pages handed back stay
in their pool for reuse.

This file also implements guest memory write tracking (the interface of
port/linux/src/memory_watch.c): the renderer write-protects the pages
behind the textures it caches; the first write to each is a permission
fault, which switch_exception.c hands to host_memory_watch_fault.
*/

#include "switch_host.h"

#include <switch.h>

#include <errno.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PAGE 0x1000ULL
#define LOW_LIMIT 0x100000000ULL
/* the lowest address the kernel lets a process map (the bottom 128 MB of
a 39-bit address space is never used) */
#define LOW_START 0x08000000ULL
#define POOL_SIZE (64ULL * 1024 * 1024)
#define POOL_PAGES (POOL_SIZE / PAGE)
#define MAXIMUM_POOLS 48
#define POOL_ALIGNMENT 0x200000ULL

/* the guest's mmap flags (Linux, generic) */
#define GUEST_MAP_SHARED 0x01
#define GUEST_MAP_PRIVATE 0x02
#define GUEST_MAP_FIXED 0x10
#define GUEST_MAP_ANONYMOUS 0x20
#define GUEST_MAP_NORESERVE 0x4000
#define GUEST_MAP_FIXED_NOREPLACE 0x100000

struct pool
{
	uint64_t base;
	uint64_t size;
	uint32_t pages;
	uint32_t free_pages;
	void *backing;
	uint8_t *used; /* 1 for each page handed out */
};

static Handle process = INVALID_HANDLE;
static struct pool *pools[MAXIMUM_POOLS];
static int pool_count;
static Mutex memory_lock;

static uint64_t window_base, window_end;
static uint64_t image_base, image_end;

/* the regions an alias may not overlap (the kernel's CanContain for
KMemoryState_AliasCode: heap, alias and, on [23.0.0+], shadow stack) */
static uint64_t heap_region_base, heap_region_end;
static uint64_t alias_region_base, alias_region_end;
static uint64_t shadow_region_base, shadow_region_end;

static uint64_t round_up(uint64_t value, uint64_t alignment)
{
	return (value + alignment - 1) & ~(alignment - 1);
}

static int in_range(uint64_t address, uint64_t size, uint64_t base, uint64_t end)
{
	return address >= base && address + size <= end && address + size >= address;
}

static int overlaps(uint64_t address, uint64_t size, uint64_t base, uint64_t end)
{
	return address < end && base < address + size;
}

/* none, read or read-write (the permissions of data aliases): write
implies read; execute is not available outside the image's code, and reads
as read */
static uint32_t permission_of(int protection)
{
	if (protection & GUEST_PROT_WRITE)
		return Perm_Rw;
	if (protection & (GUEST_PROT_READ | GUEST_PROT_EXEC))
		return Perm_R;
	return Perm_None;
}

/* reprotects data aliases (AliasCodeData) */
static int set_permission(uint64_t address, uint64_t size, uint32_t permission)
{
	Result result = svcSetMemoryPermission((void *)(uintptr_t)address, size, permission);

	if (R_FAILED(result))
	{
		host_logf(HOST_LOG_ERROR, "svcSetMemoryPermission(%010llx, %llx, %u) failed: 0x%x",
			(unsigned long long)address, (unsigned long long)size, permission, result);
		return -1;
	}
	return 0;
}

/* the one-time change of a fresh alias (AliasCode): to read-write (it
becomes AliasCodeData) or read-execute */
static int set_alias_permission(uint64_t address, uint64_t size, uint32_t permission)
{
	Result result = svcSetProcessMemoryPermission(process, address, size, permission);

	if (R_FAILED(result))
	{
		host_logf(HOST_LOG_ERROR, "svcSetProcessMemoryPermission(%010llx, %llx, %u) failed: 0x%x",
			(unsigned long long)address, (unsigned long long)size, permission, result);
		return -1;
	}
	return 0;
}

/* ---------- the address space */

static void read_regions(void)
{
	u64 value = 0, size = 0;

	svcGetInfo(&value, InfoType_HeapRegionAddress, CUR_PROCESS_HANDLE, 0);
	svcGetInfo(&size, InfoType_HeapRegionSize, CUR_PROCESS_HANDLE, 0);
	heap_region_base = value;
	heap_region_end = value + size;
	svcGetInfo(&value, InfoType_AliasRegionAddress, CUR_PROCESS_HANDLE, 0);
	svcGetInfo(&size, InfoType_AliasRegionSize, CUR_PROCESS_HANDLE, 0);
	alias_region_base = value;
	alias_region_end = value + size;
	value = size = 0;
	if (R_SUCCEEDED(svcGetInfo(&value, InfoType_ShadowStackRegionAddress, CUR_PROCESS_HANDLE, 0)) &&
		R_SUCCEEDED(svcGetInfo(&size, InfoType_ShadowStackRegionSize, CUR_PROCESS_HANDLE, 0)))
	{
		shadow_region_base = value;
		shadow_region_end = value + size;
	}
}

/* 1 if [address, address + size) touches a region an alias may not */
static int in_forbidden_region(uint64_t address, uint64_t size)
{
	return overlaps(address, size, heap_region_base, heap_region_end) ||
		overlaps(address, size, alias_region_base, alias_region_end) ||
		(shadow_region_end > shadow_region_base && overlaps(address, size, shadow_region_base, shadow_region_end));
}

void host_memory_describe(void)
{
	u64 aslr = 0, aslr_size = 0, stack = 0, stack_size = 0, total = 0, used = 0;

	svcGetInfo(&aslr, InfoType_AslrRegionAddress, CUR_PROCESS_HANDLE, 0);
	svcGetInfo(&aslr_size, InfoType_AslrRegionSize, CUR_PROCESS_HANDLE, 0);
	svcGetInfo(&stack, InfoType_StackRegionAddress, CUR_PROCESS_HANDLE, 0);
	svcGetInfo(&stack_size, InfoType_StackRegionSize, CUR_PROCESS_HANDLE, 0);
	svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
	svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
	host_logf(HOST_LOG_INFO, "address space %010llx+%llx; heap region %010llx-%010llx; alias region %010llx-%010llx; "
		"stack region %010llx+%llx", (unsigned long long)aslr, (unsigned long long)aslr_size,
		(unsigned long long)heap_region_base, (unsigned long long)heap_region_end,
		(unsigned long long)alias_region_base, (unsigned long long)alias_region_end,
		(unsigned long long)stack, (unsigned long long)stack_size);
	host_logf(HOST_LOG_INFO, "memory: %llu MB of %llu MB in use; %d guest pools",
		(unsigned long long)(used >> 20), (unsigned long long)(total >> 20), pool_count);
}

/* 1 if [address, address + size) is unmapped and an alias may go there */
static int range_available(uint64_t address, uint64_t size)
{
	uint64_t cursor = address;

	if (address < LOW_START || address + size > LOW_LIMIT)
		return 0;
	if (in_forbidden_region(address, size))
		return 0;
	while (cursor < address + size)
	{
		MemoryInfo information;
		u32 page_information;

		if (R_FAILED(svcQueryMemory(&information, &page_information, cursor)))
			return 0;
		if (information.type != MemType_Unmapped)
			return 0;
		cursor = information.addr + information.size;
		if (cursor <= information.addr)
			break;
	}
	return 1;
}

/* the lowest place at or above minimum (and below limit) where size bytes
can go; 0 if none */
static uint64_t find_gap(uint64_t size, uint64_t minimum, uint64_t limit)
{
	uint64_t cursor = round_up(minimum, POOL_ALIGNMENT);

	while (cursor + size <= limit)
	{
		MemoryInfo information;
		u32 page_information;
		uint64_t block_end;

		if (overlaps(cursor, size, heap_region_base, heap_region_end))
		{
			cursor = round_up(heap_region_end, POOL_ALIGNMENT);
			continue;
		}
		if (overlaps(cursor, size, alias_region_base, alias_region_end))
		{
			cursor = round_up(alias_region_end, POOL_ALIGNMENT);
			continue;
		}
		if (shadow_region_end > shadow_region_base && overlaps(cursor, size, shadow_region_base, shadow_region_end))
		{
			cursor = round_up(shadow_region_end, POOL_ALIGNMENT);
			continue;
		}
		if (R_FAILED(svcQueryMemory(&information, &page_information, cursor)))
			return 0;
		block_end = information.addr + information.size;
		if (information.type == MemType_Unmapped && block_end >= cursor + size)
			return cursor;
		/* past this block (mapped, or free but too small) */
		cursor = round_up(block_end, POOL_ALIGNMENT);
	}
	return 0;
}

/* zeroed heap pages for an alias of size bytes; NULL if out of memory */
static void *backing_new(uint64_t size)
{
	/* a spare page after the range keeps the heap allocator's next chunk
	header off the pages that the alias makes inaccessible at their heap
	address */
	void *backing = memalign(PAGE, size + PAGE);

	if (!backing)
	{
		host_logf(HOST_LOG_ERROR, "out of memory for %llu MB of guest memory", (unsigned long long)(size >> 20));
		return NULL;
	}
	memset(backing, 0, size);
	return backing;
}

/* aliases backing at address (the alias is readable, AliasCode); 0 on
success */
static int alias(uint64_t address, uint64_t size, void *backing)
{
	Result result = svcMapProcessCodeMemory(process, address, (u64)(uintptr_t)backing, size);

	if (R_FAILED(result))
	{
		host_logf(HOST_LOG_ERROR, "svcMapProcessCodeMemory(%010llx, %llx) failed: 0x%x",
			(unsigned long long)address, (unsigned long long)size, result);
		return -1;
	}
	return 0;
}

/* backs [address, address + size) with zeroed heap pages as a data alias,
read-write; returns the backing allocation, or NULL */
static void *map_backed(uint64_t address, uint64_t size)
{
	void *backing = backing_new(size);

	if (!backing)
		return NULL;
	if (alias(address, size, backing) != 0)
	{
		free(backing);
		return NULL;
	}
	if (set_alias_permission(address, size, Perm_Rw) != 0)
	{
		svcUnmapProcessCodeMemory(process, address, (u64)(uintptr_t)backing, size);
		free(backing);
		return NULL;
	}
	return backing;
}

void *host_image_backing(uint32_t size)
{
	return backing_new(round_up(size, PAGE));
}

int host_image_map(void *backing, const uint8_t *page_permissions)
{
	uint64_t pages = (image_end - image_base) / PAGE, page = 0;

	if (alias(image_base, image_end - image_base, backing) != 0)
		return -1;
	/* runs of pages of one permission: read-execute and read-write are
	set once (read-write ones become data aliases); read stays as it is */
	while (page < pages)
	{
		uint64_t run = 1;
		uint8_t permission = page_permissions[page];

		while (page + run < pages && page_permissions[page + run] == permission)
			run++;
		if (permission != Perm_R &&
			set_alias_permission(image_base + page * PAGE, run * PAGE, permission) != 0)
		{
			return -1;
		}
		page += run;
	}
	return 0;
}

int host_memory_initialize(uint32_t base, uint32_t size)
{
	mutexInit(&memory_lock);
	process = envGetOwnProcessHandle();
	if (process == INVALID_HANDLE || !envIsSyscallHinted(0x73) || !envIsSyscallHinted(0x77))
	{
		host_logf(HOST_LOG_ERROR, "this homebrew loader gives no process handle or no code memory system calls; "
			"run the game from the Homebrew Menu on Atmosphère");
		return -1;
	}
	read_regions();
	host_memory_describe();

	window_base = HALO_GUEST_WINDOW_BASE;
	window_end = window_base + HALO_GUEST_WINDOW_SIZE;
	image_base = base;
	image_end = base + round_up(size, PAGE);
	if (!range_available(window_base, HALO_GUEST_WINDOW_SIZE) || !range_available(image_base, image_end - image_base))
	{
		host_logf(HOST_LOG_ERROR, "the fixed guest memory (%08llx-%08llx) is not free in this launch's address space "
			"layout; start the game again", (unsigned long long)window_base, (unsigned long long)image_end);
		return -1;
	}
	if (!map_backed(window_base, HALO_GUEST_WINDOW_SIZE))
		return -1;
	/* the window starts inaccessible: the guest maps pages of it as it
	allocates (port/linux/src/xbox_memory.c) */
	if (set_permission(window_base, HALO_GUEST_WINDOW_SIZE, Perm_None) != 0)
		return -1;
	/* (the image is mapped by host_image_map, once its pages are filled) */
	host_logf(HOST_LOG_INFO, "guest window %08llx-%08llx, image %08llx-%08llx",
		(unsigned long long)window_base, (unsigned long long)window_end,
		(unsigned long long)image_base, (unsigned long long)image_end);
	return 0;
}

/* ---------- page pools */

static struct pool *pool_new(uint64_t size)
{
	uint64_t address;
	struct pool *pool;

	if (pool_count == MAXIMUM_POOLS)
		return NULL;
	size = round_up(size, POOL_ALIGNMENT);
	/* above the image first, then below the window */
	address = find_gap(size, image_end, LOW_LIMIT);
	if (!address)
		address = find_gap(size, LOW_START, HALO_GUEST_WINDOW_BASE);
	if (!address)
	{
		host_logf(HOST_LOG_ERROR, "no room below 4 GB for %llu MB more guest memory", (unsigned long long)(size >> 20));
		return NULL;
	}
	pool = calloc(1, sizeof(*pool));
	if (!pool)
		return NULL;
	pool->pages = (uint32_t)(size / PAGE);
	pool->used = calloc(pool->pages, 1);
	pool->backing = pool->used ? map_backed(address, size) : NULL;
	if (!pool->backing)
	{
		free(pool->used);
		free(pool);
		return NULL;
	}
	/* free pages are inaccessible */
	set_permission(address, size, Perm_None);
	pool->base = address;
	pool->size = size;
	pool->free_pages = pool->pages;
	pools[pool_count++] = pool;
	host_logf(HOST_LOG_INFO, "guest memory pool %d at %08llx (%llu MB)", pool_count - 1,
		(unsigned long long)address, (unsigned long long)(size >> 20));
	return pool;
}

static uint64_t pool_take(struct pool *pool, uint64_t pages)
{
	uint64_t run = 0, page;

	if (pool->free_pages < pages)
		return 0;
	for (page = 0; page < pool->pages; page++)
	{
		if (pool->used[page])
		{
			run = 0;
			continue;
		}
		if (++run == pages)
		{
			uint64_t first = page + 1 - pages;

			memset(&pool->used[first], 1, pages);
			pool->free_pages -= (uint32_t)pages;
			return pool->base + first * PAGE;
		}
	}
	return 0;
}

static struct pool *pool_of(uint64_t address, uint64_t size)
{
	int index;

	for (index = 0; index < pool_count; index++)
	{
		if (in_range(address, size, pools[index]->base, pools[index]->base + pools[index]->size))
			return pools[index];
	}
	return NULL;
}

/* fresh pages: zeroed, then given their protection */
static int pages_fresh(uint64_t address, uint64_t size, int protection)
{
	uint32_t permission = permission_of(protection);

	if (set_permission(address, size, Perm_Rw) != 0)
		return -1;
	memset((void *)(uintptr_t)address, 0, size);
	if (permission != Perm_Rw && set_permission(address, size, permission) != 0)
		return -1;
	return 0;
}

void *host_low_map(size_t size, int protection)
{
	uint64_t pages = round_up(size, PAGE) / PAGE;
	uint64_t address = 0;
	int index;

	if (!pages)
		return NULL;
	mutexLock(&memory_lock);
	for (index = 0; index < pool_count && !address; index++)
		address = pool_take(pools[index], pages);
	if (!address)
	{
		struct pool *pool = pool_new(pages > POOL_PAGES ? pages * PAGE : POOL_SIZE);

		if (pool)
			address = pool_take(pool, pages);
	}
	mutexUnlock(&memory_lock);
	if (!address)
		return NULL;
	if (pages_fresh(address, pages * PAGE, protection) != 0)
	{
		host_low_unmap((void *)(uintptr_t)address, pages * PAGE);
		return NULL;
	}
	return (void *)(uintptr_t)address;
}

void host_low_unmap(void *address, size_t size)
{
	uint64_t start = (uint64_t)(uintptr_t)address & ~(PAGE - 1);
	uint64_t length = round_up((uint64_t)(uintptr_t)address + size, PAGE) - start;
	struct pool *pool;

	mutexLock(&memory_lock);
	pool = pool_of(start, length);
	if (pool)
	{
		uint64_t first = (start - pool->base) / PAGE, count = length / PAGE, page;

		/* the memory stays in the pool, inaccessible until reused */
		set_permission(start, length, Perm_None);
		for (page = first; page < first + count; page++)
		{
			if (pool->used[page])
			{
				pool->used[page] = 0;
				pool->free_pages++;
			}
		}
	}
	mutexUnlock(&memory_lock);
}

int host_low_owns(uintptr_t address, size_t size)
{
	int result;

	if (in_range(address, size, window_base, window_end) || in_range(address, size, image_base, image_end))
		return 1;
	mutexLock(&memory_lock);
	result = pool_of(address, size) != NULL;
	mutexUnlock(&memory_lock);
	return result;
}

int host_low_protect(uintptr_t address, size_t size, int protection)
{
	uint64_t start = (uint64_t)address & ~(PAGE - 1);
	uint64_t length = round_up((uint64_t)address + size, PAGE) - start;

	if (!length)
		return 0;
	if (!host_low_owns(start, length))
		return -EINVAL;
	return set_permission(start, length, permission_of(protection)) == 0 ? 0 : -EACCES;
}

/* ---------- the guest's memory system calls */

/* the mapping's contents from a file (a private mapping: changes do not
reach the file) */
static int read_file_into(uint64_t address, uint64_t length, int fd, int64_t offset)
{
	static int warned;
	off_t previous = lseek(fd, 0, SEEK_CUR);
	uint64_t done = 0;

	if (previous < 0 || lseek(fd, (off_t)offset, SEEK_SET) < 0)
		return -EBADF;
	while (done < length)
	{
		ssize_t count = read(fd, (char *)(uintptr_t)address + done, (size_t)(length - done));

		if (count <= 0)
			break;
		done += (uint64_t)count;
	}
	lseek(fd, previous, SEEK_SET);
	if (!warned)
	{
		warned = 1;
		host_logf(HOST_LOG_INFO, "the guest maps a file (copied in; shared mappings are not supported)");
	}
	return 0;
}

long host_guest_mmap(uint64_t address, uint64_t size, int protection, int flags, int fd, int64_t offset)
{
	uint64_t length = round_up(size, PAGE);
	int anonymous = (flags & GUEST_MAP_ANONYMOUS) || fd < 0;
	void *result;

	if (!length)
		return -EINVAL;
	if (flags & (GUEST_MAP_FIXED | GUEST_MAP_FIXED_NOREPLACE))
	{
		struct pool *pool;

		if (address & (PAGE - 1))
			return -EINVAL;
		/* only inside memory mapped for the guest: there a "no replace"
		request replaces what is there, as the Android host's reservations
		are replaced */
		if (!host_low_owns(address, length))
			return (flags & GUEST_MAP_FIXED_NOREPLACE) ? -EEXIST : -ENOMEM;
		if (in_range(address, length, image_base, image_end))
			host_logf(HOST_LOG_WARN, "the guest maps over its own image at %08llx", (unsigned long long)address);
		mutexLock(&memory_lock);
		pool = pool_of(address, length);
		if (pool)
		{
			uint64_t first = (address - pool->base) / PAGE, page;

			for (page = first; page < first + length / PAGE; page++)
			{
				if (!pool->used[page])
				{
					pool->used[page] = 1;
					pool->free_pages--;
				}
			}
		}
		mutexUnlock(&memory_lock);
		if (protection == GUEST_PROT_NONE && anonymous)
		{
			/* a reservation: the pages are cleared when mapped again */
			return set_permission(address, length, Perm_None) == 0 ? (long)address : -ENOMEM;
		}
		if (pages_fresh(address, length, anonymous ? protection : GUEST_PROT_READ | GUEST_PROT_WRITE) != 0)
			return -ENOMEM;
		if (!anonymous)
		{
			read_file_into(address, length, fd, offset);
			host_low_protect(address, length, protection);
		}
		return (long)address;
	}
	result = host_low_map(length, anonymous ? protection : GUEST_PROT_READ | GUEST_PROT_WRITE);
	if (!result)
		return -ENOMEM;
	if (!anonymous)
	{
		read_file_into((uint64_t)(uintptr_t)result, length, fd, offset);
		host_low_protect((uintptr_t)result, length, protection);
	}
	(void)GUEST_MAP_SHARED;
	(void)GUEST_MAP_PRIVATE;
	(void)GUEST_MAP_NORESERVE;
	return (long)(uintptr_t)result;
}

long host_guest_munmap(uint64_t address, uint64_t size)
{
	uint64_t length = round_up(size, PAGE);

	if ((address & (PAGE - 1)) || !length || address + length > LOW_LIMIT)
		return -EINVAL;
	if (in_range(address, length, window_base, window_end))
		return set_permission(address, length, Perm_None) == 0 ? 0 : -EINVAL;
	if (in_range(address, length, image_base, image_end))
		return -EINVAL;
	if (host_low_owns(address, length))
	{
		host_low_unmap((void *)(uintptr_t)address, length);
		return 0;
	}
	return -EINVAL;
}

long host_guest_mprotect(uint64_t address, uint64_t size, int protection)
{
	if (address + size > LOW_LIMIT)
		return -EINVAL;
	return host_low_protect((uintptr_t)address, (size_t)size, protection);
}

/* ---------- write tracking (port/linux/src/memory_watch.c) */

#define WATCH_PAGE_COUNT (HALO_GUEST_WINDOW_SIZE / PAGE)

static uint8_t page_protected[WATCH_PAGE_COUNT];
static uint32_t page_generation[WATCH_PAGE_COUNT];
static volatile uint32_t current_generation = 1;
static int watch_active;
/* set when the kernel would not protect a page (out of memory blocks):
from then on every range reads as just written, so the renderer reloads
textures rather than ever drawing stale ones */
static volatile int watch_degraded;

static int in_window(uint64_t address)
{
	return address >= HALO_GUEST_WINDOW_BASE && address - HALO_GUEST_WINDOW_BASE < HALO_GUEST_WINDOW_SIZE;
}

static uint64_t watch_page(uint64_t address)
{
	return (address - HALO_GUEST_WINDOW_BASE) / PAGE;
}

static void mark_written(uint64_t page)
{
	page_generation[page] = __atomic_add_fetch(&current_generation, 1, __ATOMIC_SEQ_CST);
	page_protected[page] = 0;
	set_permission(HALO_GUEST_WINDOW_BASE + page * PAGE, PAGE, Perm_Rw);
}

int host_memory_watch_fault(uint64_t address)
{
	uint64_t page;
	MemoryInfo information;
	u32 page_information;

	if (!watch_active || !in_window(address))
		return 0;
	page = watch_page(address);
	if (page_protected[page])
	{
		mark_written(page);
		return 1;
	}
	/* another thread's write to the same page was handled first: the
	page is writeable now, so the write only needs to be made again */
	if (R_SUCCEEDED(svcQueryMemory(&information, &page_information, address)) && (information.perm & Perm_W))
		return 1;
	return 0;
}

void host_memory_watch_initialize(void)
{
	watch_active = 1;
}

void host_memory_watch_protect(uint32_t address, uint32_t size)
{
	uint64_t first, last, page;

	if (!watch_active || watch_degraded || !size || !in_window(address))
		return;
	first = watch_page(address);
	last = watch_page((uint64_t)address + size - 1);
	if (last >= WATCH_PAGE_COUNT)
		last = WATCH_PAGE_COUNT - 1;
	for (page = first; page <= last; page++)
	{
		if (page_protected[page])
			continue;
		page_protected[page] = 1;
		if (R_FAILED(svcSetMemoryPermission((void *)(uintptr_t)(HALO_GUEST_WINDOW_BASE + page * PAGE), PAGE, Perm_R)))
		{
			page_protected[page] = 0;
			if (!watch_degraded)
			{
				watch_degraded = 1;
				host_logf(HOST_LOG_WARN, "cannot write-protect guest pages any more; textures are checked every time");
			}
			return;
		}
	}
}

uint32_t host_memory_watch_serial(void)
{
	if (watch_degraded)
		return __atomic_add_fetch(&current_generation, 1, __ATOMIC_SEQ_CST);
	return current_generation;
}

uint32_t host_memory_watch_generation(uint32_t address, uint32_t size)
{
	uint64_t first, last, page;
	uint32_t newest = 0;

	if (!size || !in_window(address))
		return 0;
	if (watch_degraded)
		return __atomic_add_fetch(&current_generation, 1, __ATOMIC_SEQ_CST);
	first = watch_page(address);
	last = watch_page((uint64_t)address + size - 1);
	if (last >= WATCH_PAGE_COUNT)
		last = WATCH_PAGE_COUNT - 1;
	for (page = first; page <= last; page++)
	{
		if (page_generation[page] > newest)
			newest = page_generation[page];
	}
	return newest;
}

void host_memory_watch_prepare_write(uint32_t address, uint32_t size)
{
	uint64_t start = address, first, last, page;

	if (!watch_active || !size)
		return;
	if (start + size <= HALO_GUEST_WINDOW_BASE || start >= (uint64_t)HALO_GUEST_WINDOW_BASE + HALO_GUEST_WINDOW_SIZE)
		return;
	if (start < HALO_GUEST_WINDOW_BASE)
		start = HALO_GUEST_WINDOW_BASE;
	first = watch_page(start);
	last = watch_page((uint64_t)address + size - 1);
	if (last >= WATCH_PAGE_COUNT)
		last = WATCH_PAGE_COUNT - 1;
	for (page = first; page <= last; page++)
	{
		if (page_protected[page])
			mark_written(page);
	}
}

void host_memory_watch_forget(uint32_t address, uint32_t size)
{
	uint64_t first, last, page;

	if (!size || !in_window(address))
		return;
	first = watch_page(address);
	last = watch_page((uint64_t)address + size - 1);
	if (last >= WATCH_PAGE_COUNT)
		last = WATCH_PAGE_COUNT - 1;
	for (page = first; page <= last; page++)
	{
		page_protected[page] = 0;
		page_generation[page] = __atomic_add_fetch(&current_generation, 1, __ATOMIC_SEQ_CST);
	}
}
