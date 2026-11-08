#!/usr/bin/env sh
set -e
CC="${CC:-cc}"
CFLAGS="${CFLAGS:--O3 -pipe}"
BRAINFUNK="${BRAINFUNK:-$(dirname "$0")/build/bin/brainfunk}"
SRC="$1"
OUT="$2"

msg_echo()
{
	printf "\e[44m>>\e[0m \e[33m%s\e[0m\n" "${1}"
}

if [ $# -lt 2 ]; then
	echo "${0}: <infile> <outfile>"
	exit 4
fi

msg_echo "$SRC"
"$BRAINFUNK" -m bfc -f "$SRC" -o - | "$CC" -x c $CFLAGS - -o "$OUT"
