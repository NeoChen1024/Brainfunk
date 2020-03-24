/* ========================================================================== *\
||                       Single Header Brainfunk Library                      ||
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
	current += x
#define alus(x)	\
	_push(_pop() + current)
#define	set(x)	\
	current = x
#define push(x)	\
	_push(current)
#define pop(x)	\
	current = _pop();
#define	pshi(x)	\
	_push((memory_t)x)
#define	mov(x)	\
	ptr += x
#define	stp(x)	\
	ptr = x
#define	jmp(x)	\
	goto L ## x
#define	jez(x)	\
	if(current == 0) goto L ## x
#define	jnz(x)	\
	if(current != 0) goto L ## x
#define	jsez(x)	\
	if(_pop() == 0) goto L ## x
#define	jsnz(x)	\
	if(_pop() != 0) goto L ## x
#define	frk(x)	\
	pid_t p = fork();	\
	current = p;		\
	if(p != 0) current++;
#define	inv(x)	\
	panic("?INVALID");

INLINE void _push(memory_t content)
{
	if(stack_ptr >= STACKSIZE)
		return;
	stack[stack_ptr++] = content;
}

INLINE memory_t _pop()
{
	if(stack_ptr == 0)
		return 0;
	return stack[--stack_ptr];
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
			_push(in());
			break;
		case 3: /* OUTS */
			out(_pop());
			break;
	}
}

void hlt(arg_t arg)
{
	exit(arg);
}
