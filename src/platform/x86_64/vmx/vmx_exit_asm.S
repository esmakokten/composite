.section .text
.code64

.global vmx_exit_handler_asm
.extern vmx_exit_handler
.extern vmx_vmcall_fast_handler

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
	/* Static branch predictor, Jump forward is undesired, loop mechanisms

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

	call vmx_vmcall_fast_handler
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
	call vmx_exit_handler

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

	call vmx_vmcall_fast_handler
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
	