/* Neo_Chen's Brainfuck Interpreter */

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <regex.h>
#include <assert.h>

#pragma once

#define INLINE static inline

#ifdef EXPECT_MACRO
#  define likely(x)	__builtin_expect(!!(x), 1)
#  define unlikely(x)	__builtin_expect(!!(x), 0)
#else
#  define likely(x)	(x)
#  define unlikely(x)	(x)
#endif

/* Used in execution function */
#define _HALT	0
#define _CONT	1

/* I/O type number */
#define _IO_IN	0
#define _IO_OUT	1

/* Limits */
#define _MAXLEN			4096
#define _INITIAL_TEXT_SIZE	4096
#define _PCSTACK_SIZE		4096
#define _OPLEN			4

/* External accessible macro definition */
#define TRUE	-1
#define FALSE	0

#define DEBUG	1
#define NODEBUG	0

#define BITCODE_FORMAT_C	0
#define BITCODE_FORMAT_PLAIN	1

typedef uint8_t data_t;
typedef data_t * mem_t;
typedef uint8_t op_t;
typedef size_t addr_t;
typedef ssize_t offset_t;

typedef struct
{
	int32_t mul;
	int32_t offset;
} dual_t;

typedef union
{
	data_t im;		/* Intermediate */
	addr_t addr;		/* Address */
	offset_t offset;	/* offset */
	dual_t dual;		/* Dual */
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
	bitcode_t code;	/* Code Space */
	mem_t mem;	/* Data Space */
	struct _size size;

	int debug;	/* Debug? */
};

typedef struct _bf * brainfunk_t;

struct _pcstack
{
	size_t size;
	addr_t ptr;
	size_t *stack;
};

typedef struct _pcstack * pcstack_t;

typedef	int (*exec_handler_t)(brainfunk_t cpu);

enum opcodes
{
	_OP_X,
	_OP_A,
	_OP_MUL,
	_OP_S,
	_OP_F,
	_OP_M,
	_OP_JE,
	_OP_JN,
	_OP_IO,
	_OP_Y,
	_OP_D,
	_OP_H,
	_OP_INSTS /* Total number of instructions */
};

/* Lexer & Bitcode emitter */
typedef struct
{
	char regexp[_MAXLEN];
	int (*scan)(char *text, size_t len, brainfunk_t cpu, pcstack_t pcstack);
} scan_handler_t;

void panic(char *msg);
brainfunk_t brainfunk_init(size_t codesize, size_t memsize, int debug);
void brainfunk_destroy(brainfunk_t *brainfunk);
void brainfunk_execute(brainfunk_t bf);
void bitcode_dump(brainfunk_t cpu, int format, FILE *fp);
char *brainfunk_readtext(FILE *fp, int compat, size_t *size);
void brainfunk_dumptext(char *code, FILE *fp);
void bitcode_convert(brainfunk_t cpu, char *text);
void quit(int32_t arg);

/* These functions must be provided externally */
#define IO_IN_FUNCTION	\
	int _io_in(int debug)
#define IO_OUT_FUNCTION	\
	void _io_out(data_t data, int debug)
#define DEBUG_OUT_FUNCTION \
	void _debug_out(char *str, int debug)
#define PANIC_FUNCTION	\
	void _panic(char *msg)

extern IO_IN_FUNCTION;
extern IO_OUT_FUNCTION;
extern DEBUG_OUT_FUNCTION;
extern PANIC_FUNCTION;
