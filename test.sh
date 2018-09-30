#!/usr/bin/env bash

msg_echo()
{
	printf "\033[44m==>>\033[0m \033[1;33m%s\033[0m\t\t\033[1;32m%s\033[0m\n" "$1" "$2"
}

response()
{
	printf "\033[30;43m<<==\033[0m \033[1;32m%s\033[0m\n" "$1"
}
msg_echo "Testing Basic Loop, see if it coredumps" "-[>+.<-]"
( echo '-[>+.<-]' | ./brainfunk 2>&1 > /dev/null ) && response "PASS"

msg_echo "Hello World Test" '++++++++++[>+++++++>++++++++++>+++>+<<<<-]>++.>+.+++++++..+++.>++.<<+++++++++++++++.>.+++.------.--------.>+.>.'
( echo '++++++++++[>+++++++>++++++++++>+++>+<<<<-]>++.>+.+++++++..+++.>++.<<+++++++++++++++.>.+++.------.--------.>+.>.' | ./brainfunk 2>&1 > /dev/null ) && response "PASS"
