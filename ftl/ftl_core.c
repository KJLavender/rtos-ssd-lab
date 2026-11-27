#include "ftl_core.h"

enum { BLK_FREE, BLK_OPEN, BLK_CLOSED, BLK_VICTIM };

static uint32_t l2p[FTL_PHYS_PAGES];    /* lpn -> ppn */
static uint32_t p2l[FTL_PHYS_PAGES];    /* ppn -> lpn, FTL_NONE if not live */
static uint16_t valid[FTL_BLOCKS];
static uint8_t blk_state[FTL_BLOCKS];

/* Free blocks, oldest erased first (same order as ftl-sim's deque). */
static uint16_t free_q[FTL_BLOCKS];
static uint32_t free_head, free_count;

static uint32_t frontier = FTL_NONE;    /* open block */
static uint32_t frontier_next;          /* next page offset in it */
static uint32_t n_logical;
static struct ftl_stats stats;

static void free_push(uint32_t b)
{
    free_q[(free_head + free_count) % FTL_BLOCKS] = (uint16_t)b;
    free_count++;
}

static uint32_t free_pop(void)
{
    uint32_t b = free_q[free_head];
    free_head = (free_head + 1) % FTL_BLOCKS;
    free_count--;
    return b;
}

void ftl_init(uint32_t logical_pages)
{
    n_logical = logical_pages;
    for (uint32_t i = 0; i < FTL_PHYS_PAGES; i++) {
        l2p[i] = FTL_NONE;
        p2l[i] = FTL_NONE;
    }
    free_head = free_count = 0;
    for (uint32_t b = 0; b < FTL_BLOCKS; b++) {
        valid[b] = 0;
        blk_state[b] = BLK_FREE;
        free_push(b);
    }
    frontier = FTL_NONE;
    frontier_next = 0;
    ftl_reset_stats();
}

uint32_t ftl_logical_pages(void) { return n_logical; }
uint32_t ftl_free_blocks(void) { return free_count; }
const struct ftl_stats *ftl_stats(void) { return &stats; }

void ftl_reset_stats(void)
{
    stats = (struct ftl_stats){0};
}

uint32_t ftl_lookup(uint32_t lpn)
{
    return lpn < n_logical ? l2p[lpn] : FTL_NONE;
}

static void invalidate(uint32_t ppn)
{
    p2l[ppn] = FTL_NONE;
    valid[ppn / FTL_PPB]--;
}

void ftl_unmap(uint32_t lpn)
{
    uint32_t old = l2p[lpn];
    if (old != FTL_NONE) {
        invalidate(old);
        l2p[lpn] = FTL_NONE;
    }
}

int ftl_host_blocked(uint32_t reserve)
{
    return frontier == FTL_NONE && free_count < reserve;
}

/* Next page at the frontier, opening a free block when needed. */
static uint32_t append(uint32_t lpn)
{
    if (frontier == FTL_NONE) {
        if (free_count == 0)
            return FTL_NONE;
        frontier = free_pop();
        frontier_next = 0;
        blk_state[frontier] = BLK_OPEN;
    }
    uint32_t ppn = frontier * FTL_PPB + frontier_next;
    p2l[ppn] = lpn;
    l2p[lpn] = ppn;
    valid[frontier]++;
    stats.nand_writes++;
    if (++frontier_next == FTL_PPB) {
        blk_state[frontier] = BLK_CLOSED;
        frontier = FTL_NONE;
    }
    return ppn;
}

uint32_t ftl_host_alloc(uint32_t lpn)
{
    if (lpn >= n_logical)
        return FTL_NONE;
    if (frontier == FTL_NONE && free_count == 0)
        return FTL_NONE;
    ftl_unmap(lpn);
    stats.host_writes++;
    return append(lpn);
}

uint32_t ftl_gc_pick_victim(void)
{
    uint32_t best = FTL_NONE, best_valid = FTL_PPB + 1;
    for (uint32_t b = 0; b < FTL_BLOCKS; b++) {
        if (blk_state[b] != BLK_CLOSED)
            continue;
        if (valid[b] < best_valid) {
            best_valid = valid[b];
            best = b;
            if (best_valid == 0)
                break;
        }
    }
    if (best != FTL_NONE) {
        blk_state[best] = BLK_VICTIM;
        stats.gc_victims++;
    }
    return best;
}

uint32_t ftl_block_valid(uint32_t block) { return valid[block]; }
uint32_t ftl_page_owner(uint32_t ppn) { return p2l[ppn]; }

uint32_t ftl_gc_relocate(uint32_t lpn, uint32_t src)
{
    if (lpn >= n_logical || l2p[lpn] != src)
        return FTL_NONE;
    invalidate(src);
    stats.gc_copies++;
    return append(lpn);
}

void ftl_block_erased(uint32_t block)
{
    blk_state[block] = BLK_FREE;
    free_push(block);
    stats.erases++;
}

uint32_t ftl_check(void)
{
    uint32_t problems = 0;
    uint16_t count[FTL_BLOCKS] = {0};

    for (uint32_t lpn = 0; lpn < n_logical; lpn++) {
        uint32_t ppn = l2p[lpn];
        if (ppn == FTL_NONE)
            continue;
        if (ppn >= FTL_PHYS_PAGES || p2l[ppn] != lpn)
            problems++;
        else
            count[ppn / FTL_PPB]++;
    }
    for (uint32_t ppn = 0; ppn < FTL_PHYS_PAGES; ppn++) {
        uint32_t lpn = p2l[ppn];
        if (lpn != FTL_NONE && (lpn >= n_logical || l2p[lpn] != ppn))
            problems++;
    }
    uint32_t n_free = 0;
    for (uint32_t b = 0; b < FTL_BLOCKS; b++) {
        if (count[b] != valid[b])
            problems++;
        if (blk_state[b] == BLK_FREE) {
            n_free++;
            if (valid[b] != 0)
                problems++;
        }
    }
    if (n_free != free_count)
        problems++;
    return problems;
}
