#!/usr/bin/env sh
set -ex
CC="${CC:-cc}"
CFLAGS="-Ofast -flto -fPIE -pipe -I. -Wall -Wextra -Wno-unused-label -static -std=c99"
FILE="$(mktemp).c"
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
"$CC" $CFLAGS "$FILE" -o "$OUT"

rm "$FILE"
