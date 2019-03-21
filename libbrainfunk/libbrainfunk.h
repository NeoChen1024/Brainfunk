#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>

#pragma once

#define TRUE	1
#define FALSE	0

#define CONT	1
#define HALT	0

#define LEXERR	-1

#define DEBUG	1
#define NODEBUG	0

#define IO_OUT	0
#define IO_IN	1
#define IO_OUTS	2
#define IO_INS	3

#define STRLENGTH	4096

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

struct _handler
{
	int(*exec)(brainfunk_t cpu);
	arg_t(*scan)(bitcode_t code, arg_t pc, arg_t *textptr, char *text);	/* Brainfunk to Bitcode */
};

typedef struct _handler handler_t;

struct _pcstack
{
	size_t size;
	arg_t ptr;
	arg_t *stack;
};

typedef struct _pcstack * pcstack_t;

enum opcodes
{
	OP_HLT,
	OP_ALU,
	OP_ALUS,
	OP_SET,
	OP_POP,
	OP_PUSH,
	OP_PSHI,
	OP_MOV,
	OP_STP,
	OP_JMP,
	OP_JEZ,
	OP_JNZ,
	OP_JSEZ,
	OP_JSNZ,
	OP_IO,
	OP_FRK,
	OP_INV,
	OP_INSTS /* Total number of instructions */
};

#include "functions.h"
#include "handler.h"

extern handler_t handler[OP_INSTS];
