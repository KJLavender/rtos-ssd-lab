/*
 * Runs the C FTL core on Linux, the same way ftl-sim runs its C++ FTL:
 * sequential fill, warmup writes, then measured writes; GC runs inside the
 * write when the frontier needs a block and fewer than FTL_GC_THRESHOLD
 * blocks are free.
 *
 *   host_test [--workload=seq|uniform] [--op=0.07] [--warmup=2] [--writes=4]
 *             [--seed=1]
 *
 * Prints one JSON line. Every read-back is checked against a shadow copy.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ftl_core.h"
#include "nand_sim.h"

static uint32_t shadow[FTL_PHYS_PAGES];   /* latest data written per lpn */
static uint32_t data_seq;
static uint64_t rng_state;

static uint64_t rng_next(void)
{
    /* xorshift64* */
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return rng_state * 0x2545F4914F6CDD1DULL;
}

static void gc_collect_one(void)
{
    uint32_t victim = ftl_gc_pick_victim();
    if (victim == FTL_NONE) {
        fprintf(stderr, "GC found no victim\n");
        exit(1);
    }
    for (uint32_t i = 0; i < FTL_PPB; i++) {
        uint32_t src = victim * FTL_PPB + i;
        uint32_t lpn = ftl_page_owner(src);
        if (lpn == FTL_NONE)
            continue;
        uint32_t oob, data;
        nand_read(src, &oob, &data);
        uint32_t dst = ftl_gc_relocate(lpn, src);
        nand_program(dst, lpn, data);
    }
    nand_erase(victim);
    ftl_block_erased(victim);
}

static void host_write(uint32_t lpn)
{
    /* Like ftl-sim: invalidate first so this write's GC never copies the
     * page that is about to be overwritten. */
    ftl_unmap(lpn);
    /* Once triggered, GC runs until the free pool is back at the threshold,
     * even if GC itself opened a new frontier block on the way. */
    if (ftl_host_blocked(FTL_GC_THRESHOLD))
        while (ftl_free_blocks() < FTL_GC_THRESHOLD)
            gc_collect_one();
    uint32_t ppn = ftl_host_alloc(lpn);
    if (ppn == FTL_NONE) {
        fprintf(stderr, "out of space\n");
        exit(1);
    }
    shadow[lpn] = ++data_seq;
    nand_program(ppn, lpn, shadow[lpn]);
}

static const char *arg(int argc, char **argv, const char *name, const char *def)
{
    size_t n = strlen(name);
    for (int i = 1; i < argc; i++)
        if (strncmp(argv[i], name, n) == 0 && argv[i][n] == '=')
            return argv[i] + n + 1;
    return def;
}

int main(int argc, char **argv)
{
    const char *workload = arg(argc, argv, "--workload", "uniform");
    double op = atof(arg(argc, argv, "--op", "0.07"));
    double warmup = atof(arg(argc, argv, "--warmup", "2"));
    double writes = atof(arg(argc, argv, "--writes", "4"));
    rng_state = strtoull(arg(argc, argv, "--seed", "1"), NULL, 10) * 0x9E3779B97F4A7C15ULL + 1;
    int seq = strcmp(workload, "seq") == 0;
    const char *dump = arg(argc, argv, "--dump-trace", NULL);   /* ftl-sim CSV of the measured writes */
    FILE *trace = dump ? fopen(dump, "w") : NULL;

    uint32_t logical = (uint32_t)(FTL_PHYS_PAGES / (1.0 + op));
    if (logical + (FTL_GC_THRESHOLD + 1) * FTL_PPB > FTL_PHYS_PAGES) {
        fprintf(stderr, "OP too small\n");
        return 1;
    }
    ftl_init(logical);
    nand_init();

    for (uint32_t lpn = 0; lpn < logical; lpn++)
        host_write(lpn);

    uint64_t n_warm = (uint64_t)(warmup * logical), n_meas = (uint64_t)(writes * logical);
    uint64_t i = 0;
    for (; i < n_warm; i++)
        host_write(seq ? (uint32_t)(i % logical) : (uint32_t)(rng_next() % logical));
    ftl_reset_stats();
    for (uint64_t k = 0; k < n_meas; k++, i++) {
        uint32_t lpn = seq ? (uint32_t)(i % logical) : (uint32_t)(rng_next() % logical);
        if (trace)
            fprintf(trace, "%llu,W,%llu,4096\n", (unsigned long long)k,
                    (unsigned long long)lpn * 4096);
        host_write(lpn);
    }
    if (trace)
        fclose(trace);

    uint32_t read_errors = 0;
    for (uint32_t lpn = 0; lpn < logical; lpn++) {
        uint32_t oob, data;
        if (nand_read(ftl_lookup(lpn), &oob, &data) != 0 || oob != lpn || data != shadow[lpn])
            read_errors++;
    }

    const struct ftl_stats *s = ftl_stats();
    printf("{\"workload\":\"%s\",\"blocks\":%u,\"ppb\":%u,\"op\":%.3f,\"logical_pages\":%u,"
           "\"waf\":%.5f,\"gc_victims\":%u,\"avg_valid_per_victim\":%.2f,"
           "\"read_errors\":%u,\"nand_violations\":%u,\"check_problems\":%u}\n",
           workload, FTL_BLOCKS, FTL_PPB, op, logical,
           (double)s->nand_writes / s->host_writes, s->gc_victims,
           s->gc_victims ? (double)s->gc_copies / s->gc_victims : 0.0,
           read_errors, nand_violations(), ftl_check());
    return (read_errors || nand_violations() || ftl_check()) ? 1 : 0;
}
