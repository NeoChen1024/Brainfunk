#!/usr/bin/env sh
set -eu

CLANG="${CLANG:-clang}"
CFLAGS="${CFLAGS:--O3 -pipe -flto}"

if [ "$#" -lt 2 ]; then
    echo "${0}: <infile> <outfile>" >&2
    exit 4
fi

SRC="$1"
OUT="$2"
TMPDIR_PATH="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_PATH"' EXIT HUP INT TERM

msg_echo()
{
    printf '\033[44m>>\033[0m \033[33m%s\033[0m\n' "$1"
}

msg_echo "$SRC"
./brainfunk -m llvm -f "$SRC" -o "$TMPDIR_PATH/program.ll"
# CFLAGS is intentionally word-split so callers can supply multiple compiler flags.
# shellcheck disable=SC2086
"$CLANG" -std=c99 -Wno-override-module $CFLAGS \
    llvm-ir-wrapper.c "$TMPDIR_PATH/program.ll" -o "$OUT"
