#include <iostream>
#include <fstream>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cassert>
#include <regex>

#pragma once

#define MEMSIZE 65536
#define STACKSIZE 4096

/* Used in execution function */
#define _HALT	0
#define _CONT	1

/* I/O type number */
#define _IO_IN	0
#define _IO_OUT	1

namespace Brainfunk
{
using std::string;
using std::vector;

typedef unsigned int pc_t;
typedef uint8_t memory_t;

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

union operand_s
{
	uint8_t byte;
	int offset;
	pc_t addr;

	struct dual_s
	{
		int offset;
		int multiplier; // %256
	} mul;
};

class bytecode
{
public:
	uint8_t opcode;
	operand_s operand;
	string to_text(pc_t pc) const;
	int execute(vector<memory_t> &memory, pc_t &pc, pc_t &ptr);
private:
	static string opname[_OP_INSTS];
	static char opcode_type[_OP_INSTS];
};

class Brainfunk
{
public:
	Brainfunk(bool debug = false, size_t memsize = MEMSIZE, size_t stacksize = STACKSIZE);
	~Brainfunk();
	void translate(string &code);
	void run();
	void dump_code() const;

private:
	string *code;
	bool debug;

	pc_t pc;
	pc_t ptr;
	vector<memory_t> memory;
	vector<pc_t> stack;
	vector<class bytecode> bytecode;
};

} // namespace Brainfunk
