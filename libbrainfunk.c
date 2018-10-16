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

/* If "FAST" is defined, will bypass all runtime checking */

/* You need to define
 * 	void output(memory_t c)
 * and
 * 	memory_t input(void)
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <libbrainfunk.h>

extern memory_t *memory;
extern unsigned int ptr;
extern stack_type *stack;
extern unsigned int stack_ptr;
extern code_t *code;
extern unsigned int code_ptr;

extern int debug;

extern unsigned int memsize;
extern unsigned int codesize;
extern unsigned int stacksize;

void panic(char *msg)
{
	fputs(msg, stderr);
	exit(2);
}

/* Read Code */
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
	return stack[(*ptr)--];
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
			memory[ptr]=(memory_t)input();
			++code_ptr;
			break;
		case '.':
			output(memory[ptr]);
			++code_ptr;
			break;
		default:
			++code_ptr;
			break;
	}
}

