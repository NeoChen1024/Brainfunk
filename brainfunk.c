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

#define DELIM_CHARS	80
#define DELIM_CHAR	'='

brainfunk_t cpu;
FILE *input;
FILE *output;
char *code=NULL;
int debug = NODEBUG;
int input_opened=FALSE;
int output_opened=FALSE;

enum mode_enum
{
	MODE_BF,
	MODE_BITCODE,
	MODE_C,
	MODE_VM
};

enum mode_enum mode = MODE_BF;

IO_IN_FUNCTION
{
	int c = getc(stdin);
	if(debug)
		fputs("INPUT: ", stderr);
	if(c != EOF)
		return (data_t)(c & 0xFF);
	else
		return 0;
}

IO_OUT_FUNCTION
{
	if(debug)
		fprintf(stderr, "OUTPUT: %c\n", (char)data);
	else
		putc((char)data, stdout);
}

void delim(FILE *fp)
{
	int i=0;
	putc('\n', fp);
	for(i = 0; i < DELIM_CHARS; i++)
		putc(DELIM_CHAR, fp);
	putc('\n', fp);
	putc('\n', fp);		/* Extra empty line */
}

void parsearg(int argc, char **argv)
{
	int opt=0;

	while((opt = getopt(argc, argv, "hdm:f:o:")) != -1)
	{
		switch(opt)
		{
			case 'm':
				if(!strcmp("bf", optarg))
					mode = MODE_BF;
				else if(!strcmp("c", optarg))
					mode = MODE_C;
				else if(!strcmp("bitcode", optarg))
					mode = MODE_BITCODE;
				else if(!strcmp("vm", optarg))
					mode = MODE_VM;
				else
					panic("?INVAILD_MODE");
				break;
			case 'f':
				if(!input_opened)
				{
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
					input_opened=TRUE;
				}
				break;
			case 'o':
				if(!output_opened)
				{
					if(strcmp("-", optarg) == 0)
						output = stdout;
					else
					{
						if((output = fopen(optarg, "w+")) == NULL)
						{
							perror(optarg);
							exit(8);
						}
					}
					output_opened=TRUE;
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
}

int main(int argc, char **argv)
{
	output = stdout;	/* default to stdout */
	/* Disable Buffering */
	setvbuf(stdin, NULL, _IONBF, 0);
	setvbuf(stdout, NULL, _IONBF, 0);

	parsearg(argc, argv);
	brainfunk_t cpu = brainfunk_init(CODESIZE, MEMSIZE, STACKSIZE, debug);

	switch(mode)
	{
		case MODE_BF:
			if(input_opened == FALSE)
				panic("?INPUT");
			else
				code = brainfunk_readtext(input, CODESIZE);
			bitcode_convert(cpu, code);
			if(debug)
			{
				brainfunk_dumptext(code, stderr);
				delim(stderr);
				bitcode_dump(cpu, FORMAT_PLAIN, stderr);
				delim(stderr);
			}
			brainfunk_execute(cpu);	/* start executing code */
			break;
		case MODE_BITCODE:
		case MODE_C:
			if(input_opened == FALSE)
				panic("?INPUT");
			else
				code = brainfunk_readtext(input, CODESIZE);
			bitcode_convert(cpu, code);
			if(mode == MODE_C)
				bitcode_dump(cpu, FORMAT_C, output);
			else if(mode == MODE_BITCODE)
				bitcode_dump(cpu, FORMAT_PLAIN, output);
			break;
		case MODE_VM:
			if(input_opened == FALSE)
				panic("?INPUT");
			else
				bitcode_read(cpu, input);
			if(debug)
			{
				bitcode_dump(cpu, FORMAT_PLAIN, stderr);
				delim(stderr);
			}
			brainfunk_execute(cpu);	/* start executing code */
			break;
		default:
			break;
	}
	brainfunk_destroy(&cpu);
}
