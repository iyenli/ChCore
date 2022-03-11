#include <common/util.h>
#include <common/macro.h>
#include <common/kprint.h>
#include <mm/buddy.h>

/*
 * The layout of a phys_mem_pool:
 * | page_metadata are (an array of struct page) | alignment pad | usable memory
 * |
 *
 * The usable memory: [pool_start_addr, pool_start_addr + pool_mem_size).
 */
void init_buddy(struct phys_mem_pool *pool, struct page *start_page,
                vaddr_t start_addr, u64 page_num)
{
        int order;
        int page_idx;
        struct page *page;

        /* Init the physical memory pool. */
        pool->pool_start_addr = start_addr;
        pool->page_metadata = start_page;
        pool->pool_mem_size = page_num * BUDDY_PAGE_SIZE;
        /* This field is for unit test only. */
        pool->pool_phys_page_num = page_num;

        /* Init the free lists */
        for (order = 0; order < BUDDY_MAX_ORDER; ++order) {
                pool->free_lists[order].nr_free = 0;
                init_list_head(&(pool->free_lists[order].free_list));
        }

        /* Clear the page_metadata area. */
        memset((char *)start_page, 0, page_num * sizeof(struct page));

        /* Init the page_metadata area. */
        for (page_idx = 0; page_idx < page_num; ++page_idx) {
                page = start_page + page_idx; // a pointer to this page struct
                page->allocated = 1; // not free
                page->order = 0; // order-zero now, 4K per page
                page->pool = pool; // reverse ptr
        }

        kdebug("Ready to call function buddy_free_pages");
        /* Put each physical memory page into the free lists. */
        for (page_idx = 0; page_idx < page_num; ++page_idx) {
                page = start_page + page_idx;
                buddy_free_pages(pool, page);
        }
}

/**
 * @brief Get the buddy chunk object
 *
 * @param pool
 * @param chunk the page desiring to find its buddy
 * @return struct page* its buddy
 */
static struct page *get_buddy_chunk(struct phys_mem_pool *pool,
                                    struct page *chunk)
{
        u64 chunk_addr;
        u64 buddy_chunk_addr;
        int order;

        /* Get the address of the chunk. */
        chunk_addr = (u64)page_to_virt(chunk);
        order = chunk->order;
/*
 * Calculate the address of the buddy chunk according to the address
 * relationship between buddies.
 */
#define BUDDY_PAGE_SIZE_ORDER (12)
        buddy_chunk_addr = chunk_addr
                           ^ (1UL << (order + BUDDY_PAGE_SIZE_ORDER));

        /* Check whether the buddy_chunk_addr belongs to pool. */
        if ((buddy_chunk_addr < pool->pool_start_addr)
            || (buddy_chunk_addr
                >= (pool->pool_start_addr + pool->pool_mem_size))) {
                return NULL;
        }

        return virt_to_page((void *)buddy_chunk_addr);
}

// When input the page, it should be in some free lists.
// output page still in some free list
static struct page *split_page(struct phys_mem_pool *pool, u64 order,
                               struct page *page)
{
        /* LAB 2 TODO 2 BEGIN */
        /*
         * Hint: Recursively put the buddy of current chunk into
         * a suitable free list.
         */

        // delete this page
        int page_order = page->order;
        if (page_order == order) {
                return page;
        }
        int split_size = (1 << (page_order - 1));
        struct page *another_page = page + split_size;

        list_del(&(page->node));
        pool->free_lists[page->order].nr_free--;
        BUG_ON((long long)pool->free_lists[page->order].nr_free < 0);

        // split and decrease order
        for (int i = 0; i < split_size; ++i) {
                (page + i)->order--;
                (another_page + i)->order--;
        }

        list_add(&(another_page->node),
                 &(pool->free_lists[another_page->order].free_list));
        list_add(&(page->node), &(pool->free_lists[page->order].free_list));
        BUG_ON(another_page->order != page->order);
        BUG_ON(get_buddy_chunk(pool, page) != another_page);
        pool->free_lists[another_page->order].nr_free += 2;

        return split_page(pool, order, page);
        /* LAB 2 TODO 2 END */
}

struct page *buddy_get_pages(struct phys_mem_pool *pool, u64 order)
{
        /* LAB 2 TODO 2 BEGIN */
        /*
         * Hint: Find a chunk that satisfies the order requirement
         * in the free lists, then split it if necessary.
         */
        struct page *page = NULL;
        for (u64 i = order; i < BUDDY_MAX_ORDER; ++i) {
                if (pool->free_lists[i].nr_free > 0) {
                        if (!list_empty(&(pool->free_lists[i].free_list))) {
                                page = split_page(
                                        pool,
                                        order,
                                        // dangerous, but correct maybe
                                        // because node is first member of page
                                        (struct page *)(pool->free_lists[i]
                                                                .free_list
                                                                .next));

                                int page_size = (1 << page->order);
                                for (int i = 0; i < page_size; ++i) {
                                        if ((page + i)->allocated) {
                                                BUG("Allocated bit still error");
                                        }
                                        (page + i)->allocated = 1;
                                }

                                list_del(&(page->node));
                                pool->free_lists[page->order].nr_free--;
                                break;
                        } else {
                                BUG("Unreachable here, num = %d\n",
                                    pool->free_lists[i].nr_free);
                        }
                }
        }

        return page;
        /* LAB 2 TODO 2 END */
}

static struct page *merge_page(struct phys_mem_pool *pool, struct page *page)
{
        /* LAB 2 TODO 2 BEGIN */
        /*
         * Hint: Recursively merge current chunk with its buddy
         * if possible.
         */

        // Just care about allocated, node, order
        // 1. set all page meta data in merged area
        // 2. We assume page is in the free list, if buddy is free too, it
        // should be in free list
        // 3. recrusively call, until buddy is busy now
        struct page *buddy = get_buddy_chunk(pool, page);
        if (!buddy || buddy->allocated
            || page->order == (BUDDY_MAX_ORDER - 1)) {
                return page;
        }

        // buddy is free  delete from list
        list_del(&(page->node));
        list_del(&(buddy->node));
        if (page->order != buddy->order) {
                BUG("page order: %d, buddy order: %d",
                    page->order,
                    buddy->order);
        }
        // delete two buddy chunks
        pool->free_lists[page->order].nr_free -= 2;
        BUG_ON((s64)pool->free_lists[page->order].nr_free < 0);

        int page_size = (0x1 << (page->order));
        // set order + 1
        for (int i = 0; i < page_size; ++i) {
                if ((page + i)->order != (buddy + i)->order) {
                        BUG("page order: %d, buddy order: %d",
                            (page + i)->order,
                            (buddy + i)->order);
                }
                (page + i)->order++;
                (buddy + i)->order++;
        }

        // who is the prev buddy?
        page = ((page - pool->page_metadata) < (buddy - pool->page_metadata)) ?
                       page :
                       buddy;

        // add it!
        list_add(&(page->node), &(pool->free_lists[page->order].free_list));
        pool->free_lists[page->order].nr_free++;

        return merge_page(pool, page);
        /* LAB 2 TODO 2 END */
}

void buddy_free_pages(struct phys_mem_pool *pool, struct page *page)
{
        /* LAB 2 TODO 2 BEGIN */
        /*
         * Hint: Merge the chunk with its buddy and put it into
         * a suitable free list.
         */
        // first, set allocate = 0.
        if (page->allocated) {
                int page_size = (0x1 << (page->order));
                for (int i = 0; i < page_size; ++i) {
                        if (!(page + i)->allocated) {
                                BUG("Allocated bit still error");
                        }
                        (page + i)->allocated = 0;
                }

                // add sum of free from zero
                pool->free_lists[page->order].nr_free++;
                list_add(&(page->node),
                         &(pool->free_lists[page->order].free_list));

                // Invariant: in 1 << order area, allocate info is same to
                // header->allocate!
                struct page *buddy = get_buddy_chunk(pool, page);
                if (buddy && !buddy->allocated) {
                        page = merge_page(pool, page);
                }
        }

        /* LAB 2 TODO 2 END */
}

void *page_to_virt(struct page *page)
{
        u64 addr;
        struct phys_mem_pool *pool = page->pool;

        BUG_ON(pool == NULL);
        /* page_idx * BUDDY_PAGE_SIZE + start_addr */
        addr = (page - pool->page_metadata) * BUDDY_PAGE_SIZE
               + pool->pool_start_addr;
        return (void *)addr;
}

struct page *virt_to_page(void *ptr)
{
        struct page *page;
        struct phys_mem_pool *pool = NULL;
        u64 addr = (u64)ptr;
        int i;

        /* Find the corresponding physical memory pool. */
        for (i = 0; i < physmem_map_num; ++i) {
                if (addr >= global_mem[i].pool_start_addr
                    && addr < global_mem[i].pool_start_addr
                                       + global_mem[i].pool_mem_size) {
                        pool = &global_mem[i];
                        break;
                }
        }

        BUG_ON(pool == NULL);
        page = pool->page_metadata
               + (((u64)addr - pool->pool_start_addr) / BUDDY_PAGE_SIZE);
        return page;
}

u64 get_free_mem_size_from_buddy(struct phys_mem_pool *pool)
{
        int order;
        struct free_list *list;
        u64 current_order_size;
        u64 total_size = 0;

        for (order = 0; order < BUDDY_MAX_ORDER; order++) {
                /* 2^order * 4K */
                current_order_size = BUDDY_PAGE_SIZE * (1 << order);
                list = pool->free_lists + order;
                total_size += list->nr_free * current_order_size;

                /* debug : print info about current order */
                // kdebug("buddy memory chunk order: %d, size: 0x%lx, num:
                // %d\n",
                //        order,
                //        current_order_size,
                //        list->nr_free);
        }
        return total_size;
}

#ifdef CHCORE_KERNEL_TEST
#include <mm/mm.h>
#include <lab.h>
void lab2_test_buddy(void)
{
        struct phys_mem_pool *pool = &global_mem[0];
        {
                u64 free_mem_size = get_free_mem_size_from_buddy(pool);
                lab_check(pool->pool_phys_page_num == free_mem_size / PAGE_SIZE,
                          "Init buddy");
        }
        {
                lab_check(buddy_get_pages(pool, BUDDY_MAX_ORDER + 1) == NULL
                                  && buddy_get_pages(pool, (u64)-1) == NULL,
                          "Check invalid order");
        }
        {
                bool ok = true;
                u64 expect_free_mem = pool->pool_phys_page_num * PAGE_SIZE;
                struct page *page = buddy_get_pages(pool, 0);
                BUG_ON(page == NULL);
                lab_assert(page->order == 0 && page->allocated);
                expect_free_mem -= PAGE_SIZE;
                ok = ok
                     && get_free_mem_size_from_buddy(pool) == expect_free_mem;
                buddy_free_pages(pool, page);
                expect_free_mem += PAGE_SIZE;
                ok = ok
                     && get_free_mem_size_from_buddy(pool) == expect_free_mem;
                lab_check(ok, "Allocate & free order 0");
        }
        {
                bool ok = true;
                u64 expect_free_mem = pool->pool_phys_page_num * PAGE_SIZE;
                struct page *page;
                for (int i = 0; i < BUDDY_MAX_ORDER; i++) {
                        page = buddy_get_pages(pool, i);
                        BUG_ON(page == NULL);
                        lab_assert(page->order == i && page->allocated);
                        expect_free_mem -= (1 << i) * PAGE_SIZE;
                        ok = ok
                             && get_free_mem_size_from_buddy(pool)
                                        == expect_free_mem;
                        buddy_free_pages(pool, page);
                        expect_free_mem += (1 << i) * PAGE_SIZE;
                        ok = ok
                             && get_free_mem_size_from_buddy(pool)
                                        == expect_free_mem;
                }
                for (int i = BUDDY_MAX_ORDER - 1; i >= 0; i--) {
                        page = buddy_get_pages(pool, i);
                        BUG_ON(page == NULL);
                        lab_assert(page->order == i && page->allocated);
                        expect_free_mem -= (1 << i) * PAGE_SIZE;
                        ok = ok
                             && get_free_mem_size_from_buddy(pool)
                                        == expect_free_mem;
                        buddy_free_pages(pool, page);
                        expect_free_mem += (1 << i) * PAGE_SIZE;
                        ok = ok
                             && get_free_mem_size_from_buddy(pool)
                                        == expect_free_mem;
                }
                lab_check(ok, "Allocate & free each order");
        }
        {
                bool ok = true;
                u64 expect_free_mem = pool->pool_phys_page_num * PAGE_SIZE;
                struct page *pages[BUDDY_MAX_ORDER];
                for (int i = 0; i < BUDDY_MAX_ORDER; i++) {
                        pages[i] = buddy_get_pages(pool, i);
                        BUG_ON(pages[i] == NULL);
                        lab_assert(pages[i]->order == i);
                        expect_free_mem -= (1 << i) * PAGE_SIZE;
                        ok = ok
                             && get_free_mem_size_from_buddy(pool)
                                        == expect_free_mem;
                }
                for (int i = 0; i < BUDDY_MAX_ORDER; i++) {
                        buddy_free_pages(pool, pages[i]);
                        expect_free_mem += (1 << i) * PAGE_SIZE;
                        ok = ok
                             && get_free_mem_size_from_buddy(pool)
                                        == expect_free_mem;
                }
                lab_check(ok, "Allocate & free all orders");
        }
        {
                bool ok = true;
                u64 expect_free_mem = pool->pool_phys_page_num * PAGE_SIZE;
                struct page *page;
                for (int i = 0; i < pool->pool_phys_page_num; i++) {
                        page = buddy_get_pages(pool, 0);
                        BUG_ON(page == NULL);
                        lab_assert(page->order == 0);
                        expect_free_mem -= PAGE_SIZE;
                        ok = ok
                             && get_free_mem_size_from_buddy(pool)
                                        == expect_free_mem;
                }
                lab_assert(get_free_mem_size_from_buddy(pool) == 0);
                lab_assert(buddy_get_pages(pool, 0) == NULL);
                for (int i = 0; i < pool->pool_phys_page_num; i++) {
                        page = pool->page_metadata + i;
                        lab_assert(page->allocated);
                        buddy_free_pages(pool, page);
                        expect_free_mem += PAGE_SIZE;
                        ok = ok
                             && get_free_mem_size_from_buddy(pool)
                                        == expect_free_mem;
                }
                ok = ok
                     && pool->pool_phys_page_num * PAGE_SIZE == expect_free_mem;
                lab_check(ok, "Allocate & free all memory");
        }
        printk("[TEST] Buddy tests finished\n");
}
#endif /* CHCORE_KERNEL_TEST */
