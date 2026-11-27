/*
 * Page-mapping FTL core in plain C: L2P table, per-block valid counts, a free
 * block FIFO, one write frontier and greedy victim selection. All storage is
 * fixed-size arrays, no malloc.
 *
 * The core only does bookkeeping. The caller moves the actual data with
 * nand_sim.h, so the same code runs synchronously in the host test and as
 * separate tasks in the RTOS firmware.
 *
 * Not thread-safe: the firmware serialises calls with a mutex.
 */
#ifndef FTL_CORE_H
#define FTL_CORE_H

#include <stdint.h>

#ifndef FTL_BLOCKS
#define FTL_BLOCKS      128
#endif
#ifndef FTL_PPB
#define FTL_PPB         64          /* pages per block */
#endif
#define FTL_PHYS_PAGES  ((uint32_t)FTL_BLOCKS * FTL_PPB)

#define FTL_NONE        0xFFFFFFFFu

/* Same default as ftl-sim with one write stream: keep 3 free blocks. */
#ifndef FTL_GC_THRESHOLD
#define FTL_GC_THRESHOLD 3
#endif

struct ftl_stats {
    uint32_t host_writes;   /* pages written by the host */
    uint32_t nand_writes;   /* host + GC programs */
    uint32_t gc_copies;
    uint32_t gc_victims;
    uint32_t erases;
};

/* logical_pages <= FTL_PHYS_PAGES - (FTL_GC_THRESHOLD + 1) * FTL_PPB */
void ftl_init(uint32_t logical_pages);
uint32_t ftl_logical_pages(void);
uint32_t ftl_free_blocks(void);
const struct ftl_stats *ftl_stats(void);
void ftl_reset_stats(void);

/* Current physical page of lpn, or FTL_NONE if never written. */
uint32_t ftl_lookup(uint32_t lpn);

/* Drop the mapping of lpn (its old page becomes invalid). */
void ftl_unmap(uint32_t lpn);

/* 1 if the frontier is full and opening a new block would leave fewer than
 * `reserve` free blocks, i.e. a host write has to wait for GC. */
int ftl_host_blocked(uint32_t reserve);

/* Allocate the next page for a host write of lpn and point lpn at it; the old
 * copy, if any, becomes invalid. Caller programs the returned page.
 * Returns FTL_NONE if no block can be opened. */
uint32_t ftl_host_alloc(uint32_t lpn);

/* ---- garbage collection ---- */

/* Greedy: the closed block with the fewest valid pages. Marks it as being
 * collected so it is not picked twice. Returns FTL_NONE if none. */
uint32_t ftl_gc_pick_victim(void);
uint32_t ftl_block_valid(uint32_t block);

/* Owner of a physical page if the page still holds live data, else FTL_NONE. */
uint32_t ftl_page_owner(uint32_t ppn);

/* Move lpn from src to a newly allocated page (GC may use the reserve).
 * Returns the destination, or FTL_NONE if src is no longer lpn's live copy
 * (the host overwrote it meanwhile) - then nothing is moved. */
uint32_t ftl_gc_relocate(uint32_t lpn, uint32_t src);

/* Victim has no valid pages left and the caller has erased it. */
void ftl_block_erased(uint32_t block);

/* Full consistency check of L2P vs. reverse map and valid counts.
 * Returns the number of problems found (0 = consistent). */
uint32_t ftl_check(void);

#endif
