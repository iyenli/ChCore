/*
 * Copyright (c) 2022 Institute of Parallel And Distributed Systems (IPADS)
 * ChCore-Lab is licensed under the Mulan PSL v1.
 * You can use this software according to the terms and conditions of the Mulan PSL v1.
 * You may obtain a copy of Mulan PSL v1 at:
 *     http://license.coscl.org.cn/MulanPSL
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR
 * PURPOSE.
 * See the Mulan PSL v1 for more details.
 */

#include <common/macro.h>
#include <common/util.h>
#include <common/list.h>
#include <common/errno.h>
#include <common/kprint.h>
#include <common/types.h>
#include <lib/printk.h>
#include <mm/vmspace.h>
#include <mm/kmalloc.h>
#include <mm/mm.h>
#include <mm/vmspace.h>
#include <arch/mmu.h>
#include <object/thread.h>
#include <object/cap_group.h>
#include <sched/context.h>

#define LRU_POOL_SIZE 5
#define LRU_TEST      0

#if LRU_TEST
struct lru_node {
        struct list_head node;
        /* index in "real" memory, [0, 4] here */
        int index;

        struct vaddr_node *va;
};

// vmregion and pmo index to change pmo entry to disk area
struct vaddr_node {
        struct list_head node;
        struct vmregion *vmr;
        /* for recommitting to pmo */
        int pmo_index;
};

struct lru_node *root;
void *real_memory;
int pool_size;

/******************************************************
 * tool function
 * ****************************************************/

/**
 * @brief Get the Free Index object
 * Called at least one free slot
 */
int GetFreeIndex(void)
{
        BUG_ON(pool_size >= LRU_POOL_SIZE);

        struct lru_node *tmp_node = root->node.next;
        bool used[LRU_POOL_SIZE];
        memset(used, 0, LRU_POOL_SIZE);

        while (tmp_node != &(root->node)) {
                BUG_ON(tmp_node->index < 0 || tmp_node->index >= LRU_POOL_SIZE);

                used[tmp_node->index] = true;
                tmp_node = (tmp_node->node.next);
        }

        int index_to_allocate = -1;
        for (int i = 0; i < LRU_POOL_SIZE; ++i) {
                if (!used[i]) {
                        index_to_allocate = i;
                        break;
                }
        }

        BUG_ON(index_to_allocate == -1);
        return index_to_allocate;
}

/**
 * @brief evcit page, kmalloc new page and copy, then unmap
 * @return true when success
 */
bool EvcitPageByNode(struct vmspace *vmspace, struct lru_node *to_delete)
{
        // BUG("ss");
        list_del(&(to_delete->node));
        struct vaddr_node *tmp_node = (to_delete->va);

        do {
                // vaddr is pmo_index entry in vmr
                unmap_range_in_pgtbl(vmspace->pgtbl,
                                     tmp_node->vmr->start
                                             + PAGE_SIZE * tmp_node->pmo_index,
                                     PAGE_SIZE);

                tmp_node = tmp_node->node.next;
        } while (tmp_node != to_delete->va);

        // memory copy to the "disk" memory
        paddr_t new_pa = virt_to_phys((vaddr_t)get_pages(0));

        memcpy(phys_to_virt(new_pa),
               real_memory + PAGE_SIZE * to_delete->index,
               PAGE_SIZE);

        tmp_node = (to_delete->va);
        do {
                radix_del(tmp_node->vmr->pmo->radix, tmp_node->pmo_index);
                commit_page_to_pmo(
                        tmp_node->vmr->pmo, tmp_node->pmo_index, new_pa);
                // next time, will search to this new pa and get back to real
                // memory
                tmp_node = tmp_node->node.next;

        } while (tmp_node != to_delete->va);

        // TODO: Solve memory leak here
        // avoid memory leak
        kfree(to_delete);
        --pool_size;
        return true;
}

#endif

int handle_trans_fault(struct vmspace *vmspace, vaddr_t fault_addr)
{
#if LRU_TEST
        if (root == NULL) {
                // initialize: kmalloc and set link list
                root = (struct lru_node *)(kmalloc(sizeof(struct lru_node)));
                init_list_head(&root->node);
                root->index = -1; // i'm root!
                pool_size = 0;

                real_memory = kmalloc(LRU_POOL_SIZE * PAGE_SIZE);
        }
#endif

        struct vmregion *vmr;
        struct pmobject *pmo;
        paddr_t pa;
        u64 offset;
        u64 index;
        int ret = 0;

        vmr = find_vmr_for_va(vmspace, fault_addr);

        if (vmr == NULL) {
                printk("handle_trans_fault: no vmr found for va 0x%lx!\n",
                       fault_addr);
                kinfo("process: %p\n", current_cap_group);
                print_thread(current_thread);
                kinfo("faulting IP: 0x%lx, SP: 0x%lx\n",
                      arch_get_thread_next_ip(current_thread),
                      arch_get_thread_stack(current_thread));

                kprint_vmr(vmspace);
                kwarn("TODO: kill such faulting process.\n");
                return -ENOMAPPING;
        }

        pmo = vmr->pmo;
        switch (pmo->type) {
                // Key idea: NEVER Map vaddr not from real memory to page table!
        case PMO_ANONYM:
        case PMO_SHM: {
                vmr_prop_t perm;

                perm = vmr->perm;

                /* Get the offset in the pmo for faulting addr */
                offset = ROUND_DOWN(fault_addr, PAGE_SIZE) - vmr->start;
                BUG_ON(offset >= pmo->size);

                /* Get the index in the pmo radix for faulting addr */
                index = offset / PAGE_SIZE;

                fault_addr = ROUND_DOWN(fault_addr, PAGE_SIZE);

                /* LAB 3 TODO BEGIN */
                pa = get_page_from_pmo(pmo, index);
                /* LAB 3 TODO END */

                if (pa == 0) {
                        /* Not committed before. Then, allocate the physical
                         * page. */

                        /* LAB 3 TODO BEGIN */
#if LRU_TEST
                        kdebug("Allocate new memory");
                        struct lru_node *new_node;
                        /* evcit a page, kmalloc a "disk page", memcpy */
                        /* to get in page fault handler again,  */
                        if (pool_size >= LRU_POOL_SIZE) {
                                kdebug("Evcit happens");
                                EvcitPageByNode(vmspace, root->node.prev);
                        }

                        /* kick out and alloc */
                        new_node = (struct lru_node *)(kmalloc(
                                sizeof(struct lru_node)));

                        new_node->index = GetFreeIndex();
                        list_add(&new_node->node, &root->node);
                        ++pool_size;

                        // init list
                        new_node->va = (struct vaddr_node *)kmalloc(
                                sizeof(struct vaddr_node));
                        init_list_head(&new_node->va->node);
                        new_node->va->vmr = vmr;
                        new_node->va->pmo_index = index;
#endif

#if LRU_TEST
                        pa = virt_to_phys(real_memory
                                          + PAGE_SIZE * new_node->index);
#else
                        pa = virt_to_phys(get_pages(0));
#endif

                        memset((void *)(phys_to_virt(pa)), 0, PAGE_SIZE);

                        commit_page_to_pmo(pmo, index, pa);
                        map_range_in_pgtbl(vmspace->pgtbl,
                                           fault_addr,
                                           pa,
                                           PAGE_SIZE,
                                           perm);

                        /* LAB 3 TODO END */
#ifdef CHCORE_LAB3_TEST
                        printk("Test: Test: Successfully map\n");
#endif
                } else {
                        /*
                         * pa != 0: the faulting address has be committed a
                         * physical page.
                         *
                         * For concurrent page faults:
                         *
                         * When type is PMO_ANONYM, the later faulting threads
                         * of the process do not need to modify the page
                         * table because a previous faulting thread will do
                         * that. (This is always true for the same process)
                         * However, if one process map an anonymous pmo for
                         * another process (e.g., main stack pmo), the faulting
                         * thread (e.g, in the new process) needs to update its
                         * page table.
                         * So, for simplicity, we just update the page table.
                         * Note that adding the same mapping is harmless.
                         *
                         * When type is PMO_SHM, the later faulting threads
                         * needs to add the mapping in the page table.
                         * Repeated mapping operations are harmless.
                         */
                        /* LAB 3 TODO BEGIN */

#if LRU_TEST
                        // has allocated real memory, just map
                        pa = ROUND_DOWN(pa, PAGE_SIZE);
                        if ((paddr_t)pa >= virt_to_phys(real_memory)
                            && (paddr_t)pa
                                       < virt_to_phys(real_memory)
                                                 + LRU_POOL_SIZE * PAGE_SIZE) {
                                // find lru node, put ahead of list and add into
                                // vaddr list
                                int pa_index = (pa - virt_to_phys(real_memory))
                                               / PAGE_SIZE;
                                kdebug("Map a more vpage to allocated ppage");

                                /* tmp node is the actuall allocated node */
                                struct lru_node *tmp_node = root->node.next;
                                while (tmp_node != &(root->node)) {
                                        if (tmp_node->index == pa_index) {
                                                break;
                                        }
                                        tmp_node = (tmp_node->node.next);
                                }

                                // allocate new vaddr node for tmp node
                                struct vaddr_node *new_node =
                                        (struct vaddr_node *)kmalloc(
                                                sizeof(struct vaddr_node));
                                list_add(&new_node->node, &tmp_node->va->node);
                                new_node->pmo_index = index;
                                new_node->vmr = vmr;

                                map_range_in_pgtbl(vmspace->pgtbl,
                                                   fault_addr,
                                                   pa,
                                                   PAGE_SIZE,
                                                   perm);

                                // lru
                                list_del(&tmp_node->node);
                                list_add(&tmp_node->node, &root->node);

                        } else {
                                // have been evcited, memcopy and evcit a page
                                // and remap, rebuild
                                struct lru_node *new_node;
                                if (pool_size >= LRU_POOL_SIZE) {
                                        EvcitPageByNode(vmspace,
                                                        root->node.prev);
                                }

                                new_node = (struct lru_node *)(kmalloc(
                                        sizeof(struct lru_node)));
                                new_node->index = GetFreeIndex();
                                list_add(new_node, root);

                                ++pool_size;

                                // init list
                                new_node->va = (struct vaddr_node *)kmalloc(
                                        sizeof(struct vaddr_node));
                                init_list_head(&new_node->va->node);
                                new_node->va->vmr = vmr;
                                new_node->va->pmo_index = index;

                                paddr_t real_pa = virt_to_phys(
                                        real_memory
                                        + PAGE_SIZE * new_node->index);

                                memcpy((void *)(phys_to_virt(real_pa)),
                                       (void *)(phys_to_virt(pa)),
                                       PAGE_SIZE);

                                radix_del(pmo->radix, index);
                                commit_page_to_pmo(pmo, index, real_pa);
                                map_range_in_pgtbl(vmspace->pgtbl,
                                                   fault_addr,
                                                   real_pa,
                                                   PAGE_SIZE,
                                                   perm);
                        }
#else
                        map_range_in_pgtbl(vmspace->pgtbl,
                                           fault_addr,
                                           pa,
                                           PAGE_SIZE,
                                           perm);
#endif
                        /* LAB 3 TODO END */
#ifdef CHCORE_LAB3_TEST
                        printk("Test: Test: Successfully map for pa not 0\n");
#endif
                }

                /* Cortex A53 has VIPT I-cache which is inconsistent with
                 * dcache. */
#ifdef CHCORE_ARCH_AARCH64
                if (vmr->perm & VMR_EXEC) {
                        extern void arch_flush_cache(u64, s64, int);
                        /*
                         * Currently, we assume the fauling thread is running on
                         * the CPU. Thus, we flush cache by using user VA.
                         */
                        BUG_ON(current_thread->vmspace != vmspace);
                        /* 4 means flush idcache. */
                        arch_flush_cache(fault_addr, PAGE_SIZE, 4);
                }
#endif

                break;
        }

        /* We just need handle pf above type of page! */
        case PMO_FORBID: {
                kinfo("Forbidden memory access (pmo->type is PMO_FORBID).\n");
                BUG_ON(1);
                break;
        }
        default: {
                kinfo("handle_trans_fault: faulting vmr->pmo->type"
                      "(pmo type %d at 0x%lx)\n",
                      vmr->pmo->type,
                      fault_addr);
                kinfo("Currently, this pmo type should not trigger pgfaults\n");
                kprint_vmr(vmspace);
                BUG_ON(1);
                return -ENOMAPPING;
        }
        }

        return ret;
}
