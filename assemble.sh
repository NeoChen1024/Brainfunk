#!/usr/bin/env sh
set -e
SRC="$1"

msg_echo()
{
	printf "\e[44m>>\e[0m \e[33m%s\e[0m\n" "${*}" 1>&2
}

if [ $# -lt 1 ]; then
	echo "${0}: <infile> [<opt level>]" 1>&2
	exit 4
fi

msg_echo "$SRC"
./brainfunk -m bit -f "$SRC" | ./bit2bin
