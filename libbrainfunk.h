#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <regex.h>

#pragma once

/* Used in execution function */
#define _HALT	0
#define _CONT	1

/* Used in preprocessor */
#define _LEXERR	0
#define _ADV	1

#define _DIFF	-1
#define _SAME	0

/* I/O type number */
#define _IO_IN	0
#define _IO_OUT	1

/* Limits */
#define _MAXLEN		4096
#define _PCSTACK_SIZE	4096
#define _OPLEN		4

/* External accessible macro definition */
#define TRUE	-1
#define FALSE	0

#define DEBUG	1
#define NODEBUG	0

#define FORMAT_C	0
#define FORMAT_PLAIN	1

/* Compatibility with plain Brainfuck */
#define NOCOMPAT	0
#define COMPAT		1

typedef uint8_t data_t;
typedef data_t * mem_t;
typedef size_t ptr_t;
typedef uint8_t op_t;

struct _dual_arg
{
	ptr_t x;
	ptr_t y;
};

typedef union
{
	data_t data;
	ptr_t ptr;
	ssize_t offset;
	struct _dual_arg dual;
} arg_t;

struct _bitcode
{
	op_t op;	/* Op-code */
	arg_t arg;	/* Operand */
};

typedef struct _bitcode code_t;
typedef struct _bitcode * bitcode_t;

struct _size
{
	size_t code;
	size_t mem;
};

struct _bf
{
	size_t pc;	/* Program Counter */
	size_t codelen;	/* Total Code Length */
	size_t ptr;	/* Memory Pointer */
	bitcode_t code;		/* Code Space */
	mem_t mem;		/* Data Space */
	struct _size size;

	int debug;	/* Debug? */
};

typedef struct _bf * brainfunk_t;

struct _pcstack
{
	size_t size;
	ptr_t ptr;
	size_t *stack;
};

typedef struct _pcstack * pcstack_t;

typedef	int(*handler_t)(brainfunk_t cpu);

enum opcodes
{
	_OP_H,
	_OP_A,
	_OP_S,
	_OP_M,
	_OP_P,
	_OP_J,
	_OP_JE,
	_OP_JN,
	_OP_IO,
	_OP_F,
	_OP_D,
	_OP_I,
	_OP_INSTS /* Total number of instructions */
};

extern handler_t handler[_OP_INSTS];
extern char opname[_OP_INSTS][_OPLEN];

void panic(char *msg);
brainfunk_t brainfunk_init(size_t codesize, size_t memsize, int debug);
void brainfunk_destroy(brainfunk_t *brainfunk);
void brainfunk_execute(brainfunk_t bf);
void bitcode_dump(brainfunk_t cpu, int format, FILE *fp);
void bitcode_read(brainfunk_t cpu, FILE *fp);
char *brainfunk_readtext(FILE *fp, int compat, size_t size);
void brainfunk_dumptext(char *code, FILE *fp);
void bitcode_convert(brainfunk_t cpu, char *text);
void quit(int32_t arg);

/* These functions must be provided externally */
#define IO_IN_FUNCTION	\
	data_t io_in(int debug)
#define IO_OUT_FUNCTION	\
	void io_out(data_t data, int debug)
#define DEBUG_OUT_FUNCTION \
	void debug_out(char *str, int debug)

extern IO_IN_FUNCTION;
extern IO_OUT_FUNCTION;
extern DEBUG_OUT_FUNCTION;
