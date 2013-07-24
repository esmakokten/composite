#include "types.h"
#include "printk.h"
#include "string.h"
#include "isr.h"
#include "mm.h"
#include "vm.h"

uint32_t pagedir[1024] __attribute__((aligned(4096)));
uint32_t pagetab[1024][1024] __attribute__((aligned(4096)));

void *
chal_pa2va(void *address)
{
    return address;
}

void *
chal_va2pa(void *address)
{
    return address;
}

static void
load_page_directory(size_t dir)
{
    size_t cr0;
    uint32_t d = (uint32_t)chal_va2pa(pagedir) | PAGE_P;
     
printk (INFO, "cr3 = %x (%x)\n", d, d);
    asm volatile("mov %0, %%cr3" : : "r"(d));
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
printk (INFO, "cr0 == %x\n", cr0);
    cr0 |= 0x80000000;
printk (INFO, "cr0 = %x\n", cr0 | 0x80000000);
    asm volatile("mov %0, %%cr0" : : "r"(cr0));
printk (INFO, "OK\n");
}

static void
page_fault(struct registers *regs)
{
    uintptr_t fault_addr;
    
    asm volatile("mov %%cr2, %0" : "=r" (fault_addr));

    die("Page Fault (%s%s%s%s) at 0x%X\n",
        !(regs->err_code & PAGE_P) ? "present " : "",
        regs->err_code & PAGE_RW ? "read-only " : "",
        regs->err_code & PAGE_US ? "user-mode " : "",
        regs->err_code & PAGE_PCD ? "reserved " : "",
        fault_addr);

}


void
paging__init(size_t memory_size)
{
    int i, j;
    printk(INFO, "Intialize paging\n");
    printk(INFO, "MEMORY_SIZE: %dMB\n", memory_size/1024);

    printk(INFO, "Registering handler\n");
    register_interrupt_handler(14, &page_fault);

    printk(INFO, "Mapping pages to tables and directories\n");
    for (i = 0; i < 1024; i++) {
      pagedir[i] = (((uint32_t)pagetab[i] & PAGE_FRAME) | PAGE_RW | PAGE_P);
      for (j = 0; j < 1024; j++) {
        pagetab[i][j] = (((4096 * i * 1024) + (j * 4096)) & PAGE_FRAME) | PAGE_RW | (i == 0 ? PAGE_P : 0);
      }
    }

    printk(INFO, "Loading page directory\n");
//printk(INFO, "pagetab == %x\n", pagetab);
printk(INFO, "flags == %x\n", (PAGE_RW | PAGE_G) & (~ PAGE_US));
printk(INFO, "pagedir[0] == %x\n", pagedir[0]);
    load_page_directory(0);
    
    printk(INFO, "Finished\n");
}
