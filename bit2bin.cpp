#include "libbrainfunk.hpp"
#include <string>
#include <iostream>
#include <cstdio>
#include <vector>
#include <map>
#include <bitset>
#include <bit>

#define TEXTLEN	(65)

using std::string;
using std::bitset;
using std::fprintf;

addr_t address = 0;
char op[TEXTLEN];
char arg[TEXTLEN];

#define _ABS(x)	((x) < 0 ? -(x) : (x))

std::map<string, memory_t> opcodes = {
	{"X", 0x00},
	{"A", 0x01},
	{"S", 0x02},
	{"MUL", 0x03},
	{"F", 0x04},
	{"M", 0x05},
	{"JE", 0x06},
	{"JN", 0x07},
	{"IO", 0x08},
	{"H", 0x09}
};

void emit(addr_t address, string op, string arg, FILE *fd)
{
	bitset<24> inst = 0;

	inst |= opcodes[op] << 20;
	
	// None
	if(op == "X" || op == "H")
	{
	}
	// Imm
	else if(op == "A" || op == "S" || op == "IO")
	{
		memory_t imm = 0;
		sscanf(arg.c_str(), "%hhu", &imm);

		inst |= imm;
	}
	// Dual
	else if(op == "MUL")
	{
		memory_t mul;
		offset_t offset;
		sscanf(arg.c_str(), "%hhu, %zd", &mul, &offset);

		inst |= mul;
		if(_ABS(offset) > 0x3FF)
			fprintf(stderr, "Warning: offset %zd at %zu is too large\n", offset, address);
		inst |= (offset & 0xFFF) << 8;
	}
	// Offset
	else if(op == "F" || op == "M" || op == "JE" || op == "JN")
	{
		offset_t offset = 0;
		sscanf(arg.c_str(), "%zd", &offset);

		if(_ABS(offset) > ((1<<19) - 1))
			fprintf(stderr, "Warning: offset %zd at %zu is too large\n", offset, address);
		inst |= offset & 0xFFFFF;
	}
	else
	{
		fprintf(stderr, "Error: unknown opcode %s at %zu\n", op.c_str(), address);
	}

	fprintf(fd, "%06lx\n", inst.to_ulong());
}

int main(int argc, char **argv)
{
	size_t inst_ctr = 0;
	while(fscanf(stdin, "%zu:\t%64s\t%64[^\n]\n", &address, op, arg) != EOF)
	{
		emit(address, string(op), string(arg), stdout);
		inst_ctr++;
	}

	fprintf(stderr, "Assembled %zu instruction(s)\n", inst_ctr);
}