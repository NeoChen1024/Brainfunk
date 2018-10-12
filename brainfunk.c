/* ==================================== *
 * Brainfunk -- A Brainf**k Interpreter *
 * Neo_Chen			        *
 * ==================================== */

/* Instructions:

	+	++(*ptr)
	-	--(*ptr)
	>	++ptr
	<	--ptr
	[	while(*ptr) {
	]	}
	.	putchar(*ptr)
	,	*ptr = getchar()
*/

/* If "FAST" is defined, will bypass alli runtime checking */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#define DEF_MEMSIZE 262144
#define DEF_CODESIZE 65536
#define DEF_STACKSIZE 1024

#ifdef FAST
#	define MEMSIZE DEF_MEMSIZE
#	define CODESIZE DEF_CODESIZE
#	define STACKSIZE DEF_STACKSIZE
#else
#	define MEMSIZE memsize
#	define CODESIZE codesize
#	define STACKSIZE stacksize
#endif

#define TRUE 1
#define FALSE 0
typedef uint8_t memory_t;
typedef unsigned int stack_type;
typedef char code_t;

/* init */

memory_t *memory;
unsigned int ptr=0;
stack_type *stack;
unsigned int stack_ptr=0;
code_t *code;
unsigned int code_ptr=0;

int debug=0;

unsigned int memsize=DEF_MEMSIZE;
unsigned int codesize=DEF_CODESIZE;
unsigned int stacksize=DEF_STACKSIZE;

void panic(char *msg)
{
	fputs(msg, stderr);
	exit(2);
}

// Read Code
void read_code(FILE* fp)
{
	unsigned int i=0;
	int c=0;
	while((c=getc(fp)) != EOF)
	{
#ifndef FAST
		if(i >= CODESIZE)
			panic("?CODE");
#endif
		code[i++]=(char)c;
	}
}

void push(stack_type *stack, unsigned int *ptr, stack_type content)
{
#ifdef FAST
	if(*ptr >= STACKSIZE)
		panic("?>STACK");
#endif
	stack[++(*ptr)]=content;
}

stack_type pop(stack_type *stack, unsigned int *ptr)
{
#ifdef FAST
	if(*ptr >= STACKSIZE)
		panic("?<STACK");
#endif
	return stack[(*ptr)++];
}

void debug_output(void)
{
	fprintf(stderr, "code=%u:%c\n", code_ptr, code[code_ptr]);
	fprintf(stderr, "stack=%u:0x%0x\n", stack_ptr, stack[stack_ptr]);
	fprintf(stderr, "ptr=%0x:0x%0x\n", ptr, memory[ptr]);
	fprintf(stderr, "--------\n");
	fflush(NULL);
}

void jump_to_next_matching(void)
{
	int nest_level=0;
	while(code[code_ptr] != '\0')
	{
		if(code[code_ptr] == '[')
			++nest_level;
		else if(code[code_ptr] == ']')
			--nest_level;
		else if(nest_level == 0)
			return;
		code_ptr++;
	}
}

void interprete(code_t c)
{
	switch(c)
	{
		case '+':
			memory[ptr]++;
			++code_ptr;
			break;
		case '-':
			memory[ptr]--;
			++code_ptr;
			break;
		case '>':
			ptr++;
#ifdef FAST
			if(ptr >= MEMSIZE)
				panic("?>MEM");
#endif
			++code_ptr;
			break;
		case '<':
			ptr--;
#ifndef FAST
			if(ptr >= MEMSIZE)
				panic("?<MEM");
#endif
			++code_ptr;
			break;
		case '[':
			if(memory[ptr] == 0)
			{
				jump_to_next_matching(); /* Skip everything until reached matching "]" */
#ifndef FAST
				if(debug)
					fprintf(stderr, "[:%d\n", code_ptr);
#endif
			}
			else
			{
				push(stack, &stack_ptr, code_ptr + 1); /* Push next PC */
				code_ptr++;
			}
			break;
		case ']':
			if(memory[ptr] != 0) /* if not equals to 0 */
			{
				code_ptr=stack[stack_ptr]; /* Peek */
#ifndef FAST
				if(debug)
					fprintf(stderr, "]:%d\n", code_ptr);
#endif
			}
			else
			{
				code_ptr++;
				stack_ptr--; /* Drop */
#ifndef FAST
				if(stack_ptr >= STACKSIZE)
					panic("?<STACK");
#endif
			}
			break;
		case ',':
			memory[ptr]=(memory_t)getchar();
			++code_ptr;
			break;
		case '.':
			putchar(memory[ptr]);
			fflush(NULL);
			++code_ptr;
			break;
		default:
			++code_ptr;
			break;
	}
}

int main(int argc, char **argv)
{
	/* Init */
	memory	= calloc(MEMSIZE, sizeof(memory_t));
	code	= calloc(CODESIZE, sizeof(code_t));
	stack	= calloc(STACKSIZE, sizeof(stack_type));

	/* Parse Argument */
	FILE *corefile;
	int opt;

	if(!(argc >= 2))
	{
		puts("argc < 2!");
	}
	else
	{
		while((opt = getopt(argc, argv, "hdf:c:s:")) != -1)
		{
			switch(opt)
			{
				case 's': /* Read size from argument */
#ifndef FAST
					sscanf(optarg, "%u,%u,%u", &memsize, &codesize, &stacksize);
					if(memsize == 0 || codesize == 0 || stacksize == 0)
						panic("?SIZE=0");
					free(memory);
					free(code);
					free(stack);
					memory	= calloc(memsize, sizeof(memory_t));
					code	= calloc(codesize, sizeof(code_t));
					stack	= calloc(stacksize, sizeof(stack_type));
#endif
					break;
				case 'f': /* File */
					if(strcmp(optarg, "-"))
					{
						if((corefile = fopen(optarg, "r")) == NULL)
						{
							perror(optarg);
							exit(8);
						}
						read_code(corefile);
						fclose(corefile);
					}
					else
						read_code(stdin);
					break;
				case 'c': /* Code */
					strncpy(code, optarg, CODESIZE);
					break;
				case 'h': /* Help */
					printf("Usage: %s [-h] [-f file] [-c code] [-s memsize,codesize,stacksize] [-d]\n", argv[0]);
					break;
				case 'd': /* Debug */
					puts("Enabled Debug verbose message");
					debug = TRUE;
					break;
				default:
					exit(1);

			}
		}
	}

	if(debug)
	{
		fprintf(stderr, "memory	= %p[%d]\n", memory, MEMSIZE);
		fprintf(stderr, "code	= %p[%d]\n", code, CODESIZE);
		fprintf(stderr, "stack	= %p[%d]\n\n", stack, STACKSIZE);
		fflush(NULL);
	}

	while(code[code_ptr] != '\0')
	{
#ifndef FAST
		if(debug)
			debug_output();
#endif
		interprete(code[code_ptr]);
	}

	free(memory);
	free(code);
	free(stack);
	return 0;
}
