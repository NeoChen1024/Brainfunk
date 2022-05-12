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
using std::regex_match;
using std::regex_search;
using std::smatch;

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
}

void Brainfunk::clear()
{
	std::fill(this->memory.begin(), this->memory.end(), 0);
	this->ptr = 0;
	this->stack.clear();
	this->bytecode.clear();
}

Brainfunk::~Brainfunk()
{
	this->memory.clear();
	this->stack.clear();
	this->bytecode.clear();
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

void count_mul_offset(string const &text, vector<offset_t> &mul, vector<offset_t> &offset, size_t lastoffset)
{
	mul.push_back(count_continus(text, "+-"));
	offset.push_back(count_continus(text, "><") + lastoffset);
	return;
}

#define SCAN(name) \
	bool (scan_ ## name)(vector<Bytecode> &bytecode, vector<pc_t> &stack, string const &text)

SCAN(smul)
{
	ssize_t i=0;
	int mode=0;
	vector<offset_t> mul;
	vector<offset_t> offset;
	ssize_t pairs = 0;
	string substr = text;

	class Bytecode t;

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

	/* Omit the last false pair in mode 2 */
	if(mode == 2)
		pairs = offset.size() - 1;
	else
		pairs = offset.size();

	for(int i = 0; i < pairs; i++)
	{
		bytecode.push_back(Bytecode(_OP_MUL, {.dual = {mul[i], offset[i]}}));
	}

	/* Insert a "S 0" to get correct behavior */
	bytecode.push_back(Bytecode(_OP_S, {.byte = 0}));

	return true;
}

SCAN(s0)
{
	class Bytecode t(_OP_S, {.byte = 0});
	bytecode.push_back(t);

	return true;
}

SCAN(f)
{
	offset_t offset = count_continus(text, "><");

	class Bytecode t(_OP_F, {.offset = offset});
	bytecode.push_back(t);

	return true;
}

SCAN(a)
{
	offset_t offset=0;

	offset = count_continus(text, "+-");

	class Bytecode t(_OP_A, {.offset = offset});
	bytecode.push_back(t);

	return true;
}

SCAN(m)
{
	offset_t offset=0;
	offset = count_continus(text, "><");
	class Bytecode t(_OP_M, {.offset = offset});
	bytecode.push_back(t);

	return true;
}

SCAN(je)
{
	class Bytecode t(_OP_JE);
	bytecode.push_back(t);

	stack.push_back(bytecode.size() - 1);

	return true;
}

SCAN(jn)
{
	pc_t last_pc = stack.back();
	stack.pop_back();

	class Bytecode t(_OP_JN, {.addr = last_pc + 1});
	bytecode.push_back(t);
	/* Skip the je & jn instructions themselves, this improves speed a little */
	bytecode[last_pc].operand.addr = bytecode.size();

	return true;
}

SCAN(io)
{
	class Bytecode t;
	if(text[0] == ',')
		t.operand.byte = _IO_IN;
	else if(text[0] == '.')
		t.operand.byte = _IO_OUT;

	t.opcode = _OP_IO;
	bytecode.push_back(t);
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
	string code = text;
	this->bytecode.clear();
	class Bytecode current_instruction(_OP_X);
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
				if(it.handler(bytecode, stack, m[0].str()))
				{
					code.erase(0, m[0].length());
					break;
				}
			}
		}
	}

	this->bytecode.push_back(Bytecode(_OP_H));
}

void Brainfunk::dump_code(ostream &os, enum formats format)
{
	if(format == FMT_C)
	{
		os << "#include <stdio.h>\n"
		"#include <stdlib.h>\n"
		"#include <stdint.h>\n"
		"uint8_t *mem;\n"
		"#define MEMSIZE		(1<<21)\n"
		"#define	a(x)	*mem += x\n"
		"#define	mul(offset, mul)	mem[offset] += *mem * mul\n"
		"#define	s(x)	*mem = x\n"
		"#define	f(x)	while(*mem != 0) mem += x\n"
		"#define	m(x)	mem += x\n"
		"#define	je(x)	while(*mem) {\n"
		"#define	jn(x)	}\n"
		"#define	h()	exit(0);\n"
		"static inline void io(uint8_t arg)\n"
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
		"	if(!mem) { puts(\"Out of memory\"); exit(1); }\n" << endl;
	}
	for(pc_t pc = 0; pc < bytecode.size(); pc++)
	{
		if(format != FMT_C)
			os << pc << ":\t" << this->bytecode[pc].to_text(format) << endl;
		else
			os << this->bytecode[pc].to_text(format) << endl;
	}
	if(format == FMT_C)
	{
		os << "}" << endl;
	}
}

void Brainfunk::run()
{
	int ret = _CONT;
	this->pc = 0;
	std::fill(this->memory.begin(), this->memory.end(), 0);

	while(ret == _CONT && !this->bytecode.empty())
	{
		if(this->debug)
		{
			cerr << "I = " << this->pc << ":\t" << this->bytecode[this->pc].to_text()
			<< "\tM @(" << this->ptr << ") =" << (int)this->memory[this->ptr]
			<< endl;
		}
		ret = this->bytecode[this->pc].execute(this->memory, this->pc, this->ptr);
	}
}

string Bytecode::opname[_OP_INSTS] =
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

char Bytecode::opcode_type[_OP_INSTS] =
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
	'N'	/* H */
};

Bytecode::Bytecode()
{
	return;
}

Bytecode::Bytecode(uint8_t opcode, operand_s operand)
{
	this->opcode = opcode;
	this->operand = operand;
}

Bytecode::Bytecode(uint8_t opcode)
{
	this->opcode = opcode;
}

inline string Bytecode::to_text(enum formats format) const
{
	std::stringstream operand;
	std::stringstream output;
	std::string opcode_name = opname[this->opcode];

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
			operand << this->operand.dual.offset << ", " << this->operand.dual.mul;
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
		case FMT_BF:
		output << '(' << opcode_type[this->opcode] << ')' << opcode_name << "\t" << operand.str();
			break;
		case FMT_M:
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

inline int Bytecode::execute(vector<memory_t> &memory, pc_t &pc, pc_t &ptr)
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
			memory[wrap(ptr, size)] += operand.offset;
			pc++;
			break;
		case _OP_MUL:
			// Dual Operand
			memory[wrap(ptr + operand.dual.offset, size)] += memory[wrap(ptr, size)] * operand.dual.mul;
			pc++;
			break;
		case _OP_S:
			// Intermediate byte
			memory[wrap(ptr, size)] = operand.byte;
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
					break;
			}
			pc++;
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
	return _CONT;
}

} // namespace Brainfunk
