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

#define DEBUG	1
#define NODEBUG	0

#define STRLENGTH	4096

struct _bitcode
{
	uint8_t op;	/* Op-code */
	int32_t arg;	/* Operand */
};

typedef struct _bitcode code_t;
typedef struct _bitcode * bitcode_t;
typedef uint8_t data_t;
typedef data_t * mem_t;

struct _size
{
	size_t code;
	size_t mem;
	size_t stack;
};

struct _bf
{
	long long int pc;	/* Program Counter */
	long long int codelen;	/* Total Code Length */
	long long int ptr;	/* Memory Pointer */
	long long int sp;	/* Stack Pointer */
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
	size_t(*scan)(bitcode_t code, char *text);	/* Brainfunk to Bitcode */
};

typedef struct _handler handler_t;

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
	OP_INSTS /* Total number of instructions */
};

#include "functions.h"
#include "handler.h"

extern handler_t handler[OP_INSTS];
