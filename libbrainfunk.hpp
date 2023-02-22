/* Neo_Chen's Brainfuck Interpreter */

#include <iostream>
#include <sstream>
#include <fstream>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cassert>
#include <regex>

#pragma once

#define INLINE static inline

#ifdef EXPECT_MACRO
#  define likely(x)	__builtin_expect(!!(x), 1)
#  define unlikely(x)	__builtin_expect(!!(x), 0)
#else
#  define likely(x)	(x)
#  define unlikely(x)	(x)
#endif

#define MEMSIZE	(1 << 16)	/* Default memory size */

/* Used in execution function */
#define _HALT	0
#define _CONT	1

/* I/O type number */
#define _IO_IN	0
#define _IO_OUT	1

/* External accessible macro definition */

#define BITCODE_FORMAT_C	0
#define BITCODE_FORMAT_PLAIN	1

using std::string;
using std::vector;
using std::regex;
using std::ostream;
using std::istream;

typedef size_t addr_t;
typedef uint8_t memory_t;
typedef ssize_t offset_t;

enum opcodes
{
	_OP_X,
	_OP_A,
	_OP_S,
	_OP_MUL,
	_OP_F,
	_OP_M,
	_OP_JE,
	_OP_JN,
	_OP_IO,
	_OP_H,
	_OP_INSTS /* Total number of instructions */
};

union operand_s
{
	uint8_t byte;
	offset_t offset;
	addr_t addr;

	struct
	{
		memory_t mul; // %256
		offset_t offset;
	} dual;
};

enum formats
{
	FMT_BIT,
	FMT_C
};

class Bitcode
{
public:
	Bitcode();
	Bitcode(uint8_t opcode, operand_s operand);
	Bitcode(uint8_t opcode);
	string to_string(enum formats formats = FMT_BIT) const;
	bool execute(vector<memory_t> &memory, addr_t &pc, addr_t &ptr);

	uint8_t opcode;
	operand_s operand;

private:
	static string opname[_OP_INSTS];
	static char opcode_type[_OP_INSTS];
};

struct code_patterns
{
	bool (*handler)(vector<Bitcode> &bytecode, vector<addr_t> &stack, string const &text);
	regex pattern;
	string regexp;
};

class Brainfunk
{
public:
	Brainfunk(size_t memsize = MEMSIZE);
	~Brainfunk();
	void translate(string &code);
	void run();
	void clear();
	void dump(ostream &os, enum formats formats = FMT_BIT);

private:
	addr_t pc;
	addr_t ptr;
	vector<memory_t> memory;
	vector<class Bitcode> bitcode;
};
