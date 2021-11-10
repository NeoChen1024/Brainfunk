/* ====================================== *\
|* bf.c -- A simple Brainfuck Interpreter *|
|* Neo_Chen				  *|
\* ====================================== */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>

#ifndef SIZEDEF
#define STACKSIZE (1ULL<<12)
#define MEMSIZE (1ULL<<20)
#define CODESIZE (1ULL<<16)
#endif

#define TRUE 1
#define FALSE 0
#define _INLINE	static inline

#define ERR_MSG_ALLOC	("Unable to allocate memory!\n")

typedef uint8_t memory_t;
typedef unsigned int arg_t;

/* init */

static memory_t *memory;
static arg_t ptr=0;
static arg_t *stack;
static char *code;
static arg_t pc=0;

static size_t code_size = CODESIZE;

static int is_code(char c)
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

void panic(char *msg, int condition)
{
	if(condition)
	{
		fputs(msg, stderr);
		exit(EXIT_FAILURE);
	}
}

static void validate_code(char *str)
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
	
	panic("Unmatched Loop!\n", level != 0);
}


// Read Code
static void read_code(FILE* fp)
{
	size_t i = 0;
	int c;
	while((c = getc(fp)) != EOF)
	{
		if(is_code(c))
			code[i++] = (char)c;

		if(i >= code_size)
		{
			panic(ERR_MSG_ALLOC, (code = realloc(code, code_size += 1024)) == NULL);
		}
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

_INLINE	void interprete(char c)
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
			memory[ptr] = (uint8_t)getc(stdin);
			break;
		case '.':
			putc(memory[ptr], stdout);
			break;
		default:
			break;
	}
	pc++;
}

static void help(int argc, char **argv)
{
	printf("Usage: %s [-h] [-f file] [-s code]\n", argv[0]);
}

int main(int argc, char **argv)
{
	/* Init */
	memory	= calloc(MEMSIZE, sizeof(memory_t));
	stack	= calloc(STACKSIZE, sizeof(arg_t));
	code	= calloc(CODESIZE, sizeof(char));

	code[0] = '\0';

	/* Disable Buffering */
	setvbuf(stdin, NULL, _IONBF, 0);
	setvbuf(stdout, NULL, _IONBF, 0);

	panic(ERR_MSG_ALLOC, memory == NULL || stack == NULL || code == NULL);

	/* Parse Argument */
	FILE *corefile;
	int opt;

	if(!(argc >= 2))
	{
		help(argc, argv);
	}
	else
	{
		while((opt = getopt(argc, argv, "hf:s:")) != -1)
		{
			switch(opt)
			{
				case 'f':
					if(strcmp(optarg, "-") == 0)
						read_code(stdin);
					else
					{
						if((corefile = fopen(optarg, "r")) == NULL)
						{
							perror(optarg);
							exit(EXIT_FAILURE);
						}
						read_code(corefile);
					}
					break;
				case 's':
					validate_code(optarg);
					free(code);
					code = malloc(strlen(optarg) + 1);
					panic(ERR_MSG_ALLOC, code == NULL);
					strcpy(code, optarg);
					break;
				case 'h':
					help(argc, argv);
					break;
				default:
					exit(EXIT_FAILURE);
			}
		}
	}

	while(code[pc] != '\0')
		interprete(code[pc]);
	free(code);
	return 0;
}
