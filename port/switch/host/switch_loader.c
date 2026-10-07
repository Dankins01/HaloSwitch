/*
SWITCH_LOADER.C

Loads the guest image (the Android port's halo_guest.elf, a statically
linked AArch64 executable built from ILP32 code, tools/android_build.py):
its segments are copied to the addresses they were linked at, below 4 GB,
and its import table is filled with this host's functions
(port/android/host/host_loader.c does the same on Android).
*/

#include "switch_host.h"

#include <switch.h>

#include <stdlib.h>
#include <string.h>

/* the ELF definitions used here (newlib has no elf.h) */
#define ELFMAG "\177ELF"
#define SELFMAG 4
#define EI_CLASS 4
#define ELFCLASS64 2
#define EM_AARCH64 183
#define ET_EXEC 2
#define PT_LOAD 1
#define PF_X 1
#define PF_W 2

/* a guest address's place in the image being built */
#define AT(address) ((char *)backing + ((uint64_t)(address) - low))

typedef struct
{
	unsigned char e_ident[16];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
} Elf64_Ehdr;

typedef struct
{
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
} Elf64_Phdr;

struct host_guest_image host_image;

static void missing_import(void)
{
	host_fatal("the game called a host function that this Switch build does not have");
}

int host_load_image(const void *file, size_t size)
{
	const Elf64_Ehdr *elf = file;
	const Elf64_Phdr *segments;
	uint64_t low = ~0ULL, high = 0;
	const struct halo_guest_header *header;
	uint64_t *table;
	const char *name;
	uint32_t count, index;
	int missing = 0;
	void *backing;
	uint8_t *permissions;

	if (size < sizeof(*elf) || memcmp(elf->e_ident, ELFMAG, SELFMAG) || elf->e_ident[EI_CLASS] != ELFCLASS64 ||
		elf->e_machine != EM_AARCH64 || elf->e_type != ET_EXEC)
	{
		host_logf(HOST_LOG_ERROR, "the guest image is not an AArch64 executable");
		return -1;
	}
	segments = (const Elf64_Phdr *)((const char *)file + elf->e_phoff);
	for (index = 0; index < elf->e_phnum; index++)
	{
		if (segments[index].p_type != PT_LOAD)
			continue;
		if (segments[index].p_vaddr < low)
			low = segments[index].p_vaddr;
		if (segments[index].p_vaddr + segments[index].p_memsz > high)
			high = segments[index].p_vaddr + segments[index].p_memsz;
	}
	low &= ~0xfffULL;
	high = (high + 0xfff) & ~0xfffULL;
	if (low != HALO_GUEST_IMAGE_BASE || high > 0x100000000ULL)
	{
		host_logf(HOST_LOG_ERROR, "the guest image spans %llx-%llx", (unsigned long long)low, (unsigned long long)high);
		return -1;
	}
	/* the window, and a check that the image's range is free */
	if (host_memory_initialize((uint32_t)low, (uint32_t)(high - low)) != 0)
		return -1;

	/* the image is built in heap pages (zeroed, so .bss is ready) and only
	then aliased at its address: its code pages cannot be written once
	they are mapped (switch_memory.c) */
	backing = host_image_backing((uint32_t)(high - low));
	permissions = calloc((size_t)((high - low) / 0x1000), 1);
	if (!backing || !permissions)
		return -1;
	memset(permissions, Perm_R, (size_t)((high - low) / 0x1000));
	for (index = 0; index < elf->e_phnum; index++)
	{
		const Elf64_Phdr *segment = &segments[index];
		uint64_t first, last, page;

		if (segment->p_type != PT_LOAD)
			continue;
		if (segment->p_offset + segment->p_filesz > size || segment->p_filesz > segment->p_memsz)
			return -1;
		memcpy(AT(segment->p_vaddr), (const char *)file + segment->p_offset, segment->p_filesz);
		first = ((segment->p_vaddr & ~0xfffULL) - low) / 0x1000;
		last = ((segment->p_vaddr + segment->p_memsz + 0xfff) & ~0xfffULL) - low;
		for (page = first; page < last / 0x1000; page++)
		{
			if (segment->p_flags & PF_W)
			{
				if (permissions[page] == Perm_Rx)
					host_logf(HOST_LOG_WARN, "guest image page %08llx is both code and data; it stays code",
						(unsigned long long)(low + page * 0x1000));
				else
					permissions[page] = Perm_Rw;
			}
			else if (segment->p_flags & PF_X)
				permissions[page] = Perm_Rx;
		}
	}

	header = (const struct halo_guest_header *)AT(low);
	if (header->magic != HALO_GUEST_MAGIC || header->abi_version != HALO_GUEST_ABI_VERSION)
	{
		host_logf(HOST_LOG_ERROR, "the guest image header does not match this host");
		return -1;
	}

	table = (uint64_t *)AT(header->import_table);
	name = (const char *)AT(header->import_names);
	count = *(const uint32_t *)AT(header->import_count);
	for (index = 0; index < count; index++)
	{
		void *function = host_resolve_import(name);

		if (!function && !strncmp(name, "hostgl_", 7))
			function = host_gl_resolve(name + 7);
		if (!function)
		{
			host_logf(HOST_LOG_WARN, "guest import %s is not available", name);
			function = (void *)missing_import;
			missing++;
		}
		table[index] = (uint64_t)(uintptr_t)function;
		name += strlen(name) + 1;
	}
	host_logf(HOST_LOG_INFO, "guest image %08llx-%08llx, %u imports (%d unavailable)",
		(unsigned long long)low, (unsigned long long)high, count, missing);

	/* the code was written through the data cache at its heap address:
	clean it to memory before it runs from the alias */
	armDCacheFlush(backing, high - low);
	host_logf(HOST_LOG_INFO, "mapping the guest image");
	if (host_image_map(backing, permissions) != 0)
	{
		host_logf(HOST_LOG_ERROR, "cannot map the guest image");
		return -1;
	}
	free(permissions);
	armICacheInvalidate((void *)(uintptr_t)low, high - low);
	host_logf(HOST_LOG_INFO, "guest image mapped");

	host_image.header = (const struct halo_guest_header *)(uintptr_t)low;
	host_image.base = (uint32_t)low;
	host_image.end = (uint32_t)high;
	return 0;
}
