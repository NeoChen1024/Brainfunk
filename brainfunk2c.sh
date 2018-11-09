#!/usr/bin/env sh
set -e
CC="cc"
CFLAGS="-O2 -finline -fPIE -pipe -g3 -I."

echo_cmd()
{
	echo "${@}" >&2
	"${@}"
}

if [ $# -lt 2 ]
then
	echo '?ARG'
	exit 4
fi

echo_cmd ./bf2c -f "$1" | echo_cmd $CC $CFLAGS -o "$2" -x c -
