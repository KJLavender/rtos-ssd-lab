/*
 * NAND array model: each page stores a 32-bit data word plus its LPN in the
 * spare (OOB) area. Enforces the rules real NAND has:
 *   - a page can only be programmed once between erases
 *   - pages inside a block must be programmed in order
 *   - erase works on whole blocks
 * Breaking a rule bumps nand_violations(); the FTL must keep it at 0.
 * No timing here: the firmware's NAND task adds the delays.
 */
#ifndef NAND_SIM_H
#define NAND_SIM_H

#include <stdint.h>
#include "ftl_core.h"

void nand_init(void);
void nand_program(uint32_t ppn, uint32_t lpn, uint32_t data);
/* Returns 0 on success, -1 if the page was never programmed. */
int nand_read(uint32_t ppn, uint32_t *lpn, uint32_t *data);
void nand_erase(uint32_t block);

uint32_t nand_violations(void);
uint32_t nand_programs(void);
uint32_t nand_erase_count(uint32_t block);

#endif
