/*
 * Parsing of the ACPI DMAR table: the description of the machine's
 * VT-d remapping hardware.
 *
 * This file interprets one table and, later, owns the registers behind
 * it.  Keeping all DMAR register access here makes the design decision
 * that the kernel -- not a user-level component -- owns those registers
 * visible in the source layout: a component that could write the root
 * table address register would choose which translations the hardware
 * consults, which is the whole authority this subsystem confines.
 *
 * Structure layouts follow "Intel Virtualization Technology for
 * Directed I/O, Architecture Specification".  The offsets are gathered
 * as named constants below so a reviewer can check them against the
 * spec in one place rather than hunting through the walk.
 */

#include "kernel.h"
#include "string.h"
#include "dmar.h"

/* DMAR table header, after the 36-byte ACPI system description header. */
#define DMAR_OFF_HOST_ADDR_WIDTH 36 /* u8: host address width, minus one */
#define DMAR_OFF_FLAGS           37 /* u8                                */
#define DMAR_OFF_STRUCTS         48 /* first remapping structure         */

/* Remapping structure types. */
#define DMAR_TYPE_DRHD 0
#define DMAR_TYPE_RMRR 1

/* Common remapping structure header. */
#define REMAP_OFF_TYPE 0 /* u16 */
#define REMAP_OFF_LEN  2 /* u16 */

/* DRHD structure. */
#define DRHD_OFF_FLAGS    4  /* u8, bit 0 = INCLUDE_PCI_ALL */
#define DRHD_OFF_SEGMENT  6  /* u16                         */
#define DRHD_OFF_REGBASE  8  /* u64                         */
#define DRHD_OFF_SCOPE    16 /* device scope entries        */
#define DRHD_FLAG_INCLUDE_PCI_ALL 0x1

/* RMRR structure. */
#define RMRR_OFF_SEGMENT 6  /* u16 */
#define RMRR_OFF_BASE    8  /* u64 */
#define RMRR_OFF_LIMIT   16 /* u64 */
#define RMRR_OFF_SCOPE   24 /* device scope entries */

/*
 * Remapping unit register offsets, and the capability fields we decode.
 * CAP and ECAP are 64-bit; VER is 32-bit.
 */
#define DMAR_REG_VER  0x00
#define DMAR_REG_CAP  0x08
#define DMAR_REG_ECAP 0x10

#define CAP_ND(c)    ((u32_t)((c) & 0x7))          /* domains = 1 << (4 + 2*ND) */
#define CAP_SAGAW(c) ((u8_t)(((c) >> 8) & 0x1f))   /* supported page-table depths */
#define CAP_MGAW(c)  ((u8_t)(((c) >> 16) & 0x3f))  /* max guest address width - 1 */

#define ECAP_C(e)  ((u8_t)((e) & 0x1))         /* page-walk coherency */
#define ECAP_QI(e) ((u8_t)(((e) >> 1) & 0x1))  /* queued invalidation */

/* Global command / status, and the invalidation queue registers. */
#define DMAR_REG_GCMD 0x18
#define DMAR_REG_GSTS 0x1c
#define DMAR_REG_IQH  0x80
#define DMAR_REG_IQT  0x88
#define DMAR_REG_IQA  0x90

#define GCMD_QIE (1UL << 26) /* queued invalidation enable */
#define GSTS_QIES (1UL << 26)

/*
 * Invalidation descriptors are 16 bytes.  A one-page queue holds 256 of
 * them, which is the smallest size the queue address register encodes
 * (QS == 0) and far more than boot-time invalidation needs.
 */
#define IQ_DESC_SZ    16
#define IQ_DESC_COUNT (PAGE_SIZE / IQ_DESC_SZ)

#define IQ_TYPE_CC_INV (0x1) /* context cache invalidate */
#define IQ_TYPE_IOTLB  (0x2) /* IOTLB invalidate         */
#define IQ_TYPE_WAIT   (0x5) /* invalidation wait        */

#define IQ_GRAN_GLOBAL (0x1UL << 4)
#define IQ_IOTLB_DW    (1UL << 6) /* drain writes */
#define IQ_IOTLB_DR    (1UL << 7) /* drain reads  */
#define IQ_WAIT_SW     (1UL << 5) /* write status on completion */
#define IQ_WAIT_FN     (1UL << 6) /* fence: order against prior descriptors */

/*
 * Every poll of a hardware status bit is bounded.  A wrong register
 * offset or bit position must surface as a reported failure at boot,
 * not as a machine that stops responding -- these constants are taken
 * from a specification and the code has to survive getting one wrong.
 */
#define DMAR_POLL_MAX 1000000

/* Device scope entry. */

/* Device scope entry. */
#define SCOPE_OFF_TYPE      0 /* u8  */
#define SCOPE_OFF_LEN       1 /* u8  */
#define SCOPE_OFF_START_BUS 5 /* u8  */
#define SCOPE_OFF_PATH      6 /* pairs of (device, function) */
#define SCOPE_HDR_LEN       6

/*
 * Remapping structures live in kernel BSS.  There is no allocator this
 * early in boot, and the hardware needs a stable physical address for
 * each, which chal_va2pa gives us for kernel data.  Bounded statically
 * for the same reason the unit and RMRR arrays are.
 */
static u8_t iq_pages[DMAR_UNIT_MAX][PAGE_SIZE] __attribute__((aligned(PAGE_SIZE)));

/* Status word an invalidation-wait descriptor writes on completion. */
static volatile u32_t iq_wait_status[DMAR_UNIT_MAX] __attribute__((aligned(CACHE_LINE)));

static struct dmar_unit units[DMAR_UNIT_MAX];
static unsigned         unit_cnt;
static struct dmar_rmrr rmrrs[DMAR_RMRR_MAX];
static unsigned         rmrr_cnt;

/*
 * The device scopes of every RMRR, flattened into a list of BDFs.  We
 * only ever ask "is this device covered", so the association back to a
 * particular region is not worth keeping.
 */
#define DMAR_RMRR_BDF_MAX 32
static u16_t rmrr_bdfs[DMAR_RMRR_BDF_MAX];
static unsigned rmrr_bdf_cnt;

static u32_t
dmar_read32(struct dmar_unit *u, unsigned off)
{
	return *(volatile u32_t *)((char *)u->regs + off);
}

static void
dmar_write32(struct dmar_unit *u, unsigned off, u32_t v)
{
	*(volatile u32_t *)((char *)u->regs + off) = v;
}

static void
dmar_write64(struct dmar_unit *u, unsigned off, u64_t v)
{
	*(volatile u64_t *)((char *)u->regs + off) = v;
}

/*
 * Issue one global command and wait for the matching status bit.  The
 * command register carries every enable at once, so it is written from
 * a shadow rather than read back.
 */
static int
dmar_gcmd_set(struct dmar_unit *u, u32_t cmd_bit, u32_t sts_bit, int set)
{
	unsigned long n = 0;

	if (set) u->gcmd_shadow |= cmd_bit;
	else     u->gcmd_shadow &= ~cmd_bit;

	dmar_write32(u, DMAR_REG_GCMD, u->gcmd_shadow);

	while (n++ < DMAR_POLL_MAX) {
		u32_t sts = dmar_read32(u, DMAR_REG_GSTS);

		if (set  &&  (sts & sts_bit)) return 0;
		if (!set && !(sts & sts_bit)) return 0;
	}

	return -1;
}

void
dmar_flush_cache(struct dmar_unit *u, void *addr, unsigned long sz)
{
	char *p;

	/* A coherent unit reads what the CPU wrote; nothing to do. */
	if (u->coherent) return;

	for (p = (char *)addr; p < (char *)addr + sz; p += CACHE_LINE) {
		__asm__ volatile("clflush (%0)" : : "r"(p) : "memory");
	}
	__asm__ volatile("sfence" ::: "memory");
}

int
dmar_qi_init(struct dmar_unit *u)
{
	unsigned idx = (unsigned)(u - units);

	if (!u->qi_supported) return -1;

	u->iq      = &iq_pages[idx][0];
	u->iq_tail = 0;
	memset(u->iq, 0, PAGE_SIZE);
	dmar_flush_cache(u, u->iq, PAGE_SIZE);

	/* Queue address, with size field 0 meaning 256 descriptors. */
	dmar_write64(u, DMAR_REG_IQA, (u64_t)chal_va2pa(u->iq));
	dmar_write32(u, DMAR_REG_IQT, 0);

	if (dmar_gcmd_set(u, GCMD_QIE, GSTS_QIES, 1)) {
		printk("DMAR: unit %u timed out enabling queued invalidation\n", idx);
		return -1;
	}
	u->qi_on = 1;
	printk("DMAR: unit %u queued invalidation enabled\n", idx);

	return 0;
}

/*
 * Submit one descriptor followed by an invalidation-wait descriptor, and
 * spin until the wait's status write lands.  The spin is bounded: a
 * wedged queue is a reported failure, never a hung machine.
 */
static int
dmar_qi_submit_wait(struct dmar_unit *u, u64_t d0, u64_t d1)
{
	unsigned idx = (unsigned)(u - units);
	u64_t   *q   = (u64_t *)u->iq;
	unsigned long n = 0;
	u32_t    slot, wslot;

	if (!u->qi_on) return -1;

	slot  = u->iq_tail;
	wslot = (slot + 1) % IQ_DESC_COUNT;

	q[slot * 2]     = d0;
	q[slot * 2 + 1] = d1;

	iq_wait_status[idx] = 0;
	/* Fence so the wait cannot complete before the work it follows. */
	q[wslot * 2]     = IQ_TYPE_WAIT | IQ_WAIT_SW | IQ_WAIT_FN | (1ULL << 32);
	q[wslot * 2 + 1] = (u64_t)chal_va2pa((void *)&iq_wait_status[idx]);

	dmar_flush_cache(u, &q[slot * 2], 2 * IQ_DESC_SZ);

	u->iq_tail = (wslot + 1) % IQ_DESC_COUNT;
	dmar_write32(u, DMAR_REG_IQT, u->iq_tail * IQ_DESC_SZ);

	while (n++ < DMAR_POLL_MAX) {
		if (iq_wait_status[idx] == 1) return 0;
	}

	printk("DMAR: unit %u invalidation queue timed out\n", idx);

	return -1;
}

int
dmar_inv_context_global(struct dmar_unit *u)
{
	return dmar_qi_submit_wait(u, IQ_TYPE_CC_INV | IQ_GRAN_GLOBAL, 0);
}

int
dmar_inv_iotlb_global(struct dmar_unit *u)
{
	return dmar_qi_submit_wait(u, IQ_TYPE_IOTLB | IQ_GRAN_GLOBAL | IQ_IOTLB_DW | IQ_IOTLB_DR, 0);
}

static u64_t
dmar_read64(struct dmar_unit *u, unsigned off)
{
	return *(volatile u64_t *)((char *)u->regs + off);
}

/* Unaligned little-endian reads: ACPI tables are packed, not aligned. */
static u16_t
rd16(const u8_t *p)
{
	return (u16_t)p[0] | ((u16_t)p[1] << 8);
}

static u64_t
rd64(const u8_t *p)
{
	u64_t v = 0;
	int   i;

	for (i = 7; i >= 0; i--) v = (v << 8) | p[i];

	return v;
}

/*
 * Walk a structure's device scope entries, appending the BDF of each
 * into out[].  A scope names a device by its start bus plus a path of
 * (device, function) pairs walked through bridges; the last pair is
 * the device itself, which is the only part we need.
 */
static void
dmar_scope_walk(const u8_t *p, const u8_t *end, u16_t *out, unsigned *cnt, unsigned max)
{
	while (p + SCOPE_HDR_LEN <= end) {
		u8_t len = p[SCOPE_OFF_LEN];
		u8_t bus = p[SCOPE_OFF_START_BUS];
		int  npath;

		if (len < SCOPE_HDR_LEN || p + len > end) break;

		npath = (len - SCOPE_HDR_LEN) / 2;
		if (npath > 0) {
			const u8_t *last = p + SCOPE_OFF_PATH + (npath - 1) * 2;
			u16_t       bdf  = ((u16_t)bus << 8) | ((last[0] & 0x1f) << 3) | (last[1] & 0x7);

			if (*cnt >= max) {
				printk("DMAR: device scope larger than the fixed bound\n");
				assert(0);
			}
			out[(*cnt)++] = bdf;
		}
		p += len;
	}
}

void
dmar_init(void)
{
	const u8_t *tbl = acpi_find_dmar();
	const u8_t *p, *end;
	u32_t       tbl_len;
	u8_t        host_addr_width;
	unsigned    i;

	if (!tbl) {
		printk("DMAR: no table found; IOMMU unavailable\n");
		return;
	}

	/* Table length lives at offset 4 of the ACPI header. */
	tbl_len         = (u32_t)tbl[4] | ((u32_t)tbl[5] << 8) | ((u32_t)tbl[6] << 16) | ((u32_t)tbl[7] << 24);
	host_addr_width = tbl[DMAR_OFF_HOST_ADDR_WIDTH] + 1;

	p   = tbl + DMAR_OFF_STRUCTS;
	end = tbl + tbl_len;


	while (p + 4 <= end) {
		u16_t type = rd16(p + REMAP_OFF_TYPE);
		u16_t len  = rd16(p + REMAP_OFF_LEN);

		/* A zero or overlong length would loop or run off the table. */
		if (len < 4 || p + len > end) {
			printk("DMAR: malformed remapping structure; stopping parse\n");
			break;
		}

		switch (type) {
		case DMAR_TYPE_DRHD: {
			struct dmar_unit *u;

			if (unit_cnt >= DMAR_UNIT_MAX) {
				printk("DMAR: more units than DMAR_UNIT_MAX\n");
				assert(0);
			}
			u              = &units[unit_cnt++];
			u->reg_base    = (paddr_t)rd64(p + DRHD_OFF_REGBASE);
			u->regs        = NULL;
			u->segment     = rd16(p + DRHD_OFF_SEGMENT);
			u->include_all = !!(p[DRHD_OFF_FLAGS] & DRHD_FLAG_INCLUDE_PCI_ALL);
			u->scope_cnt   = 0;
			{
				unsigned n = 0;

				dmar_scope_walk(p + DRHD_OFF_SCOPE, p + len, u->scope_bdf, &n, DMAR_SCOPE_MAX);
				u->scope_cnt = (u8_t)n;
			}
			break;
		}
		case DMAR_TYPE_RMRR: {
			struct dmar_rmrr *r;

			if (rmrr_cnt >= DMAR_RMRR_MAX) {
				printk("DMAR: more RMRRs than DMAR_RMRR_MAX\n");
				assert(0);
			}
			r          = &rmrrs[rmrr_cnt++];
			r->segment = rd16(p + RMRR_OFF_SEGMENT);
			r->base    = (paddr_t)rd64(p + RMRR_OFF_BASE);
			r->limit   = (paddr_t)rd64(p + RMRR_OFF_LIMIT);
			dmar_scope_walk(p + RMRR_OFF_SCOPE, p + len, rmrr_bdfs, &rmrr_bdf_cnt,
			                DMAR_RMRR_BDF_MAX);
			break;
		}
		default:
			/* Firmware may report structures we do not handle. */
			break;
		}

		p += len;
	}

	/*
	 * Map each unit's registers and decode what the hardware can do.
	 * PGTBL_NOCACHE because these are device registers, matching how
	 * acpi_find_apic maps the LAPIC.
	 */
	for (i = 0; i < unit_cnt; i++) {
		u64_t cap, ecap;
		u32_t ver;

		units[i].regs = device_map_mem(units[i].reg_base, PGTBL_NOCACHE);
		assert(units[i].regs);

		ver  = dmar_read32(&units[i], DMAR_REG_VER);
		cap  = dmar_read64(&units[i], DMAR_REG_CAP);
		ecap = dmar_read64(&units[i], DMAR_REG_ECAP);

		units[i].ver_major      = (ver >> 4) & 0xf;
		units[i].ver_minor      = ver & 0xf;
		units[i].num_domains    = 1U << (4 + 2 * CAP_ND(cap));
		units[i].max_addr_width = CAP_MGAW(cap) + 1;
		units[i].sagaw          = CAP_SAGAW(cap);
		units[i].qi_supported   = ECAP_QI(ecap);
		units[i].coherent       = ECAP_C(ecap);

		/* Raw values alongside the decode, so the decode is checkable. */
		printk("DMAR: unit %u raw ver 0x%lx cap 0x%lx ecap 0x%lx\n", i, (unsigned long)ver,
		       (unsigned long)cap, (unsigned long)ecap);
	}

	printk("DMAR: %u remapping unit(s), host address width %u\n", unit_cnt, host_addr_width);
	for (i = 0; i < unit_cnt; i++) {
		unsigned j;

		printk("DMAR: unit %u regs @ 0x%lx segment %u%s, %u scoped device(s)\n", i,
		       (unsigned long)units[i].reg_base, units[i].segment,
		       units[i].include_all ? " include-all" : "", units[i].scope_cnt);
		for (j = 0; j < units[i].scope_cnt; j++) {
			printk("DMAR:   scope %02x:%02x.%u\n", units[i].scope_bdf[j] >> 8,
			       (units[i].scope_bdf[j] >> 3) & 0x1f, units[i].scope_bdf[j] & 0x7);
		}
		printk("DMAR: unit %u version %u.%u domains %u addr-width %u sagaw 0x%x qi %s coherent %s\n",
		       i, units[i].ver_major, units[i].ver_minor, units[i].num_domains,
		       units[i].max_addr_width, units[i].sagaw,
		       units[i].qi_supported ? "yes" : "no", units[i].coherent ? "yes" : "no");
	}

	if (!dmar_hw_usable()) {
		printk("DMAR: hardware unusable for the current design; IOMMU disabled\n");
		return;
	}

	for (i = 0; i < unit_cnt; i++) {
		if (dmar_qi_init(&units[i])) continue;
		if (dmar_inv_context_global(&units[i]) || dmar_inv_iotlb_global(&units[i])) {
			printk("DMAR: unit %u global invalidation FAILED\n", i);
			continue;
		}
		printk("DMAR: unit %u global context+iotlb invalidation ok\n", i);
	}
	for (i = 0; i < rmrr_cnt; i++) {
		printk("DMAR: rmrr [0x%lx, 0x%lx] segment %u\n", (unsigned long)rmrrs[i].base,
		       (unsigned long)rmrrs[i].limit, rmrrs[i].segment);
	}
}

unsigned
dmar_unit_count(void)
{
	return unit_cnt;
}

struct dmar_unit *
dmar_unit_get(unsigned i)
{
	if (i >= unit_cnt) return NULL;

	return &units[i];
}

/*
 * The lazy invalidation design pays its cost at retype by submitting an
 * invalidation and waiting for it, which requires a queue.  A unit
 * without queued invalidation needs a different design, so say so
 * loudly at boot rather than failing mysteriously later.
 */
int
dmar_hw_usable(void)
{
	unsigned i;

	if (unit_cnt == 0) return 0;
	for (i = 0; i < unit_cnt; i++) {
		if (!units[i].qi_supported) {
			printk("DMAR: unit %u lacks queued invalidation\n", i);
			return 0;
		}
	}

	return 1;
}

struct dmar_unit *
dmar_unit_for_bdf(u16_t bdf)
{
	unsigned i, j;
	struct dmar_unit *catchall = NULL;

	/* An explicit scope match wins over a catch-all unit. */
	for (i = 0; i < unit_cnt; i++) {
		for (j = 0; j < units[i].scope_cnt; j++) {
			if (units[i].scope_bdf[j] == bdf) return &units[i];
		}
		if (units[i].include_all) catchall = &units[i];
	}

	return catchall;
}

int
dmar_rmrr_covers(u16_t bdf)
{
	unsigned i;

	for (i = 0; i < rmrr_bdf_cnt; i++) {
		if (rmrr_bdfs[i] == bdf) return 1;
	}

	return 0;
}
