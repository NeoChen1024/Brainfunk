#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>

pid_t p=0;

#define MEMSIZE		(1<<20)
#define STACKSIZE	(1<<16)
#define INLINE		static inline

typedef uint8_t memory_t;
typedef unsigned int arg_t;
memory_t *memory;
unsigned int ptr=0;
memory_t *stack;
unsigned int stack_ptr=0;

#define peek stack[stack_ptr]
#define current (memory[ptr])

void panic(char *msg)
{
	puts(msg);
	free(memory);
	exit(8);
}

#define	alu(x)	\
	(current += x)
#define	mov(x)	\
	(ptr += x)
#define	set(x)	\
	(current = x)
#define	pshi(x)	\
	push((memory_t)x)
#define	stp(x)	\
	(ptr = x)
#define	jmp(x)	\
	goto L ## x
#define	jez(x)	\
	if(current == 0) goto L ## x
#define	jnz(x)	\
	if(current != 0) goto L ## x
#define	jsez(x)	\
	if(pop() == 0) goto L ## x
#define	jsnz(x)	\
	if(pop() != 0) goto L ## x
#define	frk(x)	\
	pid_t p = fork();	\
	current = p;		\
	if(p != 0) current++;
#define	inv(x)	\
	panic("?INVALID");

INLINE void push(memory_t content)
{
	if(++stack_ptr >= STACKSIZE)
		panic("?>STACK");
	stack[stack_ptr] = content;
}

INLINE memory_t pop()
{
	if(stack_ptr == 0)
		panic("?<STACK");
	return stack[stack_ptr--];
}

void init(void)
{
	setvbuf(stdin, NULL, _IONBF, 0);
	setvbuf(stdout, NULL, _IONBF, 0);

	memory = calloc(1, MEMSIZE);
	stack = calloc(1, STACKSIZE);
}

INLINE memory_t in()
{
	return (memory_t)getchar();
}

INLINE void out(memory_t arg)
{
	putchar(arg);
}

INLINE void io(arg_t arg)
{
	switch(arg)
	{
		case 0: /* IN */
			memory[ptr] = in();
			break;
		case 1: /* OUT */
			out(memory[ptr]);
			break;
		case 2: /* INS */
			push(in());
			break;
		case 3: /* OUTS */
			out(pop());
			break;
	}
}

void hlt(arg_t arg)
{
	free(memory);
	exit(arg);
}
