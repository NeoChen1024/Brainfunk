#!/usr/bin/env sh
set -ex
CC="${CC:-cc}"
CFLAGS="-Ofast -flto -march=native -pipe -Wl,-O1 -Wl,--as-needed -DEXPECT_MACRO -pipe -fPIC -fPIE -I. -g -std=c99 -pedantic -D_POSIX_C_SOURCE=2 -Wall -Wextra -static -std=c99"
FILE="$(mktemp)"
SRC="$1"
OUT="$2"

msg_echo()
{
	echo -e "\e[44m>>\e[0m \e[33m${*}\e[0m"
}

if [ $# -lt 2 ]; then
	echo "${0}: <mode> <infile> <outfile>"
	exit 4
fi

msg_echo "$SRC"
./brainfunk -m bfc -f "$SRC" -o "$FILE"
"$CC" -x c $CFLAGS "$FILE" -o "$OUT"

rm "$FILE"
