#include "nand_sim.h"

static uint32_t page_data[FTL_PHYS_PAGES];
static uint32_t page_oob[FTL_PHYS_PAGES];     /* LPN, FTL_NONE = erased */
static uint16_t next_page[FTL_BLOCKS];
static uint32_t erase_cnt[FTL_BLOCKS];
static uint32_t violations, programs;

void nand_init(void)
{
    for (uint32_t i = 0; i < FTL_PHYS_PAGES; i++) {
        page_oob[i] = FTL_NONE;
        page_data[i] = 0;
    }
    for (uint32_t b = 0; b < FTL_BLOCKS; b++) {
        next_page[b] = 0;
        erase_cnt[b] = 0;
    }
    violations = programs = 0;
}

void nand_program(uint32_t ppn, uint32_t lpn, uint32_t data)
{
    uint32_t b = ppn / FTL_PPB, off = ppn % FTL_PPB;
    if (ppn >= FTL_PHYS_PAGES || page_oob[ppn] != FTL_NONE || off != next_page[b]) {
        violations++;
        return;
    }
    page_oob[ppn] = lpn;
    page_data[ppn] = data;
    next_page[b]++;
    programs++;
}

int nand_read(uint32_t ppn, uint32_t *lpn, uint32_t *data)
{
    if (ppn >= FTL_PHYS_PAGES || page_oob[ppn] == FTL_NONE) {
        violations++;
        return -1;
    }
    *lpn = page_oob[ppn];
    *data = page_data[ppn];
    return 0;
}

void nand_erase(uint32_t block)
{
    for (uint32_t i = 0; i < FTL_PPB; i++)
        page_oob[block * FTL_PPB + i] = FTL_NONE;
    next_page[block] = 0;
    erase_cnt[block]++;
}

uint32_t nand_violations(void) { return violations; }
uint32_t nand_programs(void) { return programs; }
uint32_t nand_erase_count(uint32_t block) { return erase_cnt[block]; }
