#!/bin/sh
# Host self-checks for the station. Nothing here needs ESP-IDF or hardware:
# these are the pieces whose failure is invisible on the board.
#
#     tools/run_selftests.sh
#
# tb_vital_json  -- which JSON keys are emitted and, more importantly, which are
#                   NOT. A fabricated zero reaching the backend is a fabricated
#                   patient reading; nothing downstream can tell.
# lora_budget    -- airtime, slot timing, duty cycle and link budget. A slot that
#                   is too short fails as a timeout indistinguishable from a dead
#                   node, so the numbers behind it are asserted rather than
#                   remembered.
# battery_budget -- runtime per pack, and the gap between the datasheet estimate
#                   and the load the 11-hour proposal claim implies. Prints the
#                   disagreement rather than picking a favourite.
set -e

cd "$(dirname "$0")/.."
CC=${CC:-cc}
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

echo "=== tb_vital_json ==="
$CC -O1 -Wall -Wextra -I main -o "$OUT/tvj" tools/test_vital_json.c main/tb_vital_json.c
"$OUT/tvj"

echo
echo "=== lora_budget ==="
$CC -O2 -Wall -Wextra -o "$OUT/budget" tools/lora_budget.c -lm
"$OUT/budget"

echo
echo "=== battery_budget ==="
$CC -O2 -Wall -Wextra -o "$OUT/batt" tools/battery_budget.c -lm
"$OUT/batt"

echo
echo "all station self-checks passed"
