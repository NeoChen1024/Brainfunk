#!/usr/bin/env sh
# Runs every SymbiYosys task for the core.  bmc and multiplier check the properties, cover
# checks they are not vacuous, latency bounds every instruction except F and IO.
set -eu
root="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$root/build/formal"
cd "$root/formal"
for task in cover multiplier latency bmc; do
    sby -f --prefix "../build/formal/$task" core.sby "$task" >"../build/formal/$task.log" 2>&1 || {
        echo "formal task $task failed; see build/formal/$task.log" >&2
        exit 1
    }
    echo "formal $task: pass"
done
