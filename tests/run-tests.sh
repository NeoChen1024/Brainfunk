#!/usr/bin/env sh
set -eu

CXX="${CXX:-c++}"
CC="${CC:-cc}"
CLANG="${CLANG:-clang}"
LLVM_AS="${LLVM_AS:-llvm-as}"
LLVM_OPT="${LLVM_OPT:-opt}"

"$CXX" -std=c++20 -O2 -Wall -Wextra -pedantic -I. \
    tests/test-libbrainfunk.cpp libbrainfunk.cpp optimizer.cpp llvm_codegen.cpp \
    -o tests/test-libbrainfunk
./tests/test-libbrainfunk

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM

have_llvm=false
if command -v "$CLANG" >/dev/null 2>&1 &&
   command -v "$LLVM_AS" >/dev/null 2>&1 &&
   command -v "$LLVM_OPT" >/dev/null 2>&1; then
    have_llvm=true
fi

OPT_LEVELS="0 1 2 3"

# Every optimization level, through every backend, must match the unoptimized
# interpreter, and must still assemble.
check_codegen()
{
    name="$1"
    source="$2"
    input="$3"

    ./brainfunk -O0 -f "$source" >"$tmpdir/$name.reference" <"$input"

    for level in $OPT_LEVELS; do
        base="$tmpdir/$name.O$level"

        ./brainfunk -O"$level" -f "$source" >"$base.interpreted" <"$input"
        cmp "$tmpdir/$name.reference" "$base.interpreted"

        ./brainfunk -O"$level" -m bit -f "$source" | ./bit2bin >/dev/null 2>&1

        ./brainfunk -O"$level" -m bfc -f "$source" >"$base.c"
        "$CC" -std=c99 -O2 -Wall -Wextra -Werror -pedantic \
            "$base.c" -o "$base.generated"
        "$base.generated" >"$base.compiled" <"$input"
        cmp "$tmpdir/$name.reference" "$base.compiled"

        if [ "$have_llvm" = true ]; then
            ./brainfunk -O"$level" -m llvm -f "$source" >"$base.ll"
            if grep -Eq '^target (triple|datalayout)' "$base.ll"; then
                echo "portable LLVM IR contains target metadata" >&2
                exit 1
            fi
            "$LLVM_AS" "$base.ll" -o "$base.bc"
            "$LLVM_OPT" -passes=verify "$base.ll" -disable-output
            "$CLANG" -std=c99 -O2 -Wall -Wextra -Werror -pedantic \
                -Wno-override-module llvm-ir-wrapper.c "$base.ll" \
                -o "$base.llvm"
            "$base.llvm" >"$base.llvm.out" <"$input"
            cmp "$tmpdir/$name.reference" "$base.llvm.out"
        fi
    done
}

printf '%s' '++++++++[>++++++++<-]>+.+.+.' >"$tmpdir/hello.bf"
: >"$tmpdir/empty.input"
check_codegen hello "$tmpdir/hello.bf" "$tmpdir/empty.input"

printf '%s' ',.,.' >"$tmpdir/io.bf"
printf '%s' 'Z' >"$tmpdir/one-byte.input"
check_codegen io "$tmpdir/io.bf" "$tmpdir/one-byte.input"

printf '%s' ',.' >"$tmpdir/byte-ff.bf"
printf '\377' >"$tmpdir/byte-ff.input"
check_codegen byte-ff "$tmpdir/byte-ff.bf" "$tmpdir/byte-ff.input"

: >"$tmpdir/empty.bf"
check_codegen empty "$tmpdir/empty.bf" "$tmpdir/empty.input"

awk 'BEGIN { printf "+"; for (i = 0; i < 65536; ++i) printf ">"; printf "." }' \
    >"$tmpdir/wrap.bf"
check_codegen wrap "$tmpdir/wrap.bf" "$tmpdir/empty.input"

printf '%s' '+[>]+.' >"$tmpdir/scan.bf"
check_codegen scan "$tmpdir/scan.bf" "$tmpdir/empty.input"

printf '%s' '+++[.-]' >"$tmpdir/loop.bf"
check_codegen loop "$tmpdir/loop.bf" "$tmpdir/empty.input"

printf '%s' '+++[<++>-]<.' >"$tmpdir/backward.bf"
check_codegen backward "$tmpdir/backward.bf" "$tmpdir/empty.input"

# Operands reached through a pending pointer offset.
printf '%s' '+++>+++[->+<]>.<<.' >"$tmpdir/pending-mul.bf"
check_codegen pending-mul "$tmpdir/pending-mul.bf" "$tmpdir/empty.input"

printf '%s' ',>,>,<<.>.>.' >"$tmpdir/pending-io.bf"
printf '%s' 'xyz' >"$tmpdir/three-bytes.input"
check_codegen pending-io "$tmpdir/pending-io.bf" "$tmpdir/three-bytes.input"

printf '%s' '>>>+++[.-]>+<<<<.' >"$tmpdir/pending-test.bf"
check_codegen pending-test "$tmpdir/pending-test.bf" "$tmpdir/empty.input"

printf '%s' '+>+>+>>>+<<<<<<[>]>>+.' >"$tmpdir/pending-scan.bf"
check_codegen pending-scan "$tmpdir/pending-scan.bf" "$tmpdir/empty.input"

printf '%s' '>>+++[->>+<]+<<.' >"$tmpdir/moving-loop.bf"
check_codegen moving-loop "$tmpdir/moving-loop.bf" "$tmpdir/empty.input"

printf '%s' '<<<+.>>>.<<<[-].' >"$tmpdir/pending-negative.bf"
check_codegen pending-negative "$tmpdir/pending-negative.bf" "$tmpdir/empty.input"

# Instruction fusion.
printf '%s' ',[>+>++<<-]>.>.' >"$tmpdir/fused-mulc.bf"
check_codegen fused-mulc "$tmpdir/fused-mulc.bf" "$tmpdir/one-byte.input"
printf '%s' '+++[.>+<-]>.' >"$tmpdir/fused-djn.bf"
check_codegen fused-djn "$tmpdir/fused-djn.bf" "$tmpdir/empty.input"
printf '%s' '+>+>+>+>+<<<<[.>]<.' >"$tmpdir/fused-mjn.bf"
check_codegen fused-mjn "$tmpdir/fused-mjn.bf" "$tmpdir/empty.input"
printf '%s' ',>,>,>,<<<[-]>[-]>[-]>[-]<<<.>.>.>.' >"$tmpdir/fused-srun.bf"
printf '%s' 'abcd' >"$tmpdir/four-bytes.input"
check_codegen fused-srun "$tmpdir/fused-srun.bf" "$tmpdir/four-bytes.input"
printf '%s' ',[>+>++<<-]+++[.>+<-]+>+<[.>]<,>,>,<<[-]>[-]>[-]<<.' >"$tmpdir/fused.bf"
check_codegen fused "$tmpdir/fused.bf" "$tmpdir/four-bytes.input"
for mnemonic in MULC DJN MJN SRUN; do
    grep -q "$mnemonic" "$tmpdir/fused.O3.c"
done

repeat() { awk -v n="$2" -v c="$1" 'BEGIN { for (i = 0; i < n; ++i) printf "%s", c }'; }

# A very long multiply-shaped loop must not exhaust the stack while being matched.
{ printf '[-'; repeat '>+' 200000; repeat '<' 200000; printf ']'; } >"$tmpdir/long-pairs-1.bf"
./brainfunk -m bit -f "$tmpdir/long-pairs-1.bf" >/dev/null
{ printf '['; repeat '>+' 200000; repeat '<' 200000; printf -- '-]'; } >"$tmpdir/long-pairs-2.bf"
./brainfunk -m bit -f "$tmpdir/long-pairs-2.bf" >/dev/null

# Offsets that overflow each field force an early flush.  No real program in the
# corpus reaches these, so they are only covered here.
{ printf '+'; repeat '>' 2047; printf '+'; repeat '>' 1; printf '+.'; repeat '<' 2048; printf '.'; } \
    >"$tmpdir/overflow-addr.bf"
check_codegen overflow-addr "$tmpdir/overflow-addr.bf" "$tmpdir/empty.input"
{ repeat '>' 128; printf '+++[.-]'; repeat '<' 128; printf '+.'; } >"$tmpdir/overflow-test.bf"
check_codegen overflow-test "$tmpdir/overflow-test.bf" "$tmpdir/empty.input"
{ printf '+'; repeat '>' 127; printf '++[->+<]'; repeat '>' 2; printf '.'; } >"$tmpdir/fit-src.bf"
check_codegen fit-src "$tmpdir/fit-src.bf" "$tmpdir/empty.input"
{ printf '+'; repeat '>' 128; printf '++[->+<]'; repeat '>' 1; printf '.'; } >"$tmpdir/overflow-src.bf"
check_codegen overflow-src "$tmpdir/overflow-src.bf" "$tmpdir/empty.input"
{ repeat '>' 100; printf '+'; repeat '>' 524287; printf '.'; } >"$tmpdir/overflow-move.bf"
check_codegen overflow-move "$tmpdir/overflow-move.bf" "$tmpdir/empty.input"

# Real programs from programs/ and the 3rdparty/ submodules, when checked out.
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
        check_codegen "corpus-$(basename "$program")" "$program" "$tmpdir/empty.input"
    else
        echo "SKIP: $program not found" >&2
    fi
done

if [ "$have_llvm" = true ]; then
    for target in \
        i386-unknown-linux-gnu \
        armv7-unknown-linux-gnueabihf \
        aarch64-unknown-linux-gnu \
        riscv64-unknown-linux-gnu \
        powerpc64-unknown-linux-gnu \
        powerpc64le-unknown-linux-gnu \
        s390x-unknown-linux-gnu \
        x86_64-pc-windows-msvc \
        x86_64-apple-darwin \
        arm64-apple-darwin \
        wasm32-unknown-unknown \
        wasm64-unknown-unknown
    do
        "$CLANG" -target "$target" -O2 -Wno-override-module \
            -c "$tmpdir/fused.O3.ll" -o "$tmpdir/fused.$target.o"
    done

    CLANG="$CLANG" CFLAGS='-O2 -pipe' \
        ./compile-llvm-ir.sh "$tmpdir/hello.bf" "$tmpdir/hello.pipeline"
    "$tmpdir/hello.pipeline" >"$tmpdir/hello.pipeline.out"
    cmp "$tmpdir/hello.reference" "$tmpdir/hello.pipeline.out"
else
    echo "LLVM tools not found; skipping LLVM assembly and execution tests" >&2
fi

./brainfunk -m bit -s '++>+++[<+>-].' | ./bit2bin >"$tmpdir/program.bin"
test "$(wc -l <"$tmpdir/program.bin")" -gt 0

check_encoding()
{
    actual="$(printf '0: %s\n' "$1" | ./bit2bin 2>/dev/null)"
    if [ "$actual" != "$2" ]; then
        echo "bit2bin: '$1' encoded as '$actual', expected '$2'" >&2
        exit 1
    fi
}

check_encoding 'X' 00000000
check_encoding 'A 0, 5' 10000005
check_encoding 'A -1, 5' 100fff05
check_encoding 'S 2047, 255' 2007ffff
check_encoding 'MUL 3, 2, 3' 30200303
check_encoding 'MUL -2048, -128, 255' 380800ff
check_encoding 'F -1, 1' 4ff00001
check_encoding 'M -1' 500fffff
check_encoding 'JE 1, -2' 601ffffe
check_encoding 'JN 127, 524287' 77f7ffff
check_encoding 'IO 0, 1' 80000001
check_encoding 'H' 90000000
check_encoding 'MULC 3, 2, 3' a0200303
check_encoding 'DJN 1, -2' b01ffffe
check_encoding 'MJN 1, -2, -3' c01feffd
check_encoding 'MJN -128, 127, 2047' c807f7ff
check_encoding 'SRUN 5, 7, 3' d0300507
check_encoding 'SRUN -1, 0, 255' dfffff00

for rejected in \
    'A 2048, 1' 'A -2049, 1' 'A 0, 256' 'A 0, -1' 'A 5' 'H 1' 'IO 0, 2' \
    'MUL 1, 2' 'MUL 2048, 0, 1' 'MUL 1, 128, 1' 'MUL 1, -129, 1' 'MUL 1, 0, 256' \
    'F 128, 1' 'F 0, 524288' 'M 524288' 'M -524289' \
    'JE 128, 0' 'JN -129, 0' 'JE 0, 524288' 'NOPE 1' \
    'MULC 1, 128, 1' 'DJN 128, 0' 'MJN 1, 128, 0' 'MJN 1, 0, 2048' 'MJN 128, 0, 0' 'MJN 1, 2' \
    'SRUN 0, 0, 0' 'SRUN 0, 0, 256' 'SRUN 0, 0, -1' 'SRUN 2048, 0, 1' 'SRUN 0, 256, 1' 'SRUN 0, 0'
do
    if printf '0: %s\n' "$rejected" | ./bit2bin >/dev/null 2>&1; then
        echo "bit2bin accepted invalid instruction: $rejected" >&2
        exit 1
    fi
done

awk 'BEGIN { printf "+[-"; for (i = 0; i < 3000; ++i) printf ">"; printf "+";
            for (i = 0; i < 3000; ++i) printf "<"; printf "]" }' >"$tmpdir/far-mul.bf"
./brainfunk -m bit -f "$tmpdir/far-mul.bf" | ./bit2bin >/dev/null

./brainfunk -O0 -m stat -s '++[->+<]>.' >/dev/null 2>"$tmpdir/stat.out"
grep -q '^MUL	1	1$' "$tmpdir/stat.out"
grep -q '^memory_reads	4$' "$tmpdir/stat.out"
./brainfunk -m stat -s '+' -o "$tmpdir/stat-file.out"
grep -q '^total	' "$tmpdir/stat-file.out"
sh tests/stat-compare.sh "$tmpdir/hello.bf" >"$tmpdir/compare.out"
grep -q 'hello.bf' "$tmpdir/compare.out"

for invalid_level in 9 x 12 ''; do
    if ./brainfunk -O"$invalid_level" -s '+' >/dev/null 2>&1; then
        echo "brainfunk accepted optimization level '$invalid_level'" >&2
        exit 1
    fi
done

if ./visualbrainfunk -s '+' -t invalid >/dev/null 2>&1; then
    echo "visualbrainfunk accepted an invalid delay" >&2
    exit 1
fi

echo "All tests passed"
