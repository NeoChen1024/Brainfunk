#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>

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
#define _IO_INS	2
#define _IO_OUTS	3

/* Limits */
#define _MAXLEN		4096
#define _PCSTACK_SIZE	4096
#define _OPLEN		16

/* External accessible macro definition */
#define TRUE	-1
#define FALSE	0

#define DEBUG	1
#define NODEBUG	0

#define FORMAT_C	0
#define FORMAT_PLAIN	1

#define NOCOMPAT	0
#define COMPAT		1

typedef uint8_t data_t;
typedef data_t * mem_t;
typedef long long int arg_t;

struct _bitcode
{
	uint8_t op;	/* Op-code */
	arg_t arg;	/* Operand */
};

typedef struct _bitcode code_t;
typedef struct _bitcode * bitcode_t;

struct _size
{
	size_t code;
	size_t mem;
	size_t stack;
};

struct _bf
{
	arg_t pc;	/* Program Counter */
	arg_t codelen;	/* Total Code Length */
	arg_t ptr;	/* Memory Pointer */
	arg_t sp;	/* Stack Pointer */
	bitcode_t code;		/* Code Space */
	mem_t mem;		/* Data Space */
	mem_t stack;		/* Stack Space */
	struct _size size;

	int debug;	/* Debug? */
};

typedef struct _bf * brainfunk_t;

struct _pcstack
{
	size_t size;
	arg_t ptr;
	arg_t *stack;
};

typedef struct _pcstack * pcstack_t;

struct _handler
{
	int(*exec)(brainfunk_t cpu);
	arg_t(*scan)(
		bitcode_t code,
		arg_t *pc,
		pcstack_t pcstack,
		arg_t *textptr,
		char *text);	/* Brainfunk to Bitcode */
};

typedef struct _handler handler_t;

enum opcodes
{
	_OP_HLT,
	_OP_ALU,
	_OP_ALUS,
	_OP_SET,
	_OP_POP,
	_OP_PUSH,
	_OP_PSHI,
	_OP_MOV,
	_OP_STP,
	_OP_JMP,
	_OP_JEZ,
	_OP_JNZ,
	_OP_JSEZ,
	_OP_JSNZ,
	_OP_IO,
	_OP_FRK,
	_OP_INV,
	_OP_INSTS /* Total number of instructions */
};

extern handler_t handler[_OP_INSTS];
extern char opname[_OP_INSTS][_OPLEN];

void panic(char *msg);
brainfunk_t brainfunk_init(size_t codesize, size_t memsize, size_t stacksize, int debug);
void brainfunk_destroy(brainfunk_t *brainfunk);
void brainfunk_execute(brainfunk_t bf);
void bitcode_dump(brainfunk_t cpu, int format, FILE *fp);
void bitcode_read(brainfunk_t cpu, FILE *fp);
char *brainfunk_readtext(FILE *fp, int compat, size_t size);
void brainfunk_dumptext(char *code, FILE *fp);
void bitcode_convert(brainfunk_t cpu, char *text);
void quit(int32_t arg);

/* These functions must be provided externally */
extern data_t io_in(int debug);
extern void io_out(data_t data, int debug);

#define IO_IN_FUNCTION	\
	data_t io_in(int debug)
#define IO_OUT_FUNCTION	\
	void io_out(data_t data, int debug)
