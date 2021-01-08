/* ==================================== *
 * Brainfunk -- A Brainf**k Interpreter *
 * Neo_Chen			        *
 * ==================================== */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>

#ifndef SIZEDEF
#define STACKSIZE (1ULL<<12)
#define MEMSIZE (1ULL<<20)
#define CODESIZE (1ULL<<18)
#endif

#define TRUE 1
#define FALSE 0
#define _INLINE	static inline

typedef uint8_t memory_t;
typedef unsigned int arg_t;

/* init */

memory_t *memory;
arg_t ptr=0;
arg_t *stack;
char *code;
arg_t pc=0;

int is_code(char c)
{
	switch(c)
	{
		case '+':
		case '-':
		case '>':
		case '<':
		case '[':
		case ']':
		case '.':
		case ',':
			return TRUE;
		default:
			return FALSE;
	}
}

void validate_code(char *str)
{
	size_t i = 0;
	ssize_t level = 0;

	/* Validate that the '[' and ']'s is matched */
	while(str[i] != '\0')
	{
		switch(str[i++])
		{
			case '[':
				level++;
				break;
			case ']':
				level--;
				break;
		}
	}
	
	if(level != 0)
	{
		fputs("Unmatched Loop!\n", stderr);
		exit(1);
	}
}


// Read Code
void read_code(FILE* fp)
{
	size_t i = 0;
	int c;
	while((c = getc(fp)) != EOF)
	{
		if(i >= CODESIZE)
		{
			fputs("Code too big!\n", stderr);
			exit(1);
		}
		if(is_code(c))
			code[i++] = (char)c;
	}
	code[i] = '\0';

	validate_code(code);
}

_INLINE	void exit_loop(void)
{
	long long int level=0;
	while(code[pc] != '\0')
	{
		if(code[pc] == '[')
			++level;
		else if(code[pc] == ']')
			--level;

		if(level == 0)
			return;

		pc++;
	}
}

_INLINE	void interprete(unsigned char c)
{
	switch(c)
	{
		case '+':
			memory[ptr]++;
			break;
		case '-':
			memory[ptr]--;
			break;
		case '>':
			ptr++;
			ptr %= MEMSIZE;
			break;
		case '<':
			ptr--;
			ptr %= MEMSIZE;
			break;
		case '[':
			if(memory[ptr] == 0)
				exit_loop(); /* Skip everything until reached matching "]" */
			else
				*++stack = pc; /* Push PC */
			break;
		case ']':
			if(memory[ptr] != 0) /* if not equals to 0 */
				pc = *(stack); /* Peek */
			else
				stack--; /* Drop */
			break;
		case ',':
			memory[ptr] = (uint8_t)getchar();
			break;
		case '.':
			putchar(memory[ptr]);
			fflush(NULL);
			break;
		default:
			break;
	}
	pc++;
}

void help(int argc, char **argv)
{
	printf("Usage: %s [-h] [-f file] [-s code] [-d]\n", argv[0]);
}

int main(int argc, char **argv)
{
	/* Init */
	memory	= calloc(MEMSIZE, sizeof(memory_t));
	stack	= calloc(STACKSIZE, sizeof(arg_t));
	code	= calloc(CODESIZE, sizeof(char));

	/* Parse Argument */
	FILE *corefile;
	int opt;

	if(!(argc >= 2))
	{
		help(argc, argv);
	}
	else
	{
		while((opt = getopt(argc, argv, "hdf:s:")) != -1)
		{
			switch(opt)
			{
				case 'f':
					if(strcmp(optarg, "-"))
					{
						if((corefile = fopen(optarg, "r")) == NULL)
						{
							perror(optarg);
							exit(8);
						}
						read_code(corefile);
					}
					else
						read_code(stdin);
					break;
				case 's':
					validate_code(optarg);
					free(code);
					code = malloc(strlen(optarg) + 1);
					strcpy(code, optarg);
					break;
				case 'h':
					help(argc, argv);
					break;
				default:
					exit(1);
					break;

			}
		}
	}

	while(code[pc] != '\0')
		interprete(code[pc]);
	return 0;
}
