#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <signal.h>
#include <libbrainfunk.h>

#define CODESIZE	4096
#define MEMSIZE		4096
#define STACKSIZE	4096

brainfunk_t cpu;

data_t io_in(void)
{
	return (data_t)(getc(stdout) & 0xFF);
}

void io_out(data_t data)
{
	putc((char)data, stdout);
}

int main(int argc, char **argv)
{
	brainfunk_t cpu = brainfunk_init(CODESIZE, MEMSIZE, STACKSIZE, DEBUG);
	bitcode_read(cpu, stdin);
	brainfunk_execute(cpu);
	brainfunk_destroy(&cpu);
}
