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
using std::regex;
using std::ostream;
using std::istream;

typedef unsigned int pc_t;
typedef uint8_t memory_t;
typedef int32_t offset_t;

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
	_OP_H,
	_OP_INSTS /* Total number of instructions */
};

union operand_s
{
	uint8_t byte;
	offset_t offset;
	pc_t addr;

	struct dual_s
	{
		offset_t mul; // %256
		offset_t offset;
	} dual;
};

enum formats
{
	FMT_BF,
	FMT_M,
	FMT_C
};

class Bytecode
{
public:
	Bytecode();
	Bytecode(uint8_t opcode, operand_s operand);
	Bytecode(uint8_t opcode);
	uint8_t opcode;
	operand_s operand;
	string to_text(enum formats formats = FMT_BF) const;
	int execute(vector<memory_t> &memory, pc_t &pc, pc_t &ptr);
private:
	static string opname[_OP_INSTS];
	static char opcode_type[_OP_INSTS];
};

struct code_patterns
{
	bool (*handler)(vector<Bytecode> &bytecode, vector<pc_t> &stack, const string &text);
	regex pattern;
	string regexp;
};

class Brainfunk
{
public:
	Brainfunk(bool debug = false, size_t memsize = MEMSIZE, size_t stacksize = STACKSIZE);
	~Brainfunk();
	void translate(string &code);
	void run();
	void clear();
	void dump_code(ostream &os, enum formats formats = FMT_BF);

private:
	bool debug;

	pc_t pc;
	pc_t ptr;
	vector<memory_t> memory;
	vector<pc_t> stack;
	vector<class Bytecode> bytecode;
};

} // namespace Brainfunk
