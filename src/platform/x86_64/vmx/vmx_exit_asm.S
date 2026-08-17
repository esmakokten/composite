.section .text
.code64

.global vmx_exit_handler_asm
.extern vmx_exit_handler
.extern vmx_vmcall_fast_handler
.extern host_msr_cache

/*
 * Scratch space for the assembly-only MSR-swap benchmark path below
 * (guest values saved here while host_msr_cache values are loaded,
 * then written straight back before vmresume). Single-CPU only,
 * matching host_msr_cache[0] (NUM_CPU == 1 in this build).
 */
.section .bss
.align 8
vmcall_asm_guest_msr_scratch:
	.zero 56

.section .text
.code64

/*
 * VM exit entry point. On entry, GP registers hold guest values.
 *
 * Fast path: check exit reason immediately. If VMCALL (18),
 * skip full GP save — guest regs use native Composite IPC layout:
 *   rax = encoded sinv cap: (cap + 1) << 16
 *   rbx = arg1, rsi = arg2, rdi = arg3, rdx = arg4
 * Pass these directly to vmx_vmcall_fast_handler.
 *
 * Slow path: push all 15 GPs + cr2, call vmx_exit_handler.
 */
vmx_exit_handler_asm:
	/* Save rax (clobbered by vmread) */
	pushq %rax

	/* vmread EXIT_REASON into rax: encoding = 0x4402 */
	movq $0x4402, %rax
	vmread %rax, %rax

	/* Check for VMCALL (reason 18, low 16 bits) */
	andq $0xffff, %rax
	cmpq $18, %rax
	jne vmx_slow_path

/* vmcall_opt: measurement for vmcall_hw_floor:
	 Advance GUEST_RIP past the VMCALL so the guest loop makes progress.
	 * VMCALL is a fixed 3-byte encoding (0F 01 C1, no operands), so its
	 * exit instruction length is always 3 -- skip the VM_EXIT_INSTRUCTION_LEN
	 * vmread to keep this floor path as close to bare hardware cost as
	
	* possible. rax (guest's original value) is still saved on the stack;
	 * rdx must also be saved here since it's live guest state, not
	 * auto-restored by hardware on VM entry. */
	pushq %rdx

	movq $0x681e, %rax	/* GUEST_RIP encoding */
	vmread %rax, %rax	/* rax = current guest RIP */
	addq $3, %rax		/* rax = RIP of next instruction (VMCALL is always 3 bytes) */
	movq $0x681e, %rdx
	vmwrite %rax, %rdx	/* GUEST_RIP = next instruction */

	popq %rdx		/* restore guest rdx */
	popq %rax		/* restore guest rax */

	vmresume
*/
	

vmcall_fast_path:
	/*
	 * VMCALL fast path — guest uses native Composite IPC register layout.
	 * Save the pt_regs structure
	 */
	popq %rax          /* restore guest rax from initial push */
	subq $0x30, %rsp;	
	pushq %rax;		
	pushq %rbp;		
	pushq %rdi;		
	pushq %rsi;		
	pushq %rdx;		
	pushq %rcx;		
	pushq %rbx;		
	pushq %r8;		
	pushq %r9;		
	pushq %r10;		
	pushq %r11;		
	pushq %r12;		
	pushq %r13;		
	pushq %r14;		
	pushq %r15;		
	subq $0x20, %rsp;
	movq %rsp, %rdi /* pt_reg */

	movq $0x681e, %rax	/* GUEST_RIP encoding */
	vmread %rax, %rax	/* rax = current guest RIP */
	addq $3, %rax		/* rax = RIP of next instruction (VMCALL is always 3 bytes) */
	movq $0x681e, %rdx
	vmwrite %rax, %rdx	/* GUEST_RIP = next instruction */

	/*
	 * Benchmark-only MSR swap: mirrors the save-guest / restore-host /
	 * restore-guest sequence vmx_vmcall_fast_handler + vmx_ipc_resume
	 * do in C, done inline here to isolate the raw MSR instruction
	 * cost from the C-call / capability-lookup overhead. host_msr_cache
	 * is expected to already be populated (vmx_exit_handler fills it
	 * in on the first generic/slow-path exit).
	 */

	/* IA32_GS_BASE (0xC0000101) */
	movl $0xC0000101, %ecx
	rdmsr
	movl %eax, vmcall_asm_guest_msr_scratch+0(%rip)
	movl %edx, vmcall_asm_guest_msr_scratch+4(%rip)
	movl host_msr_cache+0(%rip), %eax
	movl host_msr_cache+4(%rip), %edx
	wrmsr

	/* IA32_KERNEL_GSBASE (0xC0000102) */
	movl $0xC0000102, %ecx
	rdmsr
	movl %eax, vmcall_asm_guest_msr_scratch+8(%rip)
	movl %edx, vmcall_asm_guest_msr_scratch+12(%rip)
	movl host_msr_cache+8(%rip), %eax
	movl host_msr_cache+12(%rip), %edx
	wrmsr

	/* IA32_TSC_AUX (0xC0000103) — guest value not saved, read disabled below
	movl $0xC0000103, %ecx
	rdmsr
	movl %eax, vmcall_asm_guest_msr_scratch+16(%rip)
	movl %edx, vmcall_asm_guest_msr_scratch+20(%rip)
	 */
	movl $0xC0000103, %ecx
	movl host_msr_cache+16(%rip), %eax
	movl host_msr_cache+20(%rip), %edx
	wrmsr

	/* IA32_STAR (0xC0000081) — guest value not saved, read disabled below
	movl $0xC0000081, %ecx
	rdmsr
	movl %eax, vmcall_asm_guest_msr_scratch+24(%rip)
	movl %edx, vmcall_asm_guest_msr_scratch+28(%rip)
	 */
	movl $0xC0000081, %ecx
	movl host_msr_cache+24(%rip), %eax
	movl host_msr_cache+28(%rip), %edx
	wrmsr

	/* IA32_LSTAR (0xC0000082) — guest value not saved, read disabled below
	movl $0xC0000082, %ecx
	rdmsr
	movl %eax, vmcall_asm_guest_msr_scratch+32(%rip)
	movl %edx, vmcall_asm_guest_msr_scratch+36(%rip)
	 */
	movl $0xC0000082, %ecx
	movl host_msr_cache+32(%rip), %eax
	movl host_msr_cache+36(%rip), %edx
	wrmsr

	/* IA32_CSTAR (0xC0000083) — guest value not saved, read disabled below
	movl $0xC0000083, %ecx
	rdmsr
	movl %eax, vmcall_asm_guest_msr_scratch+40(%rip)
	movl %edx, vmcall_asm_guest_msr_scratch+44(%rip)
	 */
	movl $0xC0000083, %ecx
	movl host_msr_cache+40(%rip), %eax
	movl host_msr_cache+44(%rip), %edx
	wrmsr

	/* IA32_FMASK (0xC0000084) — guest value not saved, read disabled below
	movl $0xC0000084, %ecx
	rdmsr
	movl %eax, vmcall_asm_guest_msr_scratch+48(%rip)
	movl %edx, vmcall_asm_guest_msr_scratch+52(%rip)
	 */
	movl $0xC0000084, %ecx
	movl host_msr_cache+48(%rip), %eax
	movl host_msr_cache+52(%rip), %edx
	wrmsr

	/*
	 * No real host-side work happens on this benchmark path, so
	 * restore the guest values we just saved before vmresume.
	 */

	/* IA32_GS_BASE */
	movl vmcall_asm_guest_msr_scratch+0(%rip), %eax
	movl vmcall_asm_guest_msr_scratch+4(%rip), %edx
	movl $0xC0000101, %ecx
	wrmsr

	/* IA32_KERNEL_GSBASE */
	movl vmcall_asm_guest_msr_scratch+8(%rip), %eax
	movl vmcall_asm_guest_msr_scratch+12(%rip), %edx
	movl $0xC0000102, %ecx
	wrmsr

	/* IA32_TSC_AUX */
	movl vmcall_asm_guest_msr_scratch+16(%rip), %eax
	movl vmcall_asm_guest_msr_scratch+20(%rip), %edx
	movl $0xC0000103, %ecx
	wrmsr

	/* IA32_STAR */
	movl vmcall_asm_guest_msr_scratch+24(%rip), %eax
	movl vmcall_asm_guest_msr_scratch+28(%rip), %edx
	movl $0xC0000081, %ecx
	wrmsr

	/* IA32_LSTAR */
	movl vmcall_asm_guest_msr_scratch+32(%rip), %eax
	movl vmcall_asm_guest_msr_scratch+36(%rip), %edx
	movl $0xC0000082, %ecx
	wrmsr

	/* IA32_CSTAR */
	movl vmcall_asm_guest_msr_scratch+40(%rip), %eax
	movl vmcall_asm_guest_msr_scratch+44(%rip), %edx
	movl $0xC0000083, %ecx
	wrmsr

	/* IA32_FMASK */
	movl vmcall_asm_guest_msr_scratch+48(%rip), %eax
	movl vmcall_asm_guest_msr_scratch+52(%rip), %edx
	movl $0xC0000084, %ecx
	wrmsr

	/* call vmx_vmcall_fast_handler*/
	/* Capabiliy check is insuccessfull.*/

	addq $0x20, %rsp;
	popq %r15;		
	popq %r14;		
	popq %r13;		
	popq %r12;		
	popq %r11;		
	popq %r10;		
	popq %r9;		
	popq %r8;		
	popq %rbx;		
	popq %rcx;		
	popq %rdx;		
	popq %rsi;		
	popq %rdi;		
	popq %rbp;		
	popq %rax;		
	addq $0x30, %rsp;
	vmresume
	
vmx_slow_path:
	/* --- Slow path: full GP save for generic exits --- */
	popq %rax
	pushq %rax
	pushq %rbp
	pushq %rdi
	pushq %rsi
	pushq %rdx
	pushq %rcx
	pushq %rbx
	pushq %r8
	pushq %r9
	pushq %r10
	pushq %r11
	pushq %r12
	pushq %r13
	pushq %r14
	pushq %r15
	movq %cr2, %rax
	pushq %rax
	movq %rsp, %rdi

	/* IA32_TSC_AUX (0xC0000103)	 */
	movl $0xC0000103, %ecx
	rdmsr
	movl %eax, vmcall_asm_guest_msr_scratch+16(%rip)
	movl %edx, vmcall_asm_guest_msr_scratch+20(%rip)

	/* IA32_STAR (0xC0000081)  */
	movl $0xC0000081, %ecx
	rdmsr
	movl %eax, vmcall_asm_guest_msr_scratch+24(%rip)
	movl %edx, vmcall_asm_guest_msr_scratch+28(%rip)

	/* IA32_LSTAR (0xC0000082)  */
	movl $0xC0000082, %ecx
	rdmsr
	movl %eax, vmcall_asm_guest_msr_scratch+32(%rip)
	movl %edx, vmcall_asm_guest_msr_scratch+36(%rip)

	/* IA32_CSTAR (0xC0000083) */
	movl $0xC0000083, %ecx
	rdmsr
	movl %eax, vmcall_asm_guest_msr_scratch+40(%rip)
	movl %edx, vmcall_asm_guest_msr_scratch+44(%rip)
	
	/* IA32_FMASK (0xC0000084)  */
	movl $0xC0000084, %ecx
	rdmsr
	movl %eax, vmcall_asm_guest_msr_scratch+48(%rip)
	movl %edx, vmcall_asm_guest_msr_scratch+52(%rip)
	
	call vmx_exit_handler