#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define MEMSIZE	262144
#define STACKSIZE 65536
typedef char memory_t;
typedef unsigned int arg_t;
memory_t *memory;
unsigned int ptr=0;
memory_t *stack;
unsigned int stack_ptr=0;

#define peek stack[stack_ptr]

void panic(char *msg)
{
	puts(msg);
	free(memory);
	exit(8);
}

void add(arg_t arg)
{
	memory[ptr]+=arg;
}

void adds(arg_t arg)
{
	peek+=arg;
}

void sub(arg_t arg)
{
	memory[ptr]-=arg;
}

void subs(arg_t arg)
{
	peek-=arg;
}

void fwd(arg_t arg)
{
	ptr += arg;
#ifndef FAST
	if(ptr >= MEMSIZE)
		panic("?MEM");
#endif
}

void rew(arg_t arg)
{
	ptr -= arg;
#ifndef FAST
	if(ptr >= MEMSIZE)
		panic("?MEM");
#endif
}

void set(arg_t arg)
{
	memory[ptr] = (memory_t)arg;
}

void push(memory_t *stack, unsigned int *ptr, memory_t content)
{
	if(++(*ptr) >= STACKSIZE)
		panic("?>STACK");
	stack[*ptr] = content;
}

memory_t pop(memory_t *stack, unsigned int *ptr)
{
	if(*ptr == 0)
		panic("?<STACK");
	return stack[(*ptr)--];
}

void pshi(arg_t arg)
{
	push(stack, &stack_ptr, (memory_t)arg);
}

void frk(void)
{
	memory[ptr] = (memory_t)fork() & 0xFF;
}

void hcf(arg_t arg)
{
	*(volatile unsigned int*)0=arg;
}

memory_t in()
{
	return (memory_t)getchar();
}

void out(memory_t arg)
{
	putchar(arg);
	fflush(NULL);
}

void hlt(arg_t arg)
{
	free(memory);
	exit(arg);
}
