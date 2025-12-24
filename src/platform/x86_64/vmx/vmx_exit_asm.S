.section .text
.code64

.global vmx_exit_handler_asm
.global vmx_exit_for_measurement
.extern vmx_exit_handler
.extern vmx_exit_handler_measurement

vmx_exit_handler_asm:
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

vmx_exit_for_measurement:
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
	call vmx_exit_handler_measurement