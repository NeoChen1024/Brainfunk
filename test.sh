#!/usr/bin/env sh
set -e

if [ -z "${BF}" ]; then
	BF="${PWD}/brainfunk"
fi

msg_echo()
{
	printf "\033[44m==>>\033[0m \033[1;33m%s\033[0m\n" "$1"
}

response()
{
	printf "\033[30;43m<<==\033[0m \033[1;32m%s\033[0m\n" "$1"
}

fetch()
{
	local url="$1"
	if [ ! -f "${url##*/}" ]; then
		msg_echo "Fetching $url"
		curl -OJv "$url"
	fi
}

test_from_url()
{
	local url="$1"
	local file="${1##*/}"
	fetch "$url"
	msg_echo "Testing $file"

	${BF} -f "$file" && response "PASS" || response "FAIL"
}


msg_echo "Testing Basic Loop, see if it coredumps" "-[>+<-]"
( ${BF} -c '-[>+<-]' ) && response "PASS" || response "FAIL"

msg_echo "Hello World Test"
( ${BF} -c '++++++++++[>+++++++>++++++++++>+++>+<<<<-]>++.>+.+++++++..+++.>++.<<+++++++++++++++.>.+++.------.--------.>+.>.' ) && response "PASS" || response "FAIL"

msg_echo "Downloading More Tests from Internet"
mkdir -p test

cd test
fetch http://www.kotay.com/sree/bf/brainf_progs.tar
tar -xpf brainf_progs.tar

for i in bench.b long.b; do
	msg_echo "Testing ${i%.b}"
	${BF} -f $i && response "PASS" || response "FAIL"
done

test_from_url https://raw.githubusercontent.com/pablojorge/brainfuck/master/programs/sierpinski.bf
