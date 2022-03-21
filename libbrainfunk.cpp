#include "libbrainfunk.hpp"

namespace Brainfunk
{
using std::string;
using std::cout;
using std::cin;
using std::flush;
using std::cerr;
using std::clog;
using std::endl;
using std::vector;
using std::regex;

Brainfunk::Brainfunk(bool debug, size_t memsize, size_t stacksize)
{
	this->ptr = 0;
	this->debug = debug;
	try
	{
		this->memory.resize(memsize);
		this->stack.reserve(stacksize);
		this->bytecode.reserve(memsize);
	}
	catch(const std::exception& e)
	{
		std::cerr << e.what() << '\n';
	}

	std::fill(this->memory.begin(), this->memory.end(), 0);
}

Brainfunk::~Brainfunk()
{
	this->memory.clear();
	this->stack.clear();
	this->bytecode.clear();
}

int count_continous_chars(string &text, pc_t &start, string c)
{
	assert(c.size() >= 2);
	int offset = 0;
	for(; start < text.size(); start++)
	{
		if(text.at(start) == c[0])
			offset++;
		else if(text.at(start) == c[1])
			offset--;
		else
			break;
	}
	return offset;
}

bool strcmp_code(string &code, string pattern, pc_t &start)
{
	pc_t i = 0;
	for(; i < pattern.size(); i++)
	{
		if(start + i >= code.size())
			return false;
		if(code.at(start + i) != pattern.at(i))
		{
			return false;
		}
	}
	start += i;
	return true;
}

void Brainfunk::translate(string &text)
{
	char c;
	this->code = &text;
	this->bytecode.clear();
	this->pc = 0;
	pc_t temp = 0;
	class bytecode current_instruction = {_OP_X, {0}};

	while(this->pc < this->code->size())
	{
		if(strcmp_code(*this->code, "[-]", this->pc))
		{
			current_instruction.opcode = _OP_S;
			current_instruction.operand.byte = 0;
			goto store_bytecode;
		}
		if(strcmp_code(*this->code, "[>]", this->pc))
		{
			current_instruction.opcode = _OP_F;
			current_instruction.operand.byte = 1;
			goto store_bytecode;
		}
		if(strcmp_code(*this->code, "[<]", this->pc))
		{
			current_instruction.opcode = _OP_F;
			current_instruction.operand.byte = -1;
			goto store_bytecode;
		}
		c = this->code->at(this->pc);
		switch(c)
		{
			case '+':
			case '-':
				current_instruction.opcode = _OP_A;
				current_instruction.operand.offset = count_continous_chars(text, this->pc, "+-");
				break;
			case '>':
			case '<':
				current_instruction.opcode = _OP_M;
				current_instruction.operand.offset = count_continous_chars(text, this->pc, "><");
				break;
			case '.':
				current_instruction.opcode = _OP_IO;
				current_instruction.operand.offset = _IO_OUT;
				this->pc++;
				break;
			case ',':
				current_instruction.opcode = _OP_IO;
				current_instruction.operand.offset = _IO_IN;
				this->pc++;
				break;
			case '[':
				current_instruction.opcode = _OP_JE;
				current_instruction.operand.addr = 0; // Placeholder, will come back later
				this->stack.push_back(this->bytecode.end() - this->bytecode.begin()); // Save the address of the JE instruction
				this->pc++;
				break;
			case ']':
				temp = this->stack.back();
				this->stack.pop_back();
				current_instruction.opcode = _OP_JN;
				current_instruction.operand.addr = temp;
				this->bytecode.at(temp).operand.addr = this->bytecode.end() - this->bytecode.begin();
				this->pc++;
				break;
			default:
				cerr << "Unknown character: " << c << endl;
				break;
		}
store_bytecode:
		this->bytecode.push_back(current_instruction);
	}
	current_instruction.opcode = _OP_H; // No operand
	this->bytecode.push_back(current_instruction);
	this->pc = 0;
}

void Brainfunk::dump_code() const
{
	pc_t pc = 0;
	for(auto it = this->bytecode.begin(); it != this->bytecode.end(); it++)
	{
		cout << it->to_text(it - this->bytecode.begin()) << endl;
		pc++;
	}
}

void Brainfunk::run()
{
	int ret = _CONT;
	this->pc = 0;
	while(ret == _CONT && !this->bytecode.empty())
	{
		if(this->debug)
		{
			cerr << "I = " << this->bytecode[this->pc].to_text(this->pc)
			<< "\tM @(" << this->ptr << ") =" << (int)this->memory[this->ptr]
			<< endl;
		}
		ret = this->bytecode[this->pc].execute(this->memory, this->pc, this->ptr);
	}
}

string bytecode::opname[_OP_INSTS] =
{
	"x",
	"a",
	"mul",
	"s",
	"f",
	"m",
	"je",
	"jn",
	"io",
	"y",
	"d",
	"h"
};

/* operand type of the opcode,
 *
 * N => None
 * O => Offset
 * M => mul's Dual Operand
 * A => Address
 * I => Intermediate
 */

char bytecode::opcode_type[_OP_INSTS] =
{
	'N',	/* X */
	'O',	/* A */
	'M',	/* MUL */
	'I',	/* S */
	'O',	/* F */
	'O',	/* M */
	'A',	/* JE */
	'A',	/* JN */
	'I',	/* IO */
	'N',	/* Y */
	'N',	/* D */
	'N'	/* H */
};


inline string bytecode::to_text(unsigned int pc) const
{
	std::stringbuf buf;
	std::ostream os(&buf);

	os << pc << ":" << "\t" << '(' << opcode_type[this->opcode] << ')' << opname[this->opcode] << "\t";
	switch(opcode_type[this->opcode])
	{
		case 'N':
			// No operand
			break;
		case 'O':
			// Offset
			os << this->operand.offset;
			break;
		case 'M':
			// Dual Operand
			os << this->operand.mul.offset << ", " << this->operand.mul.multiplier;
			break;
		case 'A':
			// Address
			os << this->operand.addr;
			break;
		case 'I':
			// Intermediate byte
			os << (int)this->operand.byte;
			break;
		default:
			break;
	}
	return buf.str();
}

inline pc_t wraparound_address(pc_t address, pc_t size)
{
	return (address < size) ? address : address - size;
}

inline int bytecode::execute(vector<memory_t> &memory, pc_t &pc, pc_t &ptr)
{
	pc_t size = memory.size();
	switch(opcode)
	{
		case _OP_X:
			// No operand
			cerr << "Empty instruction at: " << pc << endl;
			return _HALT;
			break;
		case _OP_A:
			// Offset
			memory[wraparound_address(ptr, size)] += operand.offset;
			break;
		case _OP_MUL:
			// Dual Operand
			memory[wraparound_address(ptr + operand.mul.offset, size)] += memory[wraparound_address(ptr, size)] * operand.mul.multiplier;
			break;
		case _OP_S:
			// Intermediate byte
			memory[wraparound_address(ptr, size)] = operand.byte;
			break;
		case _OP_F:
			// Offset
			for(auto it = memory.begin() + ptr; it != memory.end() || it >= memory.begin(); it += operand.offset)
			{
				if(*it == 0)
					break;
			}
			break;
		case _OP_M:
			// Offset
			ptr += operand.offset;
			break;
		case _OP_JE:
			// Jump if equal to 0
			if(memory[wraparound_address(ptr, size)] == 0)
				pc = operand.addr;
			break;
		case _OP_JN:
			// Jump if not equal to 0
			if(memory[wraparound_address(ptr, size)] != 0)
				pc = operand.addr;
			break;
		case _OP_IO:
			// 0: Input, 1: Output
			switch(operand.byte)
			{
				case 0:
					// Input
					cin >> memory[wraparound_address(ptr, size)];
					break;
				case 1:
					// Output
					cout << (char)memory[wraparound_address(ptr, size)] << flush;
					break;
				default:
					cerr << "Unknown IO instruction: " << operand.byte << endl;
					break;
			}
			break;
		case _OP_Y:
			// No operand, fork
			break;
		case _OP_D:
			// No opernd, debug
			break;
		case _OP_H:
			// No operand, halt
			//cerr << "Halt at: " << pc << endl;
			return _HALT;
			break;
		default:
			cerr << "Unknown instruction at: " << pc << endl;
			break;
	}
	pc++;
	return _CONT;
}

} // namespace Brainfunk
