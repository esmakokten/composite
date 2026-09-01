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

void              dmar_init(void);
unsigned          dmar_unit_count(void);
struct dmar_unit *dmar_unit_get(unsigned i);
/* The unit responsible for a device: scope match first, then include_all. */
struct dmar_unit *dmar_unit_for_bdf(u16_t bdf);
int               dmar_rmrr_covers(u16_t bdf);

#endif /* DMAR_H */
