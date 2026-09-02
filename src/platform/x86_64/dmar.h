#ifndef DMAR_H
#define DMAR_H

#include "shared/cos_types.h"

/*
 * Fail fast rather than silently truncating: a machine with more
 * remapping units or reserved regions than we planned for is a
 * configuration we have not reasoned about.
 */
#define DMAR_UNIT_MAX 8
#define DMAR_RMRR_MAX 16
#define DMAR_SCOPE_MAX 16
/*
 * Context tables are one page per PCI bus.  Allocating all 256 per unit
 * would cost a megabyte of BSS each, so only buses that actually carry
 * devices get one.  A device on a bus without a context table is
 * blocked by the hardware, which shows up as a recorded fault rather
 * than silence -- see dmar_translation_enable().
 */
#define DMAR_CTXT_MAX 4

/* A DRHD: one hardware remapping unit and the register set behind it. */
struct dmar_unit {
	paddr_t reg_base;    /* physical base of this unit's register set */
	void   *regs;        /* mapped virtual address; NULL until mapped */
	u16_t   segment;     /* PCI segment this unit covers              */
	u8_t    include_all; /* unit covers every device not claimed      */
	/*
	 * Devices explicitly named in this unit's scope.  Not every
	 * DRHD sets include_all -- QEMU's does not -- so scope is how a
	 * device is routed to the unit that must be told to invalidate
	 * on its behalf.  Sending an invalidation to the wrong unit is
	 * silently wrong, so routing is not optional even with one unit.
	 */
	u16_t   scope_bdf[DMAR_SCOPE_MAX];
	u8_t    scope_cnt;

	/* Decoded from the capability registers once regs is mapped. */
	u8_t    ver_major;
	u8_t    ver_minor;
	u32_t   num_domains;    /* domain-id space of this unit          */
	u8_t    max_addr_width; /* MGAW: widest address the unit accepts */
	u8_t    sagaw;          /* bitmap of supported page-table depths */
	u8_t    qi_supported;   /* queued invalidation                   */
	u8_t    coherent;       /* hardware page walks are cache-coherent */

	/*
	 * The global command register is write-only in effect: writing it
	 * issues whatever commands its set bits name, so the other enables
	 * must be preserved.  Shadow what we have asked for.
	 */
	u32_t   gcmd_shadow;

	/* Queued invalidation state; queue is one page of descriptors. */
	void   *iq;      /* invalidation queue, page-aligned */
	u32_t   iq_tail; /* next free descriptor index       */
	u8_t    qi_on;

	/* Root table, and the per-bus context tables it points at. */
	void   *root;
	u8_t    ctxt_bus[DMAR_CTXT_MAX];
	u8_t    ctxt_cnt;
	u8_t    translating;
};

/*
 * An RMRR: a region firmware requires be identity-mapped for the
 * devices in its scope.  Phase 1 refuses to bind such devices rather
 * than honouring the region, so this is recorded to say no with.
 */
struct dmar_rmrr {
	u16_t   segment;
	paddr_t base;
	paddr_t limit;
};

/* Cache maintenance for remapping structures; no-op on a coherent unit. */
void              dmar_flush_cache(struct dmar_unit *u, void *addr, unsigned long sz);
int               dmar_qi_init(struct dmar_unit *u);
int               dmar_inv_context_global(struct dmar_unit *u);
int               dmar_inv_iotlb_global(struct dmar_unit *u);

int               dmar_translation_enable(struct dmar_unit *u);

void              dmar_init(void);
unsigned          dmar_unit_count(void);
struct dmar_unit *dmar_unit_get(unsigned i);
/* The unit responsible for a device: scope match first, then include_all. */
struct dmar_unit *dmar_unit_for_bdf(u16_t bdf);
int               dmar_rmrr_covers(u16_t bdf);
/* Non-zero when every unit supports what the design requires. */
int               dmar_hw_usable(void);

#endif /* DMAR_H */
