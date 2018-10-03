#!/usr/bin/env sh
set -e

test_echo()
{
	printf "\033[44m==>>\033[0m \033[1;33m%s\033[0m\t\t\033[1;32m%s\033[0m\n" "$1" "$2"
}

msg_echo()
{
	printf "\033[44m==>>\033[0m \033[1;33m%s\033[0m\n" "$1"
}

response()
{
	printf "\033[30;43m<<==\033[0m \033[1;32m%s\033[0m\n" "$1"
}
msg_echo "Testing Basic Loop, see if it coredumps" "-[>+.<-]"
( ./brainfunk -c '-[>+<-]' ) && response "PASS"

msg_echo "Hello World Test" '++++++++++[>+++++++>++++++++++>+++>+<<<<-]>++.>+.+++++++..+++.>++.<<+++++++++++++++.>.+++.------.--------.>+.>.'
( ./brainfunk -c '++++++++++[>+++++++>++++++++++>+++>+<<<<-]>++.>+.+++++++..+++.>++.<<+++++++++++++++.>.+++.------.--------.>+.>.' ) && response "PASS"

msg_echo "Downloading More Tests from Internet"
mkdir -p test

cd test
[ -f brainf_progs.tar ] || curl -OJv http://www.kotay.com/sree/bf/brainf_progs.tar
tar -xpf brainf_progs.tar

for i in bench.b long.b fib.b ; do
	msg_echo "Testing ${i%.b}"
	../brainfunk -f $i && response "PASS"
done

[ -f mandelbrot.b ] || curl -OJv http://esoteric.sange.fi/brainfuck/utils/mandelbrot/mandelbrot.b

msg_echo "Testing mandelbrot"
../brainfunk -f mandelbrot.b && response "PASS"

cd ..
