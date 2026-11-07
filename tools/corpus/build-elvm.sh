#!/bin/sh
# Build ELVM's test programs as Brainfuck, with programs/elvm/bf-stack-top.patch.
#
#   tools/corpus/build-elvm.sh WORKDIR
#
# Copies the 3rdparty/ELVM submodule (with its own submodules checked out) to WORKDIR,
# patches the copy and runs `make build-bf` there, so the submodule stays clean. The
# Brainfuck files end up in WORKDIR/out/*.eir.bf, and those that programs/elvm/programs.toml
# lists are installed into programs/elvm/out/ after their blake3 is checked.
#
# ELVM_BF_STACK_TOP moves the stack; the recorded hashes assume the patch's default.
set -eu

repo=$(cd "$(dirname "$0")/../.." && pwd)
work=${1:?usage: build-elvm.sh WORKDIR}

if [ ! -f "$repo/3rdparty/ELVM/8cc/main.c" ]; then
    echo "run: git submodule update --init --recursive 3rdparty/ELVM" >&2
    exit 1
fi

rm -rf "$work"
mkdir -p "$work"
cp -a "$repo/3rdparty/ELVM/." "$work/"
rm -rf "$work/out"
cd "$work"
patch -p1 < "$repo/programs/elvm/bf-stack-top.patch"

# ELVM's Makefile updates its submodules through .git/index, which a copy does not
# have; they are already checked out. Its -Werror also trips over newer compilers.
mkdir -p out
touch out/git_submodule.stamp
make -j"${JOBS:-8}" -o out/git_submodule.stamp COMMONFLAGS="-MMD -MP -O -g -w" build-bf

if [ -f "$repo/programs/elvm/programs.toml" ]; then
    "$repo/tools/corpus/corpus.py" install "$repo/programs/elvm/programs.toml" "$work/out"
fi
