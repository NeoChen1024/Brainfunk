#include "libbrainfunk.hpp"
#include "ctre.hpp"

using std::string;
using std::stringstream;
using std::cout;
using std::cin;
using std::flush;
using std::cerr;
using std::clog;
using std::endl;
using std::vector;
using std::regex;
using std::regex_match;
using std::regex_search;
using std::smatch;
using std::cmatch;

BrainfunkException::BrainfunkException(string msg)
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

bool strcmp_code(string &code, string pattern, addr_t &start)
{
	addr_t i = 0;
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

ssize_t count_continus(string const &text, string symbolset)
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

void count_mul_offset(string const &text, vector<memory_t> &mul, vector<offset_t> &offset, size_t lastoffset)
{
	mul.emplace_back(count_continus(text, "+-") % 256);
	offset.emplace_back(count_continus(text, "><") + lastoffset);
	return;
}

#define SCAN(name) \
	bool (scan_ ## name)(vector<Bitcode> &bitcode, vector<addr_t> &stack, const string &text)

static const regex mul_match("^[><]+[+-]+", regex::optimize);

SCAN(smul)
{
	ssize_t i=0;
	int mode=0;
	vector<memory_t> mul;
	vector<offset_t> offset;
	ssize_t pairs = 0;
	string substr = text;

	class Bitcode t;

	smatch m;

	/* First we need to validate if it goes back to where it was */
	i = count_continus(text, "><");
	if(i != 0)
		return false;


	/* Basically, the text will look either like:
	 *
	 *	1. [->>++++>>>>++++++++<<--<<<<]
	 *
	 *	or
	 *
	 *	2. [>>+++++>>>>+++<<---<<<<-]
	 */

	if(substr[1] == '-')
	{
		substr.erase(0, 2);
		mode = 1;
	}
	else
	{
		substr.erase(0, 1);
		mode = 2;
	}

	while(regex_search(substr, m, mul_match))
	{
		count_mul_offset(m.begin()->str(), mul, offset, offset.size() == 0 ? 0 : offset.back());
		substr.erase(0, m.begin()->length());
	}

	/* Omit the last false pair in mode 2 */
	if(mode == 2)
		pairs = offset.size() - 1;
	else
		pairs = offset.size();

	assert(pairs > 0);
	for(int i = 0; i < pairs; i++)
	{
		bitcode.emplace_back(Bitcode(_OP_MUL, mul[i], offset[i]));
	}

	/* Insert a "S 0" to get correct behavior */
	bitcode.emplace_back(Bitcode(_OP_S, (memory_t)0));

	return true;
}

SCAN(f)
{
	offset_t offset = count_continus(text, "><");

	class Bitcode t(_OP_F, offset);
	bitcode.emplace_back(t);

	return true;
}

SCAN(a)
{
	offset_t offset = count_continus(text, "+-");

	class Bitcode t(_OP_A, offset);
	bitcode.emplace_back(t);

	return true;
}

SCAN(m)
{
	offset_t offset=0;
	offset = count_continus(text, "><");
	class Bitcode t(_OP_M, offset);
	bitcode.emplace_back(t);

	return true;
}

#define SCAN_HANDLER_DEF(hname, text) \
	{.handler = &scan_ ## hname, .pattern = regex(text, regex::optimize), .regex_str = text}

static struct code_patterns patterns[] =
{
		SCAN_HANDLER_DEF(smul,	"^\\[-([<>]+[+-]+)+[<>]+]"),	/* S 0 & MUL */
		SCAN_HANDLER_DEF(smul,	"^\\[([<>]+[+-]+)+[<>]+-]"),	/* S 0 & MUL */
		SCAN_HANDLER_DEF(f,	"^\\[[><]+\\]"),	/* F + / - */
		SCAN_HANDLER_DEF(a,	"^[+-]+"),	/* A */
		SCAN_HANDLER_DEF(m,	"^[<>]+"),	/* M */
};

void Brainfunk::translate(string &text)
{
	vector<addr_t> stack;	// Jump address stack
	this->bitcode.clear();

	const char * code = text.c_str();
	size_t skip_chars = 0;

	cmatch m;
	
	if(count_continus(code, "[]") != 0)
	{
		throw BrainfunkException("Unmatched brackets");
	}

	addr_t last_pc = 0;
cont_scan:
	while(text.length() > skip_chars)
	{
		for(auto &it : patterns)
		{
			if(regex_search(code + skip_chars, m, it.pattern))
			{
				//cerr << "Matched " << it.regex_str << " with " << m[0].str() << endl;
				if(it.handler(bitcode, stack, m[0].str()))
				{
					skip_chars += m[0].length();
					goto cont_scan;
				}
			}
		}
		if(std::strncmp(code + skip_chars, "[-]", 3) == 0)
		{
			bitcode.emplace_back(Bitcode(_OP_S, (memory_t)0));

			skip_chars += 3;
			goto cont_scan;
		}
		else switch(code[skip_chars++])
		{
		case '[':
			bitcode.emplace_back(Bitcode());
			stack.emplace_back(bitcode.size() - 1);
			goto cont_scan;
		case ']':
			last_pc = stack.back();
			stack.pop_back();

			bitcode.emplace_back(Bitcode(_OP_JN, (offset_t)(last_pc - bitcode.size())));
			bitcode[last_pc] = Bitcode(_OP_JE, (offset_t)((bitcode.size() - 1) - last_pc));
			goto cont_scan;
		case '.':
			bitcode.emplace_back(Bitcode(_OP_IO, (memory_t)_IO_OUT));
			goto cont_scan;
		case ',':
			bitcode.emplace_back(Bitcode(_OP_IO, (memory_t)_IO_IN));
			goto cont_scan;
		default: // unrecognized characters
			goto cont_scan;
		}
	}

	this->bitcode.emplace_back(Bitcode(_OP_H));
}

void Brainfunk::dump(ostream &os, enum formats format)
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

inline string Bitcode::to_string(enum formats format) const
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

inline bool Bitcode::execute(vector<memory_t> &memory, vector<Bitcode>::iterator &codeit, addr_t &ptr, istream &is, ostream &os)
{
	addr_t size = memory.size();
	switch(opcode)
	{
		case _OP_X:
			// No operand
			throw BrainfunkException("Empty instruction");
			return false;
			break;
		case _OP_A:
			// Offset
			memory[wrap(ptr, size)] += operand.offset;
			break;
		case _OP_S:
			// Intermediate byte
			memory[wrap(ptr, size)] = operand.byte;
			break;
		case _OP_MUL:
			// Dual Operand
			memory[wrap(ptr + operand.dual.offset, size)] += memory[wrap(ptr, size)] * operand.dual.mul;
			break;
		case _OP_F:
			// Offset
			for(; memory[wrap(ptr, size)] != 0; ptr = wrap(ptr + operand.offset, size));
			break;
		case _OP_M:
			// Offset
			ptr = wrap(ptr + operand.offset, size);
			break;
		case _OP_JE:
			// Jump if equal to 0
			if(memory[wrap(ptr, size)] == 0)
				codeit += operand.offset;
			break;
		case _OP_JN:
			// Jump if not equal to 0
			if(memory[wrap(ptr, size)] != 0)
				codeit += operand.offset;
			break;
		case _OP_IO:
			// 0: Input, 1: Output
			static memory_t io_input = 0;
			switch(operand.byte)
			{
				case 0:
					// Input
					is >> io_input;
					if(is.eof())
						io_input = 0;
					memory[wrap(ptr, size)] = io_input;
					break;
				case 1:
					// Output
					os << (char)memory[wrap(ptr, size)] << flush;
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
