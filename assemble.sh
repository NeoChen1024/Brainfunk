#!/usr/bin/env sh
set -e
SRC="$1"
OPT="${2:-1}"

msg_echo()
{
	echo -e "\e[44m>>\e[0m \e[33m${*}\e[0m" 1>&2
}

if [ $# -lt 1 ]; then
	echo "${0}: <infile>"
	exit 4
fi

msg_echo "$SRC"
./brainfunk -O "$OPT" -m bit -f "$SRC" | ./bit2bin
