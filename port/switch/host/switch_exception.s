// SWITCH_EXCEPTION.S
//
// User-mode exceptions, and running a function on another stack.
//
// hbloader forwards the process's exceptions to the NRO's entry point,
// whose crt0 (libnx switch_crt0.s) jumps to __libnx_exception_entry with
// x0 = the exception type and x1 = the kernel's ThreadExceptionFrameA64
// (x0-x8, lr, sp, pc, pstate, afsr0/1, esr, far). libnx's own entry cannot
// resume the faulting code (its handler ends in svcBreak), so this
// replaces it (the symbol is weak in libnx).
//
// The frame holds only x0-x8, lr, sp, pc and pstate: everything else is
// still in the registers, and svcReturnFromException(0) resumes from the
// frame with them as they are. So this saves x9-x29, the NEON registers and
// FPCR/FPSR, calls switch_exception_handle (switch_exception.c), restores
// them and returns to the kernel: 0 to run the faulting instruction again
// (a watched page made writeable), anything else to have the process
// terminated with a crash report (Atmosphère writes it to the SD card).
//
// The stack pointer at entry is the faulting thread's. Guest code is
// compiled for the Apple arm64 ABI (arm64_32-apple-watchos), which lets a
// function keep 128 bytes below its stack pointer, so this starts further
// down. The kernel restores the thread's stack pointer from the frame.

	.section .text.__libnx_exception_entry, "ax", %progbits
	.global __libnx_exception_entry
	.type __libnx_exception_entry, %function
	.align 2
__libnx_exception_entry:
	sub sp, sp, #0x400
	// sp may not be 16-byte aligned if the fault was a misaligned stack;
	// such a fault is not handled anyway, and stores below work regardless
	stp x9, x10, [sp, #0x000]
	stp x11, x12, [sp, #0x010]
	stp x13, x14, [sp, #0x020]
	stp x15, x16, [sp, #0x030]
	stp x17, x18, [sp, #0x040]
	stp x19, x20, [sp, #0x050]
	stp x21, x22, [sp, #0x060]
	stp x23, x24, [sp, #0x070]
	stp x25, x26, [sp, #0x080]
	stp x27, x28, [sp, #0x090]
	str x29, [sp, #0x0a0]
	mrs x9, fpcr
	mrs x10, fpsr
	stp x9, x10, [sp, #0x0b0]
	stp q0, q1, [sp, #0x0c0]
	stp q2, q3, [sp, #0x0e0]
	stp q4, q5, [sp, #0x100]
	stp q6, q7, [sp, #0x120]
	stp q8, q9, [sp, #0x140]
	stp q10, q11, [sp, #0x160]
	stp q12, q13, [sp, #0x180]
	stp q14, q15, [sp, #0x1a0]
	stp q16, q17, [sp, #0x1c0]
	stp q18, q19, [sp, #0x1e0]
	stp q20, q21, [sp, #0x200]
	stp q22, q23, [sp, #0x220]
	stp q24, q25, [sp, #0x240]
	stp q26, q27, [sp, #0x260]
	stp q28, q29, [sp, #0x280]
	stp q30, q31, [sp, #0x2a0]

	// w0 = type, x1 = frame, x2 = the saved x9-x29 (x9 first)
	mov x2, sp
	bl switch_exception_handle
	str w0, [sp, #0x2c0]

	ldp x9, x10, [sp, #0x0b0]
	msr fpcr, x9
	msr fpsr, x10
	ldp q0, q1, [sp, #0x0c0]
	ldp q2, q3, [sp, #0x0e0]
	ldp q4, q5, [sp, #0x100]
	ldp q6, q7, [sp, #0x120]
	ldp q8, q9, [sp, #0x140]
	ldp q10, q11, [sp, #0x160]
	ldp q12, q13, [sp, #0x180]
	ldp q14, q15, [sp, #0x1a0]
	ldp q16, q17, [sp, #0x1c0]
	ldp q18, q19, [sp, #0x1e0]
	ldp q20, q21, [sp, #0x200]
	ldp q22, q23, [sp, #0x220]
	ldp q24, q25, [sp, #0x240]
	ldp q26, q27, [sp, #0x260]
	ldp q28, q29, [sp, #0x280]
	ldp q30, q31, [sp, #0x2a0]
	ldp x9, x10, [sp, #0x000]
	ldp x11, x12, [sp, #0x010]
	ldp x13, x14, [sp, #0x020]
	ldp x15, x16, [sp, #0x030]
	ldp x17, x18, [sp, #0x040]
	ldp x19, x20, [sp, #0x050]
	ldp x21, x22, [sp, #0x060]
	ldp x23, x24, [sp, #0x070]
	ldp x25, x26, [sp, #0x080]
	ldp x27, x28, [sp, #0x090]
	ldr x29, [sp, #0x0a0]
	// (x0 is restored from the frame by the kernel)
	ldr w0, [sp, #0x2c0]
	svc 0x28 // svcReturnFromException
	b .

// void switch_call_on_stack(void (*function)(void *), void *argument, void *stack_top)
	.section .text.switch_call_on_stack, "ax", %progbits
	.global switch_call_on_stack
	.type switch_call_on_stack, %function
	.align 2
switch_call_on_stack:
	stp x29, x30, [sp, #-32]!
	str x19, [sp, #16]
	mov x29, sp
	mov x19, sp
	and x2, x2, #~0xf
	mov sp, x2
	mov x9, x0
	mov x0, x1
	blr x9
	mov sp, x19
	ldr x19, [sp, #16]
	ldp x29, x30, [sp], #32
	ret
