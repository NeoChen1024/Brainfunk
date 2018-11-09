/* ==================================== *
 * Brainfunk -- A Brainf**k Toolkit     *
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

int is_code(char c)
{
	if(c == '+' || c == '-' || c == '<' || c == '>' ||
		c == '[' || c == ']' || c == '.' || c == ',')
	{
		return TRUE;
	}
	else
		return FALSE;
}


/* Read Code */
void read_code(FILE* fp)
{
	unsigned int i=0;
	int c=0;
	while((c=getc(fp)) != EOF)
	{
		if(i >= codesize)
			panic("?CODE");
		if((is_code((char)c) == TRUE) || c == '\t' || c == ' ' || c == '\n')
		code[i++]=(char)c;
	}
}

void push(stack_type *stack, unsigned int *ptr, stack_type content)
{
	if(++(*ptr) >= stacksize)
		panic("?>STACK");
	stack[(*ptr)]=content;
}

stack_type pop(stack_type *stack, unsigned int *ptr)
{
	if((*ptr == 0)
		panic("?<STACK");
	return stack[(*ptr)--];
}

void jump_to_next_matching(void)
{
	int nest_level=1;
	while(code[++code_ptr] != '\0')
	{
		if(code[code_ptr] == '[')
			++nest_level;
		else if(code[code_ptr] == ']')
			--nest_level;
		else if(nest_level == 0)
			return;
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
			if(ptr >= memsize)
				panic("?>MEM");
			++code_ptr;
			break;
		case '<':
			ptr--;
			if(ptr >= memsize)
				panic("?<MEM");
			++code_ptr;
			break;
		case '[':
			if(memory[ptr] == 0)
			{
				jump_to_next_matching(); /* Skip everything until reached matching "]" */
				if(debug)
					debug_loop("[:%d\n", code_ptr);
			}
			else
			{
				push(stack, &stack_ptr, code_ptr + 1); /* Push next PC */
				code_ptr++;
			}
#ifdef VISUAL
			print_stack(stack, stack_ptr);
#endif
			break;
		case ']':
			if(memory[ptr] != 0) /* if not equals to 0 */
			{
				code_ptr=stack[stack_ptr]; /* Peek */
				if(debug)
					debug_loop("]:%d\n", code_ptr);
			}
			else
			{
				code_ptr++;
				stack_ptr--; /* Drop */
				if(stack_ptr >= stacksize)
					panic("?<STACK");
			}
#ifdef VISUAL
			print_stack(stack, stack_ptr);
#endif
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

