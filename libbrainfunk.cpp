#include "libbrainfunk.hpp"

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
	this->pc = 0;
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
	mul.push_back(count_continus(text, "+-") % 256);
	offset.push_back(count_continus(text, "><") + lastoffset);
	return;
}

#define SCAN(name) \
	bool (scan_ ## name)(vector<Bitcode> &bitcode, vector<addr_t> &stack, string const &text)

SCAN(smul)
{
	ssize_t i=0;
	int mode=0;
	vector<memory_t> mul;
	vector<offset_t> offset;
	ssize_t pairs = 0;
	string substr = text;

	class Bitcode t;

	regex preg("^[><]+[+-]+", regex::optimize);
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

	while(regex_search(substr, m, preg))
	{
		count_mul_offset(m.begin()->str(), mul, offset, offset.size() == 0 ? 0 : offset.back());
		substr.erase(0, m.begin()->length());
	}

	assert(pairs > 0);
	/* Omit the last false pair in mode 2 */
	if(mode == 2)
		pairs = offset.size() - 1;
	else
		pairs = offset.size();

	for(int i = 0; i < pairs; i++)
	{
		bitcode.push_back(Bitcode(_OP_MUL, {.dual = {mul[i], offset[i]}}));
	}

	/* Insert a "S 0" to get correct behavior */
	bitcode.push_back(Bitcode(_OP_S, {.byte = 0}));

	return true;
}

SCAN(s0)
{
	class Bitcode t(_OP_S, {.byte = 0});
	bitcode.push_back(t);

	return true;
}

SCAN(f)
{
	offset_t offset = count_continus(text, "><");

	class Bitcode t(_OP_F, {.offset = offset});
	bitcode.push_back(t);

	return true;
}

SCAN(a)
{
	offset_t offset=0;

	offset = count_continus(text, "+-");

	class Bitcode t(_OP_A, {.offset = offset});
	bitcode.push_back(t);

	return true;
}

SCAN(m)
{
	offset_t offset=0;
	offset = count_continus(text, "><");
	class Bitcode t(_OP_M, {.offset = offset});
	bitcode.push_back(t);

	return true;
}

SCAN(je)
{
	class Bitcode t(_OP_JE);
	bitcode.push_back(t);

	stack.push_back(bitcode.size() - 1);

	return true;
}

SCAN(jn)
{
	addr_t last_pc = stack.back();
	stack.pop_back();

	class Bitcode t(_OP_JN, {.addr = last_pc + 1});
	bitcode.push_back(t);
	/* Skip the je & jn instructions themselves, this improves speed a little */
	bitcode[last_pc].operand.addr = bitcode.size();

	return true;
}

SCAN(io)
{
	class Bitcode t;
	if(text[0] == ',')
		t.operand.byte = _IO_IN;
	else if(text[0] == '.')
		t.operand.byte = _IO_OUT;

	t.opcode = _OP_IO;
	bitcode.push_back(t);
	return true;
}

SCAN(split)
{
	return true;
}

#define SCAN_HANDLER_DEF(name, pattern) \
	{&scan_ ## name, regex(pattern, regex::optimize), pattern}

struct code_patterns patterns[] =
{
		SCAN_HANDLER_DEF(split,	"^!"),	/* Doesn't emit instruction */
		SCAN_HANDLER_DEF(smul,	"^\\[-([<>]+[+-]+)+[<>]+]"),	/* S 0 & MUL */
		SCAN_HANDLER_DEF(smul,	"^\\[([<>]+[+-]+)+[<>]+-]"),	/* S 0 & MUL */
		SCAN_HANDLER_DEF(f,	"^\\[[><]+\\]"),	/* F + / - */
		SCAN_HANDLER_DEF(s0,	"^\\[-\\]"),	/* S 0 */
		SCAN_HANDLER_DEF(a,	"^[+-]+"),	/* A */
		SCAN_HANDLER_DEF(m,	"^[<>]+"),	/* M */
		SCAN_HANDLER_DEF(je,	"^\\["),	/* JE */
		SCAN_HANDLER_DEF(jn,	"^\\]"),	/* JN */
		SCAN_HANDLER_DEF(io,	"^\\."),	/* IO OUT */
		SCAN_HANDLER_DEF(io,	"^,"),	/* IO IN */
};

void Brainfunk::translate(string &text)
{
	vector<addr_t> stack;	// Jump address stack
	string code = text;
	this->bitcode.clear();
	class Bitcode current_instruction(_OP_X);
	smatch m;
	
	if(count_continus(code, "[]") != 0)
	{
		cerr << "Error: Brace mismatch" << endl;
		exit(1);
	}

	while(code.length() > 0)
	{
		for(auto &it : patterns)
		{
			if(regex_search(code, m, it.pattern))
			{
				if(it.handler(bitcode, stack, m[0].str()))
				{
					code.erase(0, m[0].length());
					break;
				}
			}
		}
	}

	this->bitcode.push_back(Bitcode(_OP_H));
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

void Brainfunk::run()
{
	this->pc = 0;
	std::fill(this->memory.begin(), this->memory.end(), 0);

	if(this->bitcode.size() == 0)
		return;
	while(this->bitcode[this->pc].execute(this->memory, this->pc, this->ptr));
}

string Bitcode::opname[_OP_INSTS] =
{
	"X",
	"A",
	"S",
	"MUL",
	"F",
	"M",
	"JE",
	"JN",
	"IO",
	"H"
};

/* operand type of the opcode,
 *
 * N => None
 * O => Offset
 * M => mul's Dual Operand
 * A => Address
 * I => Intermediate
 */

char Bitcode::opcode_type[_OP_INSTS] =
{
	'N',	/* X */
	'O',	/* A */
	'I',	/* S */
	'M',	/* MUL */
	'O',	/* F */
	'O',	/* M */
	'A',	/* JE */
	'A',	/* JN */
	'I',	/* IO */
	'N'	/* H */
};

Bitcode::Bitcode()
{
	return;
}

Bitcode::Bitcode(uint8_t opcode, operand_s operand)
{
	this->opcode = opcode;
	this->operand = operand;
}

Bitcode::Bitcode(uint8_t opcode)
{
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
		case 'A':
			// Address
			operand << this->operand.addr;
			break;
		case 'I':
			// Intermediate byte
			operand << (int)this->operand.byte;
			break;
		default:
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

inline bool Bitcode::execute(vector<memory_t> &memory, addr_t &pc, addr_t &ptr)
{
	addr_t size = memory.size();
	switch(opcode)
	{
		case _OP_X:
			// No operand
			cerr << "Empty instruction at: " << pc << endl;
			return false;
			break;
		case _OP_A:
			// Offset
			memory[wrap(ptr, size)] += operand.offset;
			pc++;
			break;
		case _OP_S:
			// Intermediate byte
			memory[wrap(ptr, size)] = operand.byte;
			pc++;
			break;
		case _OP_MUL:
			// Dual Operand
			memory[wrap(ptr + operand.dual.offset, size)] += memory[wrap(ptr, size)] * operand.dual.mul;
			pc++;
			break;
		case _OP_F:
			// Offset
			for(; memory[wrap(ptr, size)] != 0; ptr = wrap(ptr + operand.offset, size));
			pc++;
			break;
		case _OP_M:
			// Offset
			ptr = wrap(ptr + operand.offset, size);
			pc++;
			break;
		case _OP_JE:
			// Jump if equal to 0
			if(memory[wrap(ptr, size)] == 0)
				pc = operand.addr;
			else
				pc++;
			break;
		case _OP_JN:
			// Jump if not equal to 0
			if(memory[wrap(ptr, size)] != 0)
				pc = operand.addr;
			else
				pc++;
			break;
		case _OP_IO:
			// 0: Input, 1: Output
			switch(operand.byte)
			{
				case 0:
					// Input
					cin >> memory[wrap(ptr, size)];
					break;
				case 1:
					// Output
					cout << (char)memory[wrap(ptr, size)] << flush;
					break;
				default:
					cerr << "Unknown IO instruction: " << operand.byte << endl;
					return false;
					break;
			}
			pc++;
			break;
		case _OP_H:
			// No operand, halt
			return false;
			break;
		default:
			cerr << "Unknown instruction at: " << pc << endl;
			return false;
			break;
	}
	return true;
}
