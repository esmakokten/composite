#include "serial.h"
#include "../../vioapic.h"

/* The UART sits on ISA IRQ 4; with one IOAPIC, GSI 4 is its pin. */
#define SERIAL_GSI 4U

static u8_t serial_data[0x08];
static u8_t send_intr_enable = 0;

/*
 * The vector the guest assigned to the serial line.
 *
 * It is not a constant: the guest kernel allocates it when it programs the
 * IOAPIC redirection entry for GSI 4. A 5.15 guest happens to choose 34, a
 * 6.6 guest chooses 32, and injecting a fixed 34 into the latter gives it
 * "No irq handler for vector" and no console. Read what the guest wrote.
 *
 * Returns 0 when the entry is masked or not yet programmed: the guest has
 * no handler for the line, so there is nothing to inject.
 */
static u8_t
serial_vector(struct vmrt_vm_vcpu *vcpu)
{
	union ioapic_rte rte;

	vioapic_get_rte(vcpu->vm, SERIAL_GSI, &rte);
	if (rte.bits.intr_mask == IOAPIC_RTE_MASK_SET || rte.bits.vector < 0x20) return 0;

	return (u8_t)rte.bits.vector;
}

void
serial_handler(u16_t port, int dir, int sz, struct vmrt_vm_vcpu *vcpu)
{
	assert(port <= SERIAL_PORT_MAX && port >= SERIAL_PORT_MIN);
	assert(sz == IO_BYTE);

	if (dir == IO_IN) {
		vcpu->shared_region->ax = serial_data[port - SERIAL_PORT_MIN];
	} else if (dir == IO_OUT) {
		serial_data[port - SERIAL_PORT_MIN] = (u8_t)vcpu->shared_region->ax;
		switch (port)
		{

		case SERIAL_PORT1:
			printc("%c", (u8_t)vcpu->shared_region->ax);
			if (send_intr_enable) {
				u8_t vector = serial_vector(vcpu);

				/* TODO: correctly handle serial output (eg: multiple vcpu case) */
				if (vector) {
#if VMX_SUPPORT_POSTED_INTR
					vlapic_set_intr(vcpu, vector, 0);
#else
					lapic_intr_inject(vcpu, vector, 1);
#endif
				}
				serial_data[SERIAL_IIR - SERIAL_PORT_MIN] = 0x2;
				serial_data[MODEM_STATUS_REGISTER - SERIAL_PORT_MIN] = 0x20;
				serial_data[SERIAL_LSR - SERIAL_PORT_MIN] = 0x60;
			}
			break;
		case SERIAL_IER:			
			if (0x2 & (u8_t)vcpu->shared_region->ax) {
				send_intr_enable = 1;
			} else {
				send_intr_enable = 0;
			}
			break;
		}
	} else {
		VM_PANIC(vcpu);
	}

	return;
}

static void __attribute__((constructor))
init(void)
{
	memset(&serial_data, 0, sizeof(serial_data));
	serial_data[SERIAL_LSR - SERIAL_PORT_MIN] = 0x60;
}
