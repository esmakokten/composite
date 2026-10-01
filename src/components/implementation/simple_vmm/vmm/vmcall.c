#include <vmrt.h>

/*
 * VMCALL from the guest.
 *
 * Linux issues none on its own once it sees no hypervisor CPUID leaf, so
 * every VMCALL that arrives here comes from a program that meant to take a
 * VM exit and nothing else: the evaluation harness's empty-handler rung, and
 * its workload markers, which the kernel's `count` build flavour reads before
 * reflecting the exit here. The guest is advanced past the instruction and
 * resumed. vmrt's weak default panics the VM instead.
 */
void
vmcall_handler(struct vmrt_vm_vcpu *vcpu)
{
	GOTO_NEXT_INST(vcpu->shared_region);
}
