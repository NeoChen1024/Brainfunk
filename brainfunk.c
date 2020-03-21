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
FILE* input;
char *code=NULL;
int debug = NODEBUG;
int code_loaded=FALSE;

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

	while((opt = getopt(argc, argv, "hdf:c:")) != -1)
	{
		switch(opt)
		{
			case 'f':
				if(!code_loaded)
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
					code = brainfunk_readtext(input, CODESIZE);
					code_loaded=TRUE;
				}
				break;
			case 'c':
				if(!code_loaded)
				{
					code = calloc(strlen(optarg) + 1, sizeof(char));
					strcpy(code, optarg);
					code_loaded=TRUE;
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
	/* Disable Buffering */
	setvbuf(stdin, NULL, _IONBF, 0);
	setvbuf(stdout, NULL, _IONBF, 0);

	parsearg(argc, argv);
	brainfunk_t cpu = brainfunk_init(CODESIZE, MEMSIZE, STACKSIZE, debug);

	if(code_loaded == FALSE)
		panic("?CODE");

	bitcode_convert(cpu, code);
	if(debug)
	{
		brainfunk_dumptext(code, stderr);
		delim(stderr);
		bitcode_dump(cpu, stderr);
		delim(stderr);
	}

	brainfunk_execute(cpu);	/* start executing code */
	brainfunk_destroy(&cpu);
}
