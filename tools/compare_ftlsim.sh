#!/usr/bin/env bash
# Compare the C FTL core (ftl/) with ftl-sim's C++ FTL.
#   1. Same write sequence: the C test dumps its writes as an ftl-sim trace,
#      both replay it after a sequential fill. WAF must match exactly.
#   2. Each with its own random generator (fill + 2x warmup + 4x measured).
# Usage: FTLSIM=/path/to/ftlsim tools/compare_ftlsim.sh
set -euo pipefail
cd "$(dirname "$0")/.."

FTLSIM=${FTLSIM:-../FTL/build/ftlsim}
BLOCKS=${BLOCKS:-1024}
PPB=${PPB:-256}
BIN=build/host/host_test_${BLOCKS}x${PPB}
TRACE=build/host/trace.csv

mkdir -p build/host
gcc -std=c11 -O2 -Wall -Wextra -Iftl -DFTL_BLOCKS=$BLOCKS -DFTL_PPB=$PPB \
    ftl/ftl_core.c ftl/nand_sim.c ftl/test/host_test.c -o "$BIN"

waf() { grep -o '"waf": *[0-9.]*' | grep -o '[0-9.]*$'; }
diff_pct() { awk -v a="$1" -v b="$2" 'BEGIN { printf "%+.2f%%", (a - b) / b * 100 }'; }

echo "geometry: $BLOCKS blocks x $PPB pages, greedy GC, threshold 3 free blocks"
echo
echo "1) same write sequence (uniform random, 4x capacity after fill)"
printf "%-6s %-10s %-10s %-7s\n" op c_waf ftlsim diff
for op in 0.07 0.14 0.28; do
  c=$("$BIN" --op=$op --warmup=0 --writes=4 --dump-trace=$TRACE | waf)
  s=$("$FTLSIM" --trace=$TRACE --op=$op --fill=1 --blocks=$BLOCKS --ppb=$PPB | waf)
  printf "%-6s %-10s %-10s %-7s\n" $op "$c" "$s" "$(diff_pct "$c" "$s")"
done
rm -f $TRACE

echo
echo "2) independent random generators"
printf "%-8s %-5s %-5s %-10s %-10s %-7s\n" workload op seed c_waf ftlsim diff
for w in seq uniform; do
  for op in 0.07 0.14 0.28; do
    for seed in 1 2 3; do
      c=$("$BIN" --workload=$w --op=$op --seed=$seed | waf)
      s=$("$FTLSIM" --workload=$w --op=$op --seed=$seed --blocks=$BLOCKS --ppb=$PPB | waf)
      printf "%-8s %-5s %-5s %-10s %-10s %-7s\n" $w $op $seed "$c" "$s" "$(diff_pct "$c" "$s")"
      [ "$w" = seq ] && break
    done
  done
done
