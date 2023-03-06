#include "libbrainfunk.hpp"
#include "ctre.hpp"

using std::string;
using std::string_view;
using std::stringstream;
using std::flush;
using std::cerr;
using std::endl;
using std::vector;

BrainfunkException::BrainfunkException(const string &msg)
{
	this->msg = msg;
}

BrainfunkException::~BrainfunkException() throw()
{
	this->msg.clear();
}

const char* BrainfunkException::what() const throw()
{
	return this->msg.c_str();
}

Brainfunk::Brainfunk(size_t memsize)
{
	this->ptr = 0;
	try
	{
		this->memory.resize(memsize);
		std::fill(this->memory.begin(), this->memory.end(), 0);
		this->bitcode.reserve(memsize);
	}
	catch(const std::exception& e)
	{
		std::cerr << e.what() << '\n';
	}
}

void Brainfunk::clear()
{
	std::fill(this->memory.begin(), this->memory.end(), 0);
	this->ptr = 0;
	this->bitcode.clear();
}

Brainfunk::~Brainfunk()
{
	this->memory.clear();
	this->bitcode.clear();
}

ssize_t count_continus(const string_view &text, const string &symbolset)
{
	size_t i=0;
	ssize_t ctr=0;

	assert(symbolset.length() == 2);

	while(i < text.length())
	{
		if(text[i] == symbolset[0])
			ctr++;
		else if(text[i] == symbolset[1])
			ctr--;
		i++;
	}
	return ctr;
}

size_t find_continus(const string_view &text, const string &symbolset, ssize_t &value)
{
	size_t i=0;
	ssize_t ctr=0;

	assert(symbolset.length() == 2);

	while(i < text.length())
	{
		if(text[i] == symbolset[0])
			ctr++;
		else if(text[i] == symbolset[1])
			ctr--;
		else
			break;
		i++;
	}
	value = ctr;
	return i;
}

void count_mul_offset(const string_view &text, vector<memory_t> &mul, vector<offset_t> &offset, size_t lastoffset)
{
	mul.emplace_back(count_continus(text, "+-") % 256);
	offset.emplace_back(count_continus(text, "><") + lastoffset);
	return;
}

void Brainfunk::translate(const string &text)
{
	vector<addr_t> stack;	// Jump address stack
	this->bitcode.clear();

	string_view code = text;

	if(count_continus(code, "[]") != 0)
	{
		throw BrainfunkException("Unmatched brackets");
	}

	addr_t last_pc = 0;
	ssize_t value = 0;

	while(code.length() > 0)
	{
		switch(code.front())
		{
		case '[':
			if(auto m = ctre::starts_with<"^\\[(\\-([\\<\\>]+[\\+\\-]+)+[\\<\\>]+|([\\<\\>]+[\\+\\-]+)+[\\<\\>]+\\-)\\]">(code))
			{
				int mode = 0;
				vector<memory_t> mul;
				vector<offset_t> offset;
				ssize_t pairs = 0;
				string_view substr = m.to_view();

				/* First we need to validate if it goes back to where it was */
				if(count_continus(m.to_view(), "><") != 0)
				{
					goto bailout;	// Not a valid mul-offset loop
				}

				/* Basically, the text will look either like:
				 *
				 *	1. [->>++++>>>>++++++++<<--<<<<]
				 *
				 *	or
				 *
				 *	2. [>>+++++>>>>+++<<---<<<<-]
				 */

				if (substr[1] == '-')
				{
					substr.remove_prefix(2);
					mode = 1;
				}
				else
				{
					substr.remove_prefix(1);
					mode = 2;
				}

				while (auto m = ctre::starts_with<"^[\\>\\<]+[\\+\\-]+">(substr))
				{
					count_mul_offset(m.to_view(), mul, offset, offset.size() == 0 ? 0 : offset.back());
					substr.remove_prefix(m.size());
				}

				/* Omit the last false pair in mode 2 */
				if (mode == 2)
					pairs = offset.size() - 1;
				else
					pairs = offset.size();

				assert(pairs > 0);
				for (int i = 0; i < pairs; i++)
				{
					bitcode.emplace_back(Bitcode(_OP_MUL, mul[i], offset[i]));
				}

				/* Insert a "S 0" to get correct behavior */
				bitcode.emplace_back(Bitcode(_OP_S, (memory_t)0));

				code.remove_prefix(m.size());
				continue;
			}
			else if(auto m = ctre::starts_with<"^\\[[\\>\\<]+\\]">(code)) // F instruction
			{
				offset_t offset = count_continus(m.to_view(), "><");
				bitcode.emplace_back(Bitcode(_OP_F, offset));

				code.remove_prefix(m.size());
				continue;
			}
			else if(auto m = ctre::starts_with<"^\\[[\\+\\-]+\\]">(code)) // S 0
			{
				auto v = count_continus(m.to_view(), "+-");
				if(v % 2 != 1) // is even
				{
					goto bailout;
				}
				bitcode.emplace_back(Bitcode(_OP_S, (memory_t)0));

				code.remove_prefix(m.size());
				continue;
			}

			// No match, just a normal loop
bailout:
			bitcode.emplace_back(Bitcode());
			stack.emplace_back(bitcode.size() - 1);

			code.remove_prefix(1);
			continue;
		case ']':
			assert(stack.size() > 0);
			last_pc = stack.back();
			stack.pop_back();

			bitcode.emplace_back(Bitcode(_OP_JN, (offset_t)(last_pc - bitcode.size())));
			bitcode[last_pc] = Bitcode(_OP_JE, (offset_t)((bitcode.size() - 1) - last_pc));

			code.remove_prefix(1);
			continue;
		case '+':
		case '-':
			code.remove_prefix(find_continus(code, "+-", value));
			bitcode.emplace_back(Bitcode(_OP_A, (memory_t)value));
			
			continue;
		case '>':
		case '<':
			code.remove_prefix(find_continus(code, "><", value));
			bitcode.emplace_back(Bitcode(_OP_M, (offset_t)value));

			continue;
		case '.':
			bitcode.emplace_back(Bitcode(_OP_IO, (memory_t)_IO_OUT));
			code.remove_prefix(1);
			continue;
		case ',':
			bitcode.emplace_back(Bitcode(_OP_IO, (memory_t)_IO_IN));
			code.remove_prefix(1);
			continue;
		default: // unrecognized characters
			code.remove_prefix(1);
			continue;
		}
	}

	this->bitcode.emplace_back(Bitcode(_OP_H));
}

void Brainfunk::dump(ostream &os, enum formats format) const
{
	if(format == FMT_C)
	{
		os << "#include <stdio.h>\n"
		"#include <stdlib.h>\n"
		"#include <stdint.h>\n"
		"uint8_t *mem;\n"
		"#define MEMSIZE		(1<<21)\n"
		"#define	X(x)	/* NOP */\n"
		"#define	A(x)	*mem += x\n"
		"#define	S(x)	*mem = x\n"
		"#define	MUL(offset, mul)	mem[offset] += *mem * mul\n"
		"#define	F(x)	while(*mem != 0) mem += x\n"
		"#define	M(x)	mem += x\n"
		"#define	JE(x)	while(*mem) {\n"
		"#define	JN(x)	}\n"
		"#define	H()	exit(0);\n"
		"static inline void IO(uint8_t arg)\n"
		"{\n"
		"	int c = 0;\n"
		"	switch(arg)\n"
		"	{\n"
		"		case 0: /* IN */ c = getchar(); *mem = c == EOF ? 0 : c; break;\n"
		"		case 1: /* OUT */ putchar(*mem); break;\n"
		"	}\n"
		"}\n"
		"int main(void)\n"
		"{\n"
		"	setvbuf(stdin, NULL, _IONBF, 0);\n"
		"	setvbuf(stdout, NULL, _IONBF, 0);\n"
		"	mem = (uint8_t *)calloc(sizeof(uint8_t), MEMSIZE) + MEMSIZE/2;\n"
		"	if(!mem) { puts(\"Out of memory\"); exit(1); }\n\n";
	}
	for(addr_t pc = 0; pc < bitcode.size(); pc++)
	{
		if(format == FMT_BIT)
			os << pc << ":\t" << this->bitcode[pc].to_string(format) << endl;
		else if(format == FMT_C)
			os << this->bitcode[pc].to_string(format) << endl;
	}
	if(format == FMT_C)
	{
		os << "}" << endl;
	}
}

void Brainfunk::run(istream &is, ostream &os)
{
	auto codeit = this->bitcode.begin();
	std::fill(this->memory.begin(), this->memory.end(), 0);

	if(this->bitcode.size() == 0)
		return;
	while(codeit->execute(this->memory, codeit, this->ptr, is, os));
}

// Bitcode class

Bitcode::Bitcode()
{
	this->opcode = _OP_X; // generate invalid opcode by default
	return;
}

Bitcode::Bitcode(uint8_t opcode, memory_t operand)
{
	if(opcode_type[opcode] != 'I')
	{
		throw BrainfunkException("Invalid opcode for intermediate instruction");
	}
	this->opcode = opcode;
	this->operand.byte = operand;
}

Bitcode::Bitcode(uint8_t opcode, offset_t operand)
{
	if(opcode_type[opcode] != 'O')
	{
		throw BrainfunkException("Invalid opcode for offset instruction");
	}
	this->opcode = opcode;
	this->operand.offset = operand;
}

Bitcode::Bitcode(uint8_t opcode, memory_t mul, offset_t offset)
{
	if(opcode_type[opcode] != 'M')
	{
		throw BrainfunkException("Invalid opcode for MUL instruction");
	}
	this->opcode = opcode;
	this->operand.dual.mul = mul;
	this->operand.dual.offset = offset;
}

Bitcode::Bitcode(uint8_t opcode) // for instructions with no operand
{
	if(opcode_type[opcode] != 'N')
	{
		throw BrainfunkException("Invalid opcode for instruction with no operand");
	}
	this->opcode = opcode;
}

inline const string Bitcode::to_string(enum formats format) const
{
	stringstream operand;
	stringstream output;
	string opcode_name = opname[this->opcode];

	switch(opcode_type[this->opcode])
	{
		case 'N':
			// No operand
			operand << "";
			break;
		case 'O':
			// Offset
			operand << this->operand.offset;
			break;
		case 'M':
			// Dual Operand
			operand << this->operand.dual.offset << ", " << (unsigned short int)this->operand.dual.mul;
			break;
		case 'I':
			// Intermediate byte
			operand << (unsigned short int)this->operand.byte;
			break;
		default:
			throw BrainfunkException("Invalid opcode operand type");
			break;
	}
	switch(format)
	{
		case FMT_BIT:
			output << opcode_name << "\t" << operand.str();
			break;
		case FMT_C:
			output << opcode_name << "(" << operand.str() << ");";
			break;
	}
	return output.str();
}

#define wrap(address, size) \
	(((address) < (size)) ? (address) : (address) % (size))

inline bool Bitcode::execute(vector<memory_t> &memory, vector<Bitcode>::iterator &codeit, addr_t &ptr, istream &is, ostream &os) const
{
	memory_t io_input = 0;
	addr_t size = memory.size();
	switch(opcode)
	{
		case _OP_X:
			// No operand
			throw BrainfunkException("Empty instruction");
			break;
		case _OP_A:
			// Offset
			memory[ptr] += operand.byte;
			break;
		case _OP_S:
			// Intermediate byte
			memory[ptr] = operand.byte;
			break;
		case _OP_MUL:
			// Dual Operand
			memory[wrap(ptr + operand.dual.offset, size)] += memory[ptr] * operand.dual.mul;
			break;
		case _OP_F:
			// Offset
			for(; memory[ptr] != 0; ptr = wrap(ptr + operand.offset, size));
			break;
		case _OP_M:
			// Offset
			ptr = wrap(ptr + operand.offset, size);
			break;
		case _OP_JE:
			// Jump if equal to 0
			if(memory[ptr] == 0)
				codeit += operand.offset;
			break;
		case _OP_JN:
			// Jump if not equal to 0
			if(memory[ptr] != 0)
				codeit += operand.offset;
			break;
		case _OP_IO:
			// 0: Input, 1: Output
			switch(operand.byte)
			{
				case 0:
					// Input
					is >> std::noskipws >> io_input;
					if(is.eof())
						io_input = 0;
					memory[ptr] = io_input;
					break;
				case 1:
					// Output
					os << (char)memory[ptr] << flush;
					break;
			}
			break;
		case _OP_H:
			// No operand, halt
			return false;
			break;
		default:
			throw BrainfunkException("Invalid opcode");
			break;
	}
	codeit++;
	return true;
}
