#include "libbrainfunk.hpp"
#include <string>
#include <iostream>
#include <vector>
#include <map>
#include <bitset>
#include <bit>

#define TEXTLEN	(64)

using std::string;
using std::bitset;

addr_t address = 0;
char op[TEXTLEN];
char arg[TEXTLEN];

#define _ABS(x)	((x) < 0 ? -(x) : (x))

std::map<string, data_t> opcodes = {
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
	if(op == "A" || op == "S" || op == "IO")
	{
		data_t imm = 0;
		sscanf(arg.c_str(), "%hhu", &imm);

		inst |= imm;
	}

	// Dual
	if(op == "MUL")
	{
		dual_t dual;
		sscanf(arg.c_str(), "%hhu, %d", &dual.mul, &dual.offset);

		inst |= dual.mul;
		if(_ABS(dual.offset) > 0xFFF)
			fprintf(stderr, "Warning: offset %d at %zu is too large\n", dual.offset, address);
		inst |= (dual.offset & 0xFFF) << 8;
	}

	// Offset
	if(op == "F" || op == "M")
	{
		offset_t offset = 0;
		sscanf(arg.c_str(), "%zd", &offset);

		if(_ABS(offset) > ((1<<20) - 1))
			fprintf(stderr, "Warning: offset %zd at %zu is too large\n", offset, address);
		inst |= offset & 0xFFFFF;
	}

	// JE & JN needs conversion to relative addresses
	if(op == "JE" || op == "JN")
	{
		addr_t addr = 0;
		sscanf(arg.c_str(), "%zu", &addr);
		ssize_t offset = addr - address;

		if(_ABS(offset) > ((1<<20) - 1))
			fprintf(stderr, "Warning: addr %zu at %zu is too distant\n", addr, address);
		inst |= offset & 0xFFFFF;
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