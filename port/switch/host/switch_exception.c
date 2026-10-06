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

static void report(uint32_t type, const ThreadExceptionFrameA64 *frame)
{
	char line[256];
	uint64_t pc = frame->elr_el1;

	snprintf(line, sizeof(line), "exception 0x%x: pc %016llx lr %016llx sp %016llx esr %08x far %016llx",
		type, (unsigned long long)pc, (unsigned long long)frame->lr, (unsigned long long)frame->sp,
		frame->esr, (unsigned long long)frame->far);
	host_log(HOST_LOG_ERROR, line);
	if (host_image.header && pc >= host_image.base && pc < host_image.end)
	{
		snprintf(line, sizeof(line), "  in the guest image: llvm-symbolizer --obj=halo_guest.elf 0x%llx 0x%llx",
			(unsigned long long)pc, (unsigned long long)frame->lr);
		host_log(HOST_LOG_ERROR, line);
	}
	snprintf(line, sizeof(line), "  x0 %016llx x1 %016llx x2 %016llx x3 %016llx",
		(unsigned long long)frame->cpu_gprs[0], (unsigned long long)frame->cpu_gprs[1],
		(unsigned long long)frame->cpu_gprs[2], (unsigned long long)frame->cpu_gprs[3]);
	host_log(HOST_LOG_ERROR, line);
	snprintf(line, sizeof(line), "  x4 %016llx x5 %016llx x6 %016llx x7 %016llx x8 %016llx",
		(unsigned long long)frame->cpu_gprs[4], (unsigned long long)frame->cpu_gprs[5],
		(unsigned long long)frame->cpu_gprs[6], (unsigned long long)frame->cpu_gprs[7],
		(unsigned long long)frame->cpu_gprs[8]);
	host_log(HOST_LOG_ERROR, line);
	/* the guest's frame records (fp, lr pairs); x29 itself is not in the
	kernel's frame, and is not followed here */
}

uint32_t switch_exception_handle(uint32_t type, ThreadExceptionFrameA64 *frame)
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
		report(type, frame);
	}
	return 0xf801; /* KERNELRESULT(UnhandledUserInterrupt) */
}
