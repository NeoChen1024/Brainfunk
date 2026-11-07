#!/usr/bin/env sh
# Compares execution statistics across optimization levels.
#
#   sh tests/stat-compare.sh program.b...
#
# Programs must halt; they read end-of-input from stdin. With no arguments, runs
# eight classic programs from programs/ and the 3rdparty/ submodules, and reports
# any that are absent. For a wider set, pass a list from the corpus tool:
#
#   sh tests/stat-compare.sh $(tools/corpus/corpus.py list --role workload \
#       --halts always --cell-bits 8 --unique)
set -eu

BRAINFUNK="${BRAINFUNK:-./brainfunk}"

if [ "$#" -eq 0 ]; then
    for program in \
        3rdparty/bfcorpus/cwfitzgerald/hello.b \
        programs/brainfuck.org/squares.b \
        3rdparty/bfcorpus/cwfitzgerald/bench.b \
        3rdparty/bfcorpus/cwfitzgerald/bottles.b \
        3rdparty/bfcorpus/nyyrikki/triangle.bf \
        3rdparty/bfcorpus/copy/third_party/hanoi.bf \
        3rdparty/rdebath-Brainfuck/testing/Long.b \
        3rdparty/rdebath-Brainfuck/testing/Mandelbrot.b
    do
        if [ -f "$program" ]; then
            set -- "$@" "$program"
        else
            echo "SKIP: $program not found" >&2
        fi
    done
fi

# Highest level, from the usage text.
max_level="$("$BRAINFUNK" -h 2>&1 | sed -n 's/.*level, 0 to \([0-9]*\).*/\1/p')"

printf 'program\tlevel\tstatic\tstatic_M\tdynamic\tdyn%%\tdynamic_M\tM%%\treads\treads%%\twrites\twrites%%\n'

for program in "$@"; do
    level=0
    while [ "$level" -le "$max_level" ]; do
        "$BRAINFUNK" -O"$level" -m stat -f "$program" 2>&1 >/dev/null </dev/null |
            awk -F'\t' -v program="$(basename "$program")" -v level="$level" '
                $1 == "total"         { static_total = $2; dynamic_total = $3 }
                $1 == "M"             { static_m = $2; dynamic_m = $3 }
                $1 == "memory_reads"  { reads = $2 }
                $1 == "memory_writes" { writes = $2 }
                END {
                    printf "%s\t%s\t%s\t%s\t%s\t\t%s\t\t%s\t\t%s\t\n", program, level,
                        static_total, static_m, dynamic_total, dynamic_m, reads, writes
                }'
        level=$((level + 1))
    done
done |
    # Percentages are relative to the first level of each program.
    awk -F'\t' -v OFS='\t' '
        function change(value, reference) {
            return reference == 0 ? "-" : sprintf("%+.1f", (value / reference - 1) * 100)
        }
        $2 == 0 { dynamic0 = $5; m0 = $7; reads0 = $9; writes0 = $11 }
        {
            print $1, $2, $3, $4, $5, change($5, dynamic0), $7, change($7, m0),
                  $9, change($9, reads0), $11, change($11, writes0)
        }'
