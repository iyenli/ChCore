#include <common/util.h>
#include <common/vars.h>
#include <common/macro.h>
#include <common/types.h>
#include <common/errno.h>
#include <lib/printk.h>
#include <mm/kmalloc.h>
#include <mm/mm.h>
#include <arch/mmu.h>

#include <arch/mm/page_table.h>

#define KERNEL_VADDR 0xffffff0000000000

// char new_pgtbl0[SIZE_4K];
// char new_pgtbl1[SIZE_4K];

extern void set_ttbr0_el1(paddr_t);
extern void set_ttbr1_el1(paddr_t);

void set_page_table_both(paddr_t pgtbl0, paddr_t pgtbl1)
{
        set_ttbr0_el1(pgtbl0);
        set_ttbr1_el1(pgtbl1);
}

void set_page_table(paddr_t pgtbl0)
{
        set_ttbr0_el1(pgtbl0);
}

#define USER_PTE   0
#define KERNEL_PTE 1
/*
 * the 3rd arg means the kind of PTE.
 */
static int set_pte_flags(pte_t *entry, vmr_prop_t flags, int kind)
{
        // Only consider USER PTE now.
        // For remap, modify PXN
        // BUG_ON(kind != USER_PTE);

        /*
         * Current access permission (AP) setting:
         * Mapped pages are always readable (No considering XOM).
         * EL1 can directly access EL0 (No restriction like SMAP
         * as ChCore is a microkernel).
         */
        if (flags & VMR_WRITE)
                entry->l3_page.AP = AARCH64_MMU_ATTR_PAGE_AP_HIGH_RW_EL0_RW;
        else
                entry->l3_page.AP = AARCH64_MMU_ATTR_PAGE_AP_HIGH_RO_EL0_RO;

        if (flags & VMR_EXEC)
                entry->l3_page.UXN = AARCH64_MMU_ATTR_PAGE_UX;
        else
                entry->l3_page.UXN = AARCH64_MMU_ATTR_PAGE_UXN;

        // EL1 cannot directly execute EL0 accessiable region.
        if (kind == USER_PTE) {
                entry->l3_page.PXN = AARCH64_MMU_ATTR_PAGE_PXN;
        } else {
                entry->l3_page.PXN = 0; // kernel can execute the code
        }
        // Set AF (access flag) in advance.
        entry->l3_page.AF = AARCH64_MMU_ATTR_PAGE_AF_ACCESSED;
        // Mark the mapping as not global
        entry->l3_page.nG = 1;
        // Mark the mappint as inner sharable
        entry->l3_page.SH = INNER_SHAREABLE;
        // Set the memory type
        if (flags & VMR_DEVICE) {
                entry->l3_page.attr_index = DEVICE_MEMORY;
                entry->l3_page.SH = 0;
        } else if (flags & VMR_NOCACHE) {
                entry->l3_page.attr_index = NORMAL_MEMORY_NOCACHE;
        } else {
                entry->l3_page.attr_index = NORMAL_MEMORY;
        }

        return 0;
}

#define GET_PADDR_IN_PTE(entry) \
        (((u64)entry->table.next_table_addr) << PAGE_SHIFT)
#define GET_NEXT_PTP(entry) phys_to_virt(GET_PADDR_IN_PTE(entry))

#define NORMAL_PTP (0)
#define BLOCK_PTP  (1)

/*
 * Find next page table page for the "va".
 *
 * cur_ptp: current page table page
 * level:   current ptp level
 *
 * next_ptp: returns "next_ptp"
 * pte     : returns "pte" (points to next_ptp) in "cur_ptp"
 *
 * alloc: if true, allocate a ptp when missing
 *
 */
static int get_next_ptp(ptp_t *cur_ptp, u32 level, vaddr_t va, ptp_t **next_ptp,
                        pte_t **pte, bool alloc)
{
        u32 index = 0;
        pte_t *entry;

        if (cur_ptp == NULL)
                return -ENOMAPPING;

        switch (level) {
        case 0:
                index = GET_L0_INDEX(va);
                break;
        case 1:
                index = GET_L1_INDEX(va);
                break;
        case 2:
                index = GET_L2_INDEX(va);
                break;
        case 3:
                index = GET_L3_INDEX(va);
                break;
        default:
                BUG_ON(1);
        }

        entry = &(cur_ptp->ent[index]);
        if (IS_PTE_INVALID(entry->pte)) {
                if (alloc == false) {
                        return -ENOMAPPING;
                } else {
                        /* alloc a new page table page */
                        ptp_t *new_ptp;
                        paddr_t new_ptp_paddr;
                        pte_t new_pte_val;

                        /* alloc a single physical page as a new page table page
                         */
                        new_ptp = get_pages(0);
                        BUG_ON(new_ptp == NULL);
                        memset((void *)new_ptp, 0, PAGE_SIZE);
                        new_ptp_paddr = virt_to_phys((vaddr_t)new_ptp);

                        new_pte_val.pte = 0;
                        new_pte_val.table.is_valid = 1;
                        new_pte_val.table.is_table = 1;
                        new_pte_val.table.next_table_addr = new_ptp_paddr
                                                            >> PAGE_SHIFT;

                        /* same effect as: cur_ptp->ent[index] = new_pte_val; */
                        entry->pte = new_pte_val.pte;
                }
        }

        *next_ptp = (ptp_t *)GET_NEXT_PTP(entry);
        *pte = entry;
        if (IS_PTE_TABLE(entry->pte))
                return NORMAL_PTP;
        else
                return BLOCK_PTP;
}

void free_page_table(void *pgtbl)
{
        ptp_t *l0_ptp, *l1_ptp, *l2_ptp, *l3_ptp;
        pte_t *l0_pte, *l1_pte, *l2_pte;
        int i, j, k;

        if (pgtbl == NULL) {
                kwarn("%s: input arg is NULL.\n", __func__);
                return;
        }

        /* L0 page table */
        l0_ptp = (ptp_t *)pgtbl;

        /* Interate each entry in the l0 page table*/
        for (i = 0; i < PTP_ENTRIES; ++i) {
                l0_pte = &l0_ptp->ent[i];
                if (IS_PTE_INVALID(l0_pte->pte) || !IS_PTE_TABLE(l0_pte->pte))
                        continue;
                l1_ptp = (ptp_t *)GET_NEXT_PTP(l0_pte);

                /* Interate each entry in the l1 page table*/
                for (j = 0; j < PTP_ENTRIES; ++j) {
                        l1_pte = &l1_ptp->ent[j];
                        if (IS_PTE_INVALID(l1_pte->pte)
                            || !IS_PTE_TABLE(l1_pte->pte))
                                continue;
                        l2_ptp = (ptp_t *)GET_NEXT_PTP(l1_pte);

                        /* Interate each entry in the l2 page table*/
                        for (k = 0; k < PTP_ENTRIES; ++k) {
                                l2_pte = &l2_ptp->ent[k];
                                if (IS_PTE_INVALID(l2_pte->pte)
                                    || !IS_PTE_TABLE(l2_pte->pte))
                                        continue;
                                l3_ptp = (ptp_t *)GET_NEXT_PTP(l2_pte);
                                /* Free the l3 page table page */
                                free_pages(l3_ptp);
                        }

                        /* Free the l2 page table page */
                        free_pages(l2_ptp);
                }

                /* Free the l1 page table page */
                free_pages(l1_ptp);
        }

        free_pages(l0_ptp);
}

/*
 * Translate a va to pa, and get its pte for the flags
 */
int query_in_pgtbl(void *pgtbl, vaddr_t va, paddr_t *pa, pte_t **entry)
{
        /* LAB 2 TODO 3 BEGIN */
        /*
         * Hint: Walk through each level of page table using `get_next_ptp`,
         * return the pa and pte until a L0/L1 block or page, return
         * `-ENOMAPPING` if the va is not mapped.
         */
        int flag = 0x0;
        *pa = 0;
        ptp_t *next_ptp = pgtbl;
        pte_t *next_pte = NULL;

        flag = get_next_ptp(next_ptp, 0, va, &next_ptp, &next_pte, false);
        if (flag != NORMAL_PTP) { // flag = -ENOMAPPING
                // kdebug("Return Error when query L0\n");
                return -ENOMAPPING;
        }

        flag = get_next_ptp(next_ptp, 1, va, &next_ptp, &next_pte, false);
        if (flag < 0) {
                // kdebug("Return Error when query L1\n");
                return flag;
        }

        // support huge page
        if (flag == BLOCK_PTP) {
                u64 tmp = next_pte->l1_block.pfn;
                (*pa) = ((tmp << L1_INDEX_SHIFT) | (GET_VA_OFFSET_L1(va)));

                *entry = next_pte;
                return NORMAL_MEMORY;
        }

        BUG_ON(flag != NORMAL_PTP);
        flag = get_next_ptp(next_ptp, 2, va, &next_ptp, &next_pte, false);
        if (flag < 0) {
                // kdebug("Return Error when query L2\n");
                return flag;
        }
        if (flag == BLOCK_PTP) {
                u64 tmp = next_pte->l2_block.pfn;
                (*pa) = ((tmp << L2_INDEX_SHIFT) | (GET_VA_OFFSET_L2(va)));

                *entry = next_pte;
                return NORMAL_MEMORY;
        }

        BUG_ON(flag != NORMAL_PTP);
        flag = get_next_ptp(next_ptp, 3, va, &next_ptp, &next_pte, false);
        if (flag < 0) {
                // kdebug("Return Error when query L3\n");
                return flag;
        }

        // So weird!!!
        u64 tmp = next_pte->l3_page.pfn;
        (*pa) = ((tmp << L3_INDEX_SHIFT) | (GET_VA_OFFSET_L3(va)));

        *entry = next_pte;
        return NORMAL_MEMORY;
        /* LAB 2 TODO 3 END */
}

int map_4k(void *pgtbl, vaddr_t va, paddr_t pa, vmr_prop_t flags)
{
        int flag;
        ptp_t *next_ptp = pgtbl;
        pte_t *next_pte = NULL;

        for (int k = 0; k < 3; ++k) {
                flag = get_next_ptp(
                        next_ptp, k, va, &next_ptp, &next_pte, true);

                if (flag < 0) {
                        kdebug("Return Error when map 4K page\n");
                        return flag;
                }
        }

        next_pte = &(next_ptp->ent[GET_L3_INDEX(va)]);
        next_pte->pte = 0; // memset

        next_pte->l3_page.pfn = ((pa) >> L3_INDEX_SHIFT);
        next_pte->l3_page.is_page = 1;
        next_pte->l3_page.is_valid = 1;

        if (va & (KERNEL_VADDR)) {
                set_pte_flags(next_pte, flags, KERNEL_PTE);
        } else {
                set_pte_flags(next_pte, flags, USER_PTE);
        }

        return NORMAL_MEMORY;
}

int map_2M(void *pgtbl, vaddr_t va, paddr_t pa, vmr_prop_t flags)
{
        int flag;
        ptp_t *next_ptp = pgtbl;
        pte_t *next_pte = NULL;

        // if first time, we will alloc it
        // again, else just return
        for (int k = 0; k < 2; ++k) {
                flag = get_next_ptp(
                        next_ptp, k, va, &next_ptp, &next_pte, true);

                if (flag < 0) {
                        // kdebug("Return Error when map 2M page\n");
                        return flag;
                }
        }

        next_pte = &(next_ptp->ent[GET_L2_INDEX(va)]);
        next_pte->pte = 0;
        next_pte->l2_block.pfn = ((pa) >> L2_INDEX_SHIFT);
        next_pte->l2_block.is_table = 0;
        next_pte->l2_block.is_valid = 1;
        set_pte_flags(next_pte, flags, USER_PTE);

        return NORMAL_MEMORY;
}

int map_1G(void *pgtbl, vaddr_t va, paddr_t pa, vmr_prop_t flags)
{
        int flag;
        ptp_t *next_ptp = pgtbl;
        pte_t *next_pte = NULL;

        // if first time, we will alloc it
        // again, else just return
        flag = get_next_ptp(next_ptp, 0, va, &next_ptp, &next_pte, true);
        if (flag < 0) {
                // kdebug("Return Error when map 1G page\n");
                return flag;
        }

        next_pte = &(next_ptp->ent[GET_L1_INDEX(va)]);
        next_pte->pte = 0;
        next_pte->l1_block.pfn = ((pa) >> L1_INDEX_SHIFT);
        next_pte->l1_block.is_table = 0;
        next_pte->l1_block.is_valid = 1;
        set_pte_flags(next_pte, flags, USER_PTE);

        return NORMAL_MEMORY;
}

int unmap_4k(void *pgtbl, vaddr_t va)
{
        int flag;
        ptp_t *next_ptp = pgtbl;
        pte_t *next_pte = NULL;

        for (int k = 0; k <= 3; ++k) {
                flag = get_next_ptp(
                        next_ptp, k, va, &next_ptp, &next_pte, false);

                if (flag < 0) {
                        kdebug("Return Error\n");
                        return flag;
                }
        }
        next_pte->pte = (next_pte->pte & (~(0x1ULL)));
        BUG_ON(next_pte->l3_page.is_valid != 0);

        return NORMAL_MEMORY;
}

int unmap_2M(void *pgtbl, vaddr_t va)
{
        int flag;
        ptp_t *next_ptp = pgtbl;
        pte_t *next_pte = NULL;

        for (int k = 0; k <= 2; ++k) {
                flag = get_next_ptp(
                        next_ptp, k, va, &next_ptp, &next_pte, false);

                if (flag < 0) {
                        // kdebug("Return Error\n");
                        return flag;
                }
        }
        next_pte->pte = (next_pte->pte & (~(0x1UL)));
        BUG_ON(next_pte->l2_block.is_valid != 0);
        return NORMAL_MEMORY;
}

int unmap_1G(void *pgtbl, vaddr_t va)
{
        int flag;
        ptp_t *next_ptp = pgtbl;
        pte_t *next_pte = NULL;

        for (int k = 0; k <= 1; ++k) {
                flag = get_next_ptp(
                        next_ptp, k, va, &next_ptp, &next_pte, false);

                if (flag < 0) {
                        // kdebug("Return Error\n");
                        return flag;
                }
        }
        next_pte->pte = (next_pte->pte & (~(0x1UL)));
        BUG_ON(next_pte->l1_block.is_valid != 0);
        return NORMAL_MEMORY;
}

// Just support 4K
int map_range_in_pgtbl(void *pgtbl, vaddr_t va, paddr_t pa, size_t len,
                       vmr_prop_t flags)
{
        int flag;
        vaddr_t va_end = (va + len);
        BUG_ON((va & (SIZE_4K - 1)) || (len & (SIZE_4K - 1)));

        for (; va < va_end; va += SIZE_4K, pa += SIZE_4K) {
                if ((flag = map_4k(pgtbl, va, pa, flags)) < 0) {
                        kdebug("return error in map stage 1");
                        return flag;
                }
        }

        BUG_ON(va != va_end);
        return NORMAL_MEMORY;
}

// Just support 4K
int unmap_range_in_pgtbl(void *pgtbl, vaddr_t va, size_t len)
{
        /* LAB 2 TODO 3 BEGIN */
        /*
         * Hint: Walk through each level of page table using `get_next_ptp`,
         * mark the final level pte as invalid. Iterate until all pages are
         * unmapped.
         */

        int flag;

        vaddr_t va_end = va + len;
        BUG_ON((va & (SIZE_4K - 1)) || (len & (SIZE_4K - 1)));

        for (; va < va_end; va += SIZE_4K) {
                if ((flag = unmap_4k(pgtbl, va)) < 0) {
                        kdebug("return error in unmap stage 1");
                        return flag;
                }
        }
        if (va == va_end) {
                return NORMAL_MEMORY;
        }

        BUG("Unreachable here.");
        return -ENOMAPPING; /* LAB 2 TODO 3 END */
        /* LAB 2 TODO 3 END */
}

int map_range_in_pgtbl_huge(void *pgtbl, vaddr_t va, paddr_t pa, size_t len,
                            vmr_prop_t flags)
{
        /* LAB 2 TODO 4 BEGIN */
        /* LAB 2 TODO 3 BEGIN */
        /*
         * Hint: Walk through each level of page table using `get_next_ptp`,
         * create new page table page if necessary, fill in the final level
         * pte with the help of `set_pte_flags`. Iterate until all pages are
         * mapped.
         */
        int flag;
        vaddr_t va_end = (va + len), tmp_va_end = 0;
        BUG_ON((va & (SIZE_4K - 1)) || (len & (SIZE_4K - 1)));

        tmp_va_end = MIN(ROUND_UP(va, SIZE_2M), va_end);
        for (; va < tmp_va_end; va += SIZE_4K, pa += SIZE_4K) {
                if ((flag = map_4k(pgtbl, va, pa, flags)) < 0) {
                        // kdebug("return error in map stage 1");
                        return flag;
                }
        }
        if (va == va_end) {
                return NORMAL_MEMORY;
        }
        BUG_ON(pa != va);

        tmp_va_end = MIN(ROUND_UP(va, SIZE_1G), va_end);
        for (; va < tmp_va_end; va += SIZE_2M, pa += SIZE_2M) {
                if ((flag = map_2M(pgtbl, va, pa, flags)) < 0) {
                        // kdebug("return error in map stage 2");
                        return flag;
                }
        }
        if (va == va_end) {
                return NORMAL_MEMORY;
        }
        BUG_ON(pa != va);

        tmp_va_end = (ROUND_DOWN(va_end, SIZE_1G));
        for (; va < tmp_va_end; va += SIZE_1G, pa += SIZE_1G) {
                if ((flag = map_1G(pgtbl, va, pa, flags)) < 0) {
                        // kdebug("return error in map stage 3");
                        return flag;
                }
        }
        if (va == va_end) {
                return NORMAL_MEMORY;
        }
        BUG_ON(pa != va);

        tmp_va_end = (ROUND_DOWN(va_end, SIZE_2M));
        for (; va < tmp_va_end; va += SIZE_2M, pa += SIZE_2M) {
                if ((flag = map_2M(pgtbl, va, pa, flags)) < 0) {
                        // kdebug("return error in map stage 4");
                        return flag;
                }
        }
        if (va == va_end) {
                return NORMAL_MEMORY;
        }
        BUG_ON(pa != va);

        for (; va < va_end; va += SIZE_4K, pa += SIZE_4K) {
                if ((flag = map_4k(pgtbl, va, pa, flags)) < 0) {
                        // kdebug("return error in map stage 5");
                        return flag;
                }
        }
        if (va == va_end) {
                return NORMAL_MEMORY;
        }

        BUG("Unreachable here.");
        return -ENOMAPPING; /* LAB 2 TODO 3 END */
        /* LAB 2 TODO 4 END */
}

int unmap_range_in_pgtbl_huge(void *pgtbl, vaddr_t va, size_t len)
{
        /* LAB 2 TODO 4 BEGIN */
        int flag;

        vaddr_t va_end = va + len, tmp_va_end = 0;
        BUG_ON((va & (SIZE_4K - 1)) || (len & (SIZE_4K - 1)));

        tmp_va_end = MIN(ROUND_UP(va, SIZE_2M), va_end);
        for (; va < tmp_va_end; va += SIZE_4K) {
                if ((flag = unmap_4k(pgtbl, va)) < 0) {
                        return flag;
                }
        }
        if (va == va_end) {
                return NORMAL_MEMORY;
        }

        tmp_va_end = MIN(ROUND_UP(va, SIZE_1G), va_end);
        for (; va < tmp_va_end; va += SIZE_2M) {
                if ((flag = unmap_2M(pgtbl, va)) < 0) {
                        return flag;
                }
        }
        if (va == va_end) {
                return NORMAL_MEMORY;
        }

        tmp_va_end = (ROUND_DOWN(va_end, SIZE_1G));
        for (; va < tmp_va_end; va += SIZE_1G) {
                if ((flag = unmap_1G(pgtbl, va)) < 0) {
                        return flag;
                }
        }
        if (va == va_end) {
                return NORMAL_MEMORY;
        }

        tmp_va_end = (ROUND_DOWN(va_end, SIZE_2M));
        for (; va < tmp_va_end; va += SIZE_2M) {
                if ((flag = unmap_2M(pgtbl, va)) < 0) {
                        return flag;
                }
        }
        if (va == va_end) {
                return NORMAL_MEMORY;
        }

        for (; va < va_end; va += SIZE_4K) {
                if ((flag = unmap_4k(pgtbl, va)) < 0) {
                        return flag;
                }
        }
        if (va == va_end) {
                return NORMAL_MEMORY;
        }
        BUG("Unreachable here.");
        return NORMAL_MEMORY;
        /* LAB 2 TODO 4 END */
}

#define PHYSMEM_START   (0x0UL)
#define PERIPHERAL_BASE (0x3F000000UL) // 16 MB
#define PHYSMEM_END     (0x40000000UL)

void remap(void)
{
        void *new_pgtbl0 = get_pages(0), *new_pgtbl1 = get_pages(0);

        /**
         * UXN: NOT SET VMR_EXEC
         * ACCESSED, NG: Default set in function
         * SHARED: Need Set here!
         * NORMAL/DEVICE: VMR_DEVICE
         */
        memset(new_pgtbl0, 0, PAGE_SIZE);
        memset(new_pgtbl1, 0, PAGE_SIZE);
        map_range_in_pgtbl(new_pgtbl0,
                           PHYSMEM_START,
                           PHYSMEM_START,
                           PERIPHERAL_BASE - PHYSMEM_START,
                           0);

        map_range_in_pgtbl(new_pgtbl0,
                           PERIPHERAL_BASE,
                           PERIPHERAL_BASE,
                           PHYSMEM_END - PERIPHERAL_BASE,
                           VMR_DEVICE);

        map_range_in_pgtbl(new_pgtbl1,
                           PHYSMEM_START | KERNEL_VADDR,
                           PHYSMEM_START,
                           PERIPHERAL_BASE - PHYSMEM_START,
                           0);

        map_range_in_pgtbl(new_pgtbl1,
                           PERIPHERAL_BASE | KERNEL_VADDR,
                           PERIPHERAL_BASE,
                           PHYSMEM_END - PERIPHERAL_BASE,
                           VMR_DEVICE);

        map_range_in_pgtbl(new_pgtbl1,
                           PHYSMEM_END | KERNEL_VADDR,
                           PHYSMEM_END,
                           SIZE_1G,
                           VMR_DEVICE);

        u64 pgtbl0_pa, pgtbl1_pa;
        pte_t *pte;
        int res1 = query_in_pgtbl(new_pgtbl1, new_pgtbl0, &pgtbl0_pa, &pte);
        int res2 = query_in_pgtbl(new_pgtbl1, new_pgtbl1, &pgtbl1_pa, &pte);
        kdebug("res1: %d, res2: %d \n", res1, res2);
        kdebug("res1: 0x%llx, res2: 0x%llx \n", pgtbl0_pa, pgtbl1_pa);
        kdebug("res1: 0x%llx, res2: 0x%llx \n",
               (u64)new_pgtbl0,
               (u64)new_pgtbl1);

        set_page_table_both(pgtbl0_pa, pgtbl1_pa);
}

#ifdef CHCORE_KERNEL_TEST
#include <mm/buddy.h>
#include <lab.h>
void lab2_test_page_table(void)
{
        vmr_prop_t flags = VMR_READ | VMR_WRITE;
        {
                bool ok = true;
                void *pgtbl = get_pages(0);
                memset(pgtbl, 0, PAGE_SIZE);
                paddr_t pa;
                pte_t *pte;
                int ret;

                ret = map_range_in_pgtbl(
                        pgtbl, 0x1001000, 0x1000, PAGE_SIZE, flags);
                lab_assert(ret == 0);

                ret = query_in_pgtbl(pgtbl, 0x1001000, &pa, &pte);
                lab_assert(ret == 0 && pa == 0x1000);
                lab_assert(pte && pte->l3_page.is_valid && pte->l3_page.is_page
                           && pte->l3_page.SH == INNER_SHAREABLE);
                ret = query_in_pgtbl(pgtbl, 0x1001050, &pa, &pte);
                lab_assert(ret == 0 && pa == 0x1050);

                ret = unmap_range_in_pgtbl(pgtbl, 0x1001000, PAGE_SIZE);
                lab_assert(ret == 0);
                ret = query_in_pgtbl(pgtbl, 0x1001000, &pa, &pte);
                lab_assert(ret == -ENOMAPPING);

                free_page_table(pgtbl);
                lab_check(ok, "Map & unmap one page");
                BUG_ON(!ok);
        }
        {
                bool ok = true;
                void *pgtbl = get_pages(0);
                memset(pgtbl, 0, PAGE_SIZE);
                paddr_t pa;
                pte_t *pte;
                int ret;
                size_t nr_pages = 10;
                size_t len = PAGE_SIZE * nr_pages;

                ret = map_range_in_pgtbl(pgtbl, 0x1001000, 0x1000, len, flags);
                lab_assert(ret == 0);
                ret = map_range_in_pgtbl(
                        pgtbl, 0x1001000 + len, 0x1000 + len, len, flags);
                lab_assert(ret == 0);

                for (int i = 0; i < nr_pages * 2; i++) {
                        ret = query_in_pgtbl(
                                pgtbl, 0x1001050 + i * PAGE_SIZE, &pa, &pte);
                        lab_assert(ret == 0 && pa == 0x1050 + i * PAGE_SIZE);
                        lab_assert(pte && pte->l3_page.is_valid
                                   && pte->l3_page.is_page);
                }

                ret = unmap_range_in_pgtbl(pgtbl, 0x1001000, len);
                lab_assert(ret == 0);
                ret = unmap_range_in_pgtbl(pgtbl, 0x1001000 + len, len);
                lab_assert(ret == 0);

                for (int i = 0; i < nr_pages * 2; i++) {
                        ret = query_in_pgtbl(
                                pgtbl, 0x1001050 + i * PAGE_SIZE, &pa, &pte);
                        lab_assert(ret == -ENOMAPPING);
                }

                free_page_table(pgtbl);
                lab_check(ok, "Map & unmap multiple pages");
        }
        {
                bool ok = true;
                void *pgtbl = get_pages(0);
                memset(pgtbl, 0, PAGE_SIZE);
                paddr_t pa;
                pte_t *pte;
                int ret;
                /* 1GB + 4MB + 40KB */
                size_t len = (1 << 30) + (4 << 20) + 10 * PAGE_SIZE;

                ret = map_range_in_pgtbl(
                        pgtbl, 0x100000000, 0x100000000, len, flags);
                lab_assert(ret == 0);
                ret = map_range_in_pgtbl(pgtbl,
                                         0x100000000 + len,
                                         0x100000000 + len,
                                         len,
                                         flags);
                lab_assert(ret == 0);

                for (vaddr_t va = 0x100000000; va < 0x100000000 + len * 2;
                     va += 5 * PAGE_SIZE + 0x100) {
                        ret = query_in_pgtbl(pgtbl, va, &pa, &pte);
                        lab_assert(ret == 0 && pa == va);
                }

                ret = unmap_range_in_pgtbl(pgtbl, 0x100000000, len);
                lab_assert(ret == 0);
                ret = unmap_range_in_pgtbl(pgtbl, 0x100000000 + len, len);
                lab_assert(ret == 0);

                for (vaddr_t va = 0x100000000; va < 0x100000000 + len;
                     va += 5 * PAGE_SIZE + 0x100) {
                        ret = query_in_pgtbl(pgtbl, va, &pa, &pte);
                        lab_assert(ret == -ENOMAPPING);
                }

                free_page_table(pgtbl);
                lab_check(ok, "Map & unmap huge range");
        }
        {
                bool ok = true;
                void *pgtbl = get_pages(0);
                memset(pgtbl, 0, PAGE_SIZE);
                paddr_t pa;
                pte_t *pte;
                int ret;
                /* 1GB + 4MB + 40KB */
                size_t len = (1 << 30) + (4 << 20) + 10 * PAGE_SIZE;
                size_t free_mem, used_mem;

                free_mem = get_free_mem_size_from_buddy(&global_mem[0]);
                ret = map_range_in_pgtbl_huge(
                        pgtbl, 0x100000000, 0x100000000, len, flags);
                lab_assert(ret == 0);

                used_mem =
                        free_mem - get_free_mem_size_from_buddy(&global_mem[0]);
                lab_assert(used_mem < PAGE_SIZE * 8);

                for (vaddr_t va = 0x100000000; va < 0x100000000 + len;
                     va += 5 * PAGE_SIZE + 0x100) {
                        ret = query_in_pgtbl(pgtbl, va, &pa, &pte);
                        lab_assert(ret == 0 && pa == va);
                }

                ret = unmap_range_in_pgtbl_huge(pgtbl, 0x100000000, len);
                lab_assert(ret == 0);

                for (vaddr_t va = 0x100000000; va < 0x100000000 + len;
                     va += 5 * PAGE_SIZE + 0x100) {
                        ret = query_in_pgtbl(pgtbl, va, &pa, &pte);
                        lab_assert(ret == -ENOMAPPING);
                }

                free_page_table(pgtbl);
                lab_check(ok, "Map & unmap with huge page support");
        }
        printk("[TEST] Page table tests finished\n");
}
#endif /* CHCORE_KERNEL_TEST */
