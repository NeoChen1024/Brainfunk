/* ========================================================================== *\
||			      Brainfunk in ANSI C			      ||
||                                 Neo_Chen                                   ||
\* ========================================================================== */

/* ========================================================================== *\
||   This is free and unencumbered software released into the public domain.  ||
||									      ||
||   Anyone is free to copy, modify, publish, use, compile, sell, or	      ||
||   distribute this software, either in source code form or as a compiled    ||
||   binary, for any purpose, commercial or non-commercial, and by any	      ||
||   means.								      ||
||									      ||
||   In jurisdictions that recognize copyright laws, the author or authors    ||
||   of this software dedicate any and all copyright interest in the	      ||
||   software to the public domain. We make this dedication for the benefit   ||
||   of the public at large and to the detriment of our heirs and	      ||
||   successors. We intend this dedication to be an overt act of	      ||
||   relinquishment in perpetuity of all present and future rights to this    ||
||   software under copyright law.					      ||
||									      ||
||   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,	      ||
||   EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF       ||
||   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.   ||
||   IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR        ||
||   OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,    ||
||   ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR    ||
||   OTHER DEALINGS IN THE SOFTWARE.					      ||
||									      ||
||   For more information, please refer to <http://unlicense.org/>            ||
\* ========================================================================== */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <signal.h>
#include <string.h>
#include <libbrainfunk.h>

/* This should be enough to run any Brainfuck program */
#define CODESIZE	(1<<24)
#define MEMSIZE		(1<<20)
#define STACKSIZE	(1<<12)

#define DELIM_CHARS	80
#define DELIM_CHAR	'='

brainfunk_t cpu;
FILE *input;
FILE *output;
char *code=NULL;
int debug = NODEBUG;
int input_opened=FALSE;
int output_opened=FALSE;
int string_input=FALSE;
int compat=NOCOMPAT;	/* don't ignore Brainfuck extensions */

enum mode_enum
{
	MODE_BF,	/* Execute Brainfuck / Brainfunk program */
	MODE_BC,	/* Output bitcode */
	MODE_BFC,	/* Convert Brainfunk to C code for compiling */
	MODE_BCC,	/* Convert bitcode to C for compiling */
	MODE_VM		/* Execute bitcode directly */
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
		fprintf(stdout, "OUTPUT: %c\n", (char)data);
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

	while((opt = getopt(argc, argv, "hdcm:f:o:s:")) != -1)
	{
		switch(opt)
		{
			case 'm':
				if(!strcmp("bf", optarg))
					mode = MODE_BF;
				else if(!strcmp("bfc", optarg))
					mode = MODE_BFC;
				else if(!strcmp("bit", optarg))
					mode = MODE_BC;
				else if(!strcmp("bcc", optarg))
					mode = MODE_BCC;
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
			case 's':
				code = calloc(strlen(optarg) + 1, sizeof(char));
				strcpy(code, optarg);
				string_input = TRUE;
				break;
			case 'd':
				debug = DEBUG;
				break;
			case 'c':
				compat = COMPAT; /* ignore Brainfunk extensions */
				break;
			default:
				printf("%s: [-d] [-m mode] [-c] [-s code] -f file [-o file]\n", argv[0]);
				exit(1);
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

	if(input_opened == FALSE && (string_input == FALSE && mode == MODE_BF))
		panic("?INPUT");
	else
	{
		if(mode == MODE_VM || mode == MODE_BCC)
			bitcode_read(cpu, input);
		else
		{
			if(!string_input)
				code = brainfunk_readtext(input, compat, CODESIZE);
			bitcode_convert(cpu, code);
			if(debug)
			{
				bitcode_dump(cpu, FORMAT_PLAIN, output);
				delim(output);
			}
		}
		if(mode == MODE_BFC || mode == MODE_BCC)
			bitcode_dump(cpu, FORMAT_C, output);
		else if(mode == MODE_BC)
			bitcode_dump(cpu, FORMAT_PLAIN, output);
		else
			brainfunk_execute(cpu);	/* start executing code */
	}
	brainfunk_destroy(&cpu);
}
