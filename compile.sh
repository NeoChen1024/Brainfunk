#!/usr/bin/env sh
set -e
CC="${CC:-cc}"
CFLAGS="-Ofast -pipe -static"
FILE="$(mktemp)"
SRC="$1"
OUT="$2"

msg_echo()
{
	printf "\e[44m>>\e[0m \e[33m%s\e[0m\n" "${1}"
}

if [ $# -lt 2 ]; then
	echo "${0}: <mode> <infile> <outfile>"
	exit 4
fi

msg_echo "$SRC"
./brainfunk -m bfc -f "$SRC" -o "$FILE"
"$CC" -x c $CFLAGS "$FILE" -o "$OUT"

rm "$FILE"
