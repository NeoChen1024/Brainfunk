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
#define INLINE		static inline

typedef uint8_t memory_t;
memory_t *mem;

/* This implementation sacrificed all sanity check for the sake of speed */

void panic(char *msg)
{
	puts(msg);
	free(mem);
	exit(8);
}

#define	a(x)	\
	*mem += x
#define mul(mul, offset)	\
	mem[offset] += *mem * mul
#define	s(x)	\
	*mem = x
#define f(x)	\
	while(*mem != 0) mem += x
#define	m(x)	\
	mem += x
#define	je(x)	\
	while(*mem) {
#define	jn(x)	\
	}
#define	y(x)	\
	pid_t p = fork();	\
	*mem = p;		\
	if(p != 0) *mem = 0xFF
#define	x(x)	\
	panic("?INVALID")

void init(void)
{
	/* Disable Buffering */
	setvbuf(stdin, NULL, _IONBF, 0);
	setvbuf(stdout, NULL, _IONBF, 0);

	mem = (memory_t *)calloc(sizeof(memory_t), MEMSIZE) + MEMSIZE / 2;
}

INLINE memory_t in()
{
	int c = getchar();
	return c == EOF ? 0 : (memory_t)c;
}

INLINE void out(memory_t arg)
{
	putchar(arg);
}

INLINE void io(memory_t arg)
{
	switch(arg)
	{
		case 0: /* IN */
			*mem = in();
			break;
		case 1: /* OUT */
			out(*mem);
			break;
	}
}

void d(void)
{
	fprintf(stderr, ">> MEM[PTR] = %hhu", *mem);
}

void h(void)
{
	exit(0);
}
