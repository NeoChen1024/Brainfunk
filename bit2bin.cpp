#include "libbrainfunk.h"
#include <string>
#include <iostream>
#include <vector>
#include <map>
#define TEXTLEN	(64)

using std::string;

addr_t address = 0;
char op[TEXTLEN];
char arg[TEXTLEN];

std::map<string, data_t> opcodes = {
	{"X", 0x00},
	{"A", 0x01},
	{"S", 0x02},
	{"MUL", 0x03},
	{"F", 0x04},
	{"M", 0x05},
	{"JE", 0x06},
	{"JZ", 0x07},
	{"IO", 0x08},
	{"H", 0x09}
};

void emit(string op, string arg, FILE *fd)
{
	uint32_t inst = 0; // only use lower 24bits

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
		inst |= (dual.offset & 0xFFF) << 8;
	}

	// Offset
	if(op == "F" || op == "M")
	{
		offset_t offset = 0;
		sscanf(arg.c_str(), "%zd", &offset);

		inst |= offset & 0xFFFFF;
	}

	// Addr
	if(op == "JE" || op == "JN")
	{
		addr_t addr = 0;
		sscanf(arg.c_str(), "%zu", &addr);

		inst |= addr & 0xFFFFF;
	}

	fprintf(fd, "%06x\n", inst);
}

int main(int argc, char **argv)
{
	while(fscanf(stdin, "%zu:\t%64s\t%64[^\n]\n", &address, op, arg) == 3)
	{
		printf("addr == %zu, op == '%s', arg == '%s'\n", address, op, arg);
		// Ignore address
		
		emit(string(op), string(arg), stdout);
	}
}