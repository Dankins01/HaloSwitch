/*
SWITCH_EXCEPTION.C

What a user-mode exception means (switch_exception.s calls this on the
faulting thread, with the kernel's frame).

A write to a page the renderer watches (switch_memory.c) is the expected
one: the page becomes writeable and the write runs again. Anything else is
a crash: its registers go to the log, with the program counter as an
address in the guest image where it is one (symbolize it with
llvm-symbolizer --obj=halo_guest.elf), and the kernel ends the process.
*/

#include "switch_host.h"

#include <switch.h>

#include <stdio.h>

/* ESR_EL1: the exception class, and for data aborts the write bit and the
fault status code */
#define ESR_CLASS(esr) (((esr) >> 26) & 0x3f)
#define ESR_CLASS_DATA_ABORT_LOWER 0x24
#define ESR_WRITE(esr) (((esr) >> 6) & 1)
#define ESR_STATUS(esr) ((esr) & 0x3f)
/* permission faults, levels 1 to 3 */
#define IS_PERMISSION_FAULT(status) ((status) >= 0x0d && (status) <= 0x0f)

static volatile int crashing;

/* the program's load address (libnx's switch.ld) */
extern char __start__[];

static void where(char *buffer, size_t size, uint64_t address)
{
	uint64_t program = (uint64_t)(uintptr_t)__start__;

	if (host_image.header && address >= host_image.base && address < host_image.end)
		snprintf(buffer, size, "%016llx (game image)", (unsigned long long)address);
	else if (address >= program && address - program < 0x10000000)
		snprintf(buffer, size, "%016llx (opence.elf +0x%llx)", (unsigned long long)address,
			(unsigned long long)(address - program));
	else
		snprintf(buffer, size, "%016llx", (unsigned long long)address);
}

/* saved: x9 to x29, as switch_exception.s stored them */
static void report(uint32_t type, const ThreadExceptionFrameA64 *frame, const uint64_t *saved)
{
	char line[320], pc[64], lr[64];
	uint64_t registers[31];
	uint64_t fp;
	int index;

	for (index = 0; index < 9; index++)
		registers[index] = frame->cpu_gprs[index];
	for (index = 9; index < 30; index++)
		registers[index] = saved[index - 9];
	registers[30] = frame->lr;
	where(pc, sizeof(pc), frame->elr_el1);
	where(lr, sizeof(lr), frame->lr);
	snprintf(line, sizeof(line), "CRASH: exception 0x%x, esr %08x, address %016llx", type, frame->esr,
		(unsigned long long)frame->far);
	host_log_crash(line);
	snprintf(line, sizeof(line), "  pc %s", pc);
	host_log_crash(line);
	snprintf(line, sizeof(line), "  lr %s, sp %016llx", lr, (unsigned long long)frame->sp);
	host_log_crash(line);
	for (index = 0; index < 31; index += 4)
	{
		snprintf(line, sizeof(line), "  x%-2d %016llx %016llx %016llx %016llx", index,
			(unsigned long long)registers[index], (unsigned long long)(index + 1 < 31 ? registers[index + 1] : 0),
			(unsigned long long)(index + 2 < 31 ? registers[index + 2] : 0),
			(unsigned long long)(index + 3 < 31 ? registers[index + 3] : 0));
		host_log_crash(line);
	}
	/* the frame records (fp, lr pairs), where they are readable */
	fp = registers[29];
	for (index = 0; index < 24 && fp && (fp & 7) == 0; index++)
	{
		MemoryInfo information;
		u32 page_information;
		const uint64_t *record = (const uint64_t *)(uintptr_t)fp;

		if (R_FAILED(svcQueryMemory(&information, &page_information, fp)) || !(information.perm & Perm_R) ||
			fp + 16 > information.addr + information.size)
		{
			break;
		}
		where(lr, sizeof(lr), record[1]);
		snprintf(line, sizeof(line), "  frame %2d: %s", index, lr);
		host_log_crash(line);
		fp = record[0];
	}
}

uint32_t switch_exception_handle(uint32_t type, ThreadExceptionFrameA64 *frame, const uint64_t *saved)
{
	uint32_t esr = frame->esr;

	if (ESR_CLASS(esr) == ESR_CLASS_DATA_ABORT_LOWER && ESR_WRITE(esr) && IS_PERMISSION_FAULT(ESR_STATUS(esr)) &&
		host_memory_watch_fault(frame->far))
	{
		return 0;
	}
	/* (a fault while reporting one: give up at once) */
	if (!crashing)
	{
		crashing = 1;
		report(type, frame, saved);
	}
	return 0xf801; /* KERNELRESULT(UnhandledUserInterrupt) */
}
