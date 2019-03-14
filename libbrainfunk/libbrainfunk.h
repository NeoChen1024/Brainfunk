#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>

#pragma once

#define TRUE	1
#define FALSE	0

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
	long long int ptr;	/* Memory Pointer */
	long long int sp;	/* Stack Pointer */
	bitcode_t code;		/* Code Space */
	mem_t mem;		/* Data Space */
	mem_t stack;		/* Stack Space */
	struct _size size;
};

typedef struct _bf * brainfunk_t;

struct _handler
{
	void(*exec)(code_t code);
	void(*print)(bitcode_t code, char *dst);
	void(*printc)(bitcode_t code, char *dst);
	size_t(*scan)(bitcode_t code, char *text);
};

typedef struct _handler * handler;

enum opcodes
{
	OP_HLT,
	OP_ADD,
	OP_SUB,
	OP_FWD,
	OP_REW,
	OP_JEZ,
	OP_JNZ,
	OP_IO,
	OP_SET,
	OP_STP,
	OP_JMP,
	OP_POP,
	OP_PUSH,
	OP_PSHI,
	OP_ADDS,
	OP_SUBS,
	OP_JSEZ,
	OP_JSNZ,
	OP_FRK,
	OP_HCF,
	OP_INSTS /* Total number of instructions */
};
