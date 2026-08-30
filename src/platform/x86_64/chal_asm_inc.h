/* Segment selectors for the GDT */
#define SEL_RPL_USR 0x3

/*
 * Laid out to match Linux's selector indices, so that IA32_STAR holds the
 * same value in Composite and in a Linux guest and the vmcall fast path does
 * not have to swap it. Linux: __KERNEL_CS 0x10, __USER32_CS 0x23,
 * __USER_DS 0x2b, __USER_CS 0x33 (arch/x86/include/asm/segment.h).
 *
 * The layout is forced by the SYSCALL/SYSRET selector arithmetic, which is
 * fixed in hardware:
 *   SYSCALL  CS = STAR[47:32] & 0xFFFC      SS = STAR[47:32] + 8
 *   SYSRET   CS = (STAR[63:48] + 16) | 3    SS = (STAR[63:48] + 8) | 3
 * With STAR[47:32] = 0x0010 and STAR[63:48] = 0x0023 that gives kernel
 * CS 0x10 / SS 0x18 and user CS 0x33 / SS 0x2b, which is why the entries
 * below sit where they do. SEL_UCSEG32 is the SYSRET base; 64-bit SYSRET
 * never loads it, but the arithmetic is relative to it.
 *
 * GDT index is selector >> 3, so the RPL bits truncate away and the
 * seg_descs[SEL_x / 8] indexing in gdt.c still lands correctly.
 *
 *   0x00  0   null
 *   0x08  1   unused (Linux puts __KERNEL32_CS here)
 *   0x10  2   SEL_KCSEG
 *   0x18  3   SEL_KDSEG
 *   0x23  4   SEL_UCSEG32   (SYSRET base)
 *   0x2b  5   SEL_UDSEG
 *   0x33  6   SEL_UCSEG
 *   0x38  7-8 SEL_TSS       (16-byte descriptor, two slots)
 *   0x4b  9   SEL_UGSEG
 *   0x53  10  SEL_UFSEG
 */
#define SEL_NULL 0x00
#define SEL_KCSEG 0x10                   /* Kernel code selector. */
#define SEL_KDSEG 0x18                   /* Kernel data selector. */
#define SEL_UCSEG32 (0x20 | SEL_RPL_USR) /* 32-bit user code; SYSRET base. */
#define SEL_UDSEG (0x28 | SEL_RPL_USR)   /* User data selector. */
#define SEL_UCSEG (0x30 | SEL_RPL_USR)   /* User code selector. */
#define SEL_TSS 0x38                     /* Task-state segment (2 slots). */
#define SEL_UGSEG (0x48 | SEL_RPL_USR)   /* User TLS selector. */
#define SEL_UFSEG (0x50 | SEL_RPL_USR)   /* User TLS selector in x86_64. */

#define SEL_CNT 11                     /* Number of segments. */

#define STK_INFO_SZ 144                 /* sizeof(struct cos_cpu_local_info) */
#define STK_INFO_OFF (STK_INFO_SZ + 8) /* sizeof(struct cos_cpu_local_info) + sizeof(long) */

#define SMP_BOOT_PATCH_ADDR 0x70000

#define KERNEL_STACK_OFFSET 0
#define USER_STACK_OFFSET 8