/* ========================================================================== *\
||                       Single Header Brainfunk Executer                     ||
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
typedef size_t arg_t;
memory_t *memory;
int ptr=0;

#define peek (stack[stack_ptr])
#define current (memory[ptr])
#
void panic(char *msg)
{
	puts(msg);
	free(memory);
	exit(8);
}

/* Assumption: a must be positive */
INLINE int overflow_add(int a, int b, int max)
{
	if(b > 0)
	{
		if(a + b > max)
			return a + b - max;
	}
	else
	{
		if(a + b < 0)
			return a + b + max;
	}
	return a + b;
}

#define	a(x)	\
	current += x
#define mul(mul, offset)	\
	memory[overflow_add(ptr, offset, MEMSIZE)] += current * mul
#define	s(x)	\
	current = x
#define f(x)	\
	while(current != 0) ptr += x
#define	m(x)	\
	ptr = overflow_add(ptr, x, MEMSIZE);
#define	je(x)	\
	if(current == 0) goto L ## x
#define	jn(x)	\
	if(current != 0) goto L ## x
#define	y(x)	\
	pid_t p = fork();	\
	current = p;		\
	if(p != 0) current = 0xFF
#define	x(x)	\
	panic("?INVALID");

void init(void)
{
	setvbuf(stdin, NULL, _IONBF, 0);
	setvbuf(stdout, NULL, _IONBF, 0);

	memory = calloc(1, MEMSIZE);
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
	}
}

void d(void)
{
	fprintf(stderr, ">> PTR = %d, MEM[PTR] = %hhu", ptr, memory[ptr]);
}

void h(void)
{
	exit(0);
}
