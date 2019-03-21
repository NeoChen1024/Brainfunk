#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <signal.h>
#include <string.h>
#include <libbrainfunk.h>

/* This should be enough to run any Brainfuck program */
#define CODESIZE	1048576
#define MEMSIZE		262144
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
	FILE* input = stdin;
	int opt=0;
	int debug = NODEBUG;

	while((opt = getopt(argc, argv, "hdf:")) != -1)
	{
		switch(opt)
		{
			case 'f':
				if(strcmp("-", optarg) == 0)
					input = stdin;
				else
				{
					if((input = fopen(optarg, "r")) == NULL)
					{
						perror(optarg);
						exit(8);
					}
				}
				break;
			case 'd':
				debug = DEBUG;
				break;
			default:
				printf("%s: [-d] [-f file]\n", argv[0]);
				break;
		}
	}

	/* Disable Buffering */
	setvbuf(stdin, NULL, _IONBF, 0);
	setvbuf(stdout, NULL, _IONBF, 0);
	brainfunk_t cpu = brainfunk_init(CODESIZE, MEMSIZE, STACKSIZE, debug);
	bitcode_read(cpu, input);
	brainfunk_execute(cpu);
	brainfunk_destroy(&cpu);
}
