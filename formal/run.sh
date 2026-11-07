#!/usr/bin/env sh
# Runs every SymbiYosys task for the core, across $JOBS processes at nice 19. bmc, bmc_miss and
# multiplier check the properties, cover checks they are not vacuous, and latency bounds every
# instruction except F and IO.
set -eu
root="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$root/build/formal"
. "$root/tb/parallel.sh"
cd "$root/formal"
for task in cover multiplier latency bmc bmc_miss; do
    echo "sby -f --prefix '../build/formal/$task' core.sby '$task' >'../build/formal/$task.log' 2>&1 && echo 'formal $task: pass' || { echo 'formal task $task failed; see build/formal/$task.log'; exit 1; }"
done | run_jobs "$root/build/formal"
