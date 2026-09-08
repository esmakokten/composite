#include <cc.h>
#include <pgtbl.h>
#include <thd.h>

#include "kernel.h"
#include "string.h"
#include "isr.h"
#include "chal_cpu.h"
#include "mem_layout.h"
#include "chal_pgtbl.h"

#define LARGE_BSS __attribute__((section(".largebss,\"aw\",@nobits#")))

struct tlb_quiescence tlb_quiescence[NUM_CPU] CACHE_ALIGNED LARGE_BSS;
struct liveness_entry __liveness_tbl[LTBL_ENTS] CACHE_ALIGNED LARGE_BSS;

#define KERN_INIT_PGD_IDX ((COS_MEM_KERN_START_VA & COS_MEM_KERN_HIGH_ADDR_VA_PGD_MASK) >> PGD_SHIFT)
u64_t boot_comp_pgd[PAGE_SIZE / sizeof(u64_t)] PAGE_ALIGNED = {0};

u64_t boot_comp_pgt1[PAGE_SIZE / sizeof(u64_t)] PAGE_ALIGNED = {0};

/*
 * SYSCALL trampoline: one page at COS_SYSCALL_TRAMP_VA, reached through its own
 * PUD/PMD/PT chain hanging off PGD index 509. The kernel proper is mapped with
 * 1G super-pages at the PUD level, which cannot express a single page, so this
 * VA gets a full four-level walk of its own.
 */
u64_t tramp_pud[PAGE_SIZE / sizeof(u64_t)] PAGE_ALIGNED = {0};
u64_t tramp_pmd[PAGE_SIZE / sizeof(u64_t)] PAGE_ALIGNED = {0};
u64_t tramp_pte[PAGE_SIZE / sizeof(u64_t)] PAGE_ALIGNED = {0};
u8_t  tramp_page[PAGE_SIZE] PAGE_ALIGNED = {0};

/*
 * Build the trampoline and map it.
 *
 * The jump must clobber no register: SYSCALL arrives with the user register
 * file live and RAX holding the syscall number, so a movabs into a register
 * would destroy it. A RIP-relative indirect jump through an address stored in
 * the same page touches nothing:
 *
 *	ff 25 00 00 00 00	jmp *0x0(%rip)
 *	<8 bytes>		absolute address of sysenter_entry
 *
 * The displacement is 0 because RIP already points at the address slot.
 *
 * The PTE is deliberately not writable. Two caveats, both hardware facts on
 * this part rather than choices:
 *   - execute-only would need PKS, and CPUID.(EAX=7,ECX=0):ECX[31] is 0 on the
 *     Xeon 8160, so R+X is the floor for a supervisor page here;
 *   - CR0.WP is never set by loader.S on x86_64, so supervisor writes bypass
 *     the read-only bit anyway. The bit is set for when that changes.
 */
static void
tramp_setup(void)
{
	u64_t target = (u64_t)sysenter_entry;

	tramp_page[0] = 0xff;		/* jmp *disp32(%rip) */
	tramp_page[1] = 0x25;
	tramp_page[2] = 0x00;
	tramp_page[3] = 0x00;
	tramp_page[4] = 0x00;
	tramp_page[5] = 0x00;
	*(u64_t *)&tramp_page[6] = target;

	tramp_pte[COS_TRAMP_PTE_IDX] = (u64_t)chal_va2pa(tramp_page)
	                             | X86_PGTBL_PRESENT | X86_PGTBL_GLOBAL;
	tramp_pmd[COS_TRAMP_PMD_IDX] = (u64_t)chal_va2pa(tramp_pte)
	                             | X86_PGTBL_PRESENT | X86_PGTBL_WRITABLE;
	tramp_pud[COS_TRAMP_PUD_IDX] = (u64_t)chal_va2pa(tramp_pmd)
	                             | X86_PGTBL_PRESENT | X86_PGTBL_WRITABLE;
	boot_comp_pgd[COS_TRAMP_PGD_IDX] = (u64_t)chal_va2pa(tramp_pud)
	                                 | X86_PGTBL_PRESENT | X86_PGTBL_WRITABLE;

	printk("\tSYSCALL trampoline at %p -> sysenter_entry %p\n",
	       (void *)COS_SYSCALL_TRAMP_VA, (void *)target);
}

u64_t boot_ap_pgd[PAGE_SIZE / sizeof(u64_t)] PAGE_ALIGNED = {[0] = 0 | X86_PGTBL_PRESENT | X86_PGTBL_WRITABLE | X86_PGTBL_SUPER,
                                                             [KERN_INIT_PGD_IDX] = 0 | X86_PGTBL_PRESENT | X86_PGTBL_WRITABLE
                                                                                     | X86_PGTBL_SUPER};


void
kern_retype_initial(void)
{
	u8_t *k;

	assert((unsigned long)mem_bootc_start() % RETYPE_MEM_NPAGES == 0);
	assert((unsigned long)mem_bootc_end() % RETYPE_MEM_NPAGES == 0);
	for (k = mem_bootc_start(); k < mem_bootc_end(); k += PAGE_SIZE * RETYPE_MEM_NPAGES) {
		if (retypetbl_retype2user((void *)chal_va2pa(k), PAGE_ORDER)) assert(0);
	}
}

u8_t *
mem_boot_alloc(int npages) /* boot-time, bump-ptr heap */
{
	u8_t *        r = glb_memlayout.kern_boot_heap;
	unsigned long i;

	assert(glb_memlayout.allocs_avail);

	glb_memlayout.kern_boot_heap += npages * (PAGE_SIZE / sizeof(u8_t));
	assert(glb_memlayout.kern_boot_heap <= mem_kmem_end());
	for (i = (unsigned long)r; i < (unsigned long)glb_memlayout.kern_boot_heap; i += PAGE_SIZE) {
		if ((unsigned long)i % RETYPE_MEM_NPAGES == 0) {
			if (retypetbl_retype2kern((void *)chal_va2pa((void *)i), PAGE_ORDER)) {
			}
		}
	}

	memset((void *)r, 0, npages * (PAGE_SIZE / sizeof(u8_t)));

	return r;
}

/*
 * Essentially the kernel's heap pointer. Used for device memory
 * allocation AFTER memory has been allocated for typeable memory.
 */
unsigned long kernel_mapped_offset;

int
kern_setup_image(void)
{
	u64_t i, j;
	paddr_t       kern_pa_start, kern_pa_end;
	int cpu_id = get_cpuid();

	printk("\tSetting up initial page directory.\n");
	kern_pa_start = round_to_pgd_page(chal_va2pa(mem_kern_start())); /* likely 0 */
	kern_pa_end   = chal_va2pa(mem_kmem_end());
	/* ASSUMPTION: The static layout of boot_comp_pgd is identical to a pgd post-pgtbl_alloc */
	/* FIXME: should use pgtbl_extend instead of directly accessing the pgd array... */
	for (i = kern_pa_start; i < (unsigned long)round_up_to_pgt1_page(kern_pa_end); i += PGT1_RANGE, j++) {
		boot_comp_pgt1[i / PGT1_RANGE] = i | X86_PGTBL_PRESENT | X86_PGTBL_WRITABLE | X86_PGTBL_SUPER | X86_PGTBL_GLOBAL;
	}
	boot_comp_pgd[0] = 0; /* unmap lower addresses */

	kernel_mapped_offset = i / PGT1_RANGE;

	tramp_setup();

	chal_cpu_init();
	chal_cpu_pgtbl_activate((pgtbl_t)chal_va2pa(boot_comp_pgd));
	kern_retype_initial();

	return 0;
}

/***
 * The device API for allocating virtual memory, and accessing the
 * device. Functions to map the memory, and to translate to virtual
 * addresses. As devices are often in high physical memory, these are
 * not the typical implementations, and require a simply
 * data-structure to track the mappings. We want to avoid memory
 * allocation (e.g. for PGD nodes) here, so we map super-pages worth
 * of memory. This uses a non-trivial amount of kernel virtual memory.
 *
 * Note that this is relevant for devices that the kernel needs to
 * access (timers, ACPI, LAPIC, etc...), and NOT the devices that are
 * accessed from user-level as the hardware capability provides that
 * mappings in a more conventional way.
 *
 * Thus, this is a simple implementation that assumes that we have
 * relatively few devices that require mapping. We also bound the
 * number of regions devoted to devices so that we fail fast if
 * something strange is configured.
 */

#define DEV_MAPS_MAX 16

int dev_map_off = 0;
struct dev_map {
	paddr_t physaddr;
	void   *virtaddr;
} dev_mem[DEV_MAPS_MAX];

void *
device_pa2va(paddr_t dev_addr)
{
	int i;

	for (i = 0; i < dev_map_off; i++) {
		paddr_t rounded = round_to_pgt1_page(dev_addr);
		if (round_to_pgt1_page(dev_mem[i].physaddr) == rounded) {
			return (char *)dev_mem[i].virtaddr + (dev_addr - rounded);
		}
	}

	return NULL;
}

/*
 * For a device mapped at a physical address, map it into virtual
 * memory and return the address. This is very wasteful of physical
 * memory as it uses PGD ranges (4MB) for the allocations. For 64-bit,
 * a PGT3 is used, ranges a 1GB for allocations, but that should be OK
 * since we have already mapped all physical memory address before this
 * step, the remaining slots in page tables are free to use.
 */
void *
device_map_mem(paddr_t dev_addr, unsigned int pt_extra_flags)
{
	paddr_t rounded;
	void   *vaddr;
	unsigned long off = kernel_mapped_offset;

	boot_state_assert(INIT_UT_MEM);
	vaddr = device_pa2va(dev_addr);
	if (vaddr) {
		boot_comp_pgt1[((unsigned long)vaddr - (unsigned long)(COS_MEM_KERN_START_VA)) / PGT1_RANGE] |= pt_extra_flags; /* use the union of the flags */

		return vaddr;
	}

	/* Allocate a PGD/PGT3 region, and map it in */
	assert(off < PAGE_SIZE / sizeof(unsigned long));
	rounded = round_to_pgt1_page(dev_addr);
	boot_comp_pgt1[off] = rounded | X86_PGTBL_PRESENT | X86_PGTBL_WRITABLE | X86_PGTBL_SUPER | X86_PGTBL_GLOBAL | pt_extra_flags;
	dev_mem[dev_map_off] = (struct dev_map) {
		.physaddr = rounded,
		.virtaddr = (void *)(off * PGT1_RANGE + COS_MEM_KERN_START_VA)
	};
	dev_map_off++;
	kernel_mapped_offset++;

	assert(((unsigned long)device_pa2va(dev_addr) & (PGT1_RANGE-1)) == (dev_addr & (PGT1_RANGE-1)));

	return device_pa2va(dev_addr);
}


void
kern_paging_map_init(void *pa)
{
	u64_t i = 0;
	paddr_t       kern_pa_start = 0, kern_pa_end = (paddr_t)pa;

	/* lower mapping */
	boot_comp_pgd[0] = (u64_t)chal_va2pa(&boot_comp_pgt1) | X86_PGTBL_PRESENT | X86_PGTBL_WRITABLE;
	/* higher mapping */
	boot_comp_pgd[KERN_INIT_PGD_IDX] = (u64_t)chal_va2pa(&boot_comp_pgt1) | X86_PGTBL_PRESENT | X86_PGTBL_WRITABLE;


	for (i = kern_pa_start; i < (unsigned long)round_up_to_pgt1_page(kern_pa_end); i += PGT1_RANGE) {
		boot_comp_pgt1[i / PGT1_RANGE] = i | X86_PGTBL_PRESENT | X86_PGTBL_WRITABLE | X86_PGTBL_SUPER | X86_PGTBL_GLOBAL;
	}
}


void
paging_init(void)
{
	int ret;

	printk("Initializing virtual memory\n");
	if ((ret = kern_setup_image())) {
		die("Could not set up kernel image, errno %d.\n", ret);
	}
}
