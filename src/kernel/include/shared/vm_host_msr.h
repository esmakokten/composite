#pragma once

/*
 * The host's values of the SYSCALL registers, shared with the VMM.
 *
 * A Linux guest writes the same values (its IA32_FMASK, and its IA32_STAR
 * once the host GDT follows Linux's selector layout), so the registers
 * coincide on both sides of a VM crossing and the exit path leaves them
 * alone. The VMM intercepts the guest's writes and compares them with these;
 * a guest whose value differs takes the fallback swap (CAPTBL_OP_VM_VCPU_MSR_SET).
 */

/* Linux's IA32_FMASK: CF PF AF ZF SF TF IF DF OF IOPL NT RF AC ID (arch/x86/kernel/cpu/common.c). */
#define COS_HOST_FMASK 0x257FD5ULL

/*
 * The host's IA32_STAR with the GDT laid out to Linux's indices (chal_asm_inc.h):
 * SYSCALL CS 0x10, SYSRET base 0x23. Linux's value on x86_64.
 */
#define COS_HOST_STAR 0x0023001000000000ULL
