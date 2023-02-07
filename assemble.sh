#!/usr/bin/env sh
set -ex
SRC="$1"

msg_echo()
{
	echo -e "\e[44m>>\e[0m \e[33m${*}\e[0m"
}

if [ $# -lt 1 ]; then
	echo "${0}: <infile>"
	exit 4
fi

msg_echo "$SRC"
./brainfunk -m bit -f "$SRC" | ./bit2bin
