#!/usr/bin/env sh
set -eu

CXX="${CXX:-c++}"
CC="${CC:-cc}"
CLANG="${CLANG:-clang}"
LLVM_AS="${LLVM_AS:-llvm-as}"
LLVM_OPT="${LLVM_OPT:-opt}"

"$CXX" -std=c++20 -O2 -Wall -Wextra -pedantic -I. \
    tests/test-libbrainfunk.cpp libbrainfunk.cpp llvm_codegen.cpp \
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

check_codegen()
{
    name="$1"
    source="$2"
    input="$3"

    ./brainfunk -f "$source" >"$tmpdir/$name.interpreted" <"$input"
    ./brainfunk -m bfc -f "$source" >"$tmpdir/$name.c"
    "$CC" -std=c99 -O2 -Wall -Wextra -Werror -pedantic \
        "$tmpdir/$name.c" -o "$tmpdir/$name.generated"
    "$tmpdir/$name.generated" >"$tmpdir/$name.compiled" <"$input"
    cmp "$tmpdir/$name.interpreted" "$tmpdir/$name.compiled"

    if [ "$have_llvm" = true ]; then
        ./brainfunk -m llvm -f "$source" >"$tmpdir/$name.ll"
        if grep -Eq '^target (triple|datalayout)' "$tmpdir/$name.ll"; then
            echo "portable LLVM IR contains target metadata" >&2
            exit 1
        fi
        "$LLVM_AS" "$tmpdir/$name.ll" -o "$tmpdir/$name.bc"
        "$LLVM_OPT" -passes=verify "$tmpdir/$name.ll" -disable-output
        "$CLANG" -std=c99 -O2 -Wall -Wextra -Werror -pedantic \
            -Wno-override-module llvm-ir-wrapper.c "$tmpdir/$name.ll" \
            -o "$tmpdir/$name.llvm"
        "$tmpdir/$name.llvm" >"$tmpdir/$name.llvm.out" <"$input"
        cmp "$tmpdir/$name.interpreted" "$tmpdir/$name.llvm.out"
    fi
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
            -c "$tmpdir/hello.ll" -o "$tmpdir/hello.$target.o"
    done

    CLANG="$CLANG" CFLAGS='-O2 -pipe' \
        ./compile-llvm-ir.sh "$tmpdir/hello.bf" "$tmpdir/hello.pipeline"
    "$tmpdir/hello.pipeline" >"$tmpdir/hello.pipeline.out"
    cmp "$tmpdir/hello.interpreted" "$tmpdir/hello.pipeline.out"
else
    echo "LLVM tools not found; skipping LLVM assembly and execution tests" >&2
fi

./brainfunk -m bit -s '++>+++[<+>-].' | ./bit2bin >"$tmpdir/program.bin"
test "$(wc -l <"$tmpdir/program.bin")" -gt 0

if printf '0: M 524288\n' | ./bit2bin >/dev/null 2>"$tmpdir/oversized.err"; then
    echo "bit2bin accepted an out-of-range offset" >&2
    exit 1
fi

if ./visualbrainfunk -s '+' -t invalid >/dev/null 2>&1; then
    echo "visualbrainfunk accepted an invalid delay" >&2
    exit 1
fi

echo "All tests passed"
