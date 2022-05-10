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

	std::fill(this->memory.begin(), this->memory.end(), 0);
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
	size_t match_len=0;
	int pairs = 0;
	string substr = text;

	class Bytecode t;

	regex preg("^[><]+[+-]+", regex::extended | regex::optimize);
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
		substr = substr.substr(2);
		mode = 1;
	}
	else
	{
		substr = substr.substr(1);
		mode = 2;
	}

	i = 0; /* Reuse it as index */
	while(regex_search(substr, m, preg))
	{
		count_mul_offset(m.begin()->str(), mul, offset, offset.size() == 0 ? 0 : offset.back());
		match_len = m.begin()->length();
		substr = substr.substr(match_len);
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

SCAN(y)
{
	class Bytecode t(_OP_Y);
	bytecode.push_back(t);
	return true;
}

SCAN(h)
{
	class Bytecode t(_OP_H);
	bytecode.push_back(t);
	return true;
}

SCAN(d)
{
	class Bytecode t(_OP_D);
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
		SCAN_HANDLER_DEF(y,	"^~"),	/* Y */
		SCAN_HANDLER_DEF(h,	"^%"),	/* H */
		SCAN_HANDLER_DEF(d,	"^#")	/* D */
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
		//cerr << "Loop: " << code << endl;
		for(auto &it : patterns)
		{
			//cerr << "Pattern: " << it.regexp << endl;
			if(regex_search(code, m, it.pattern))
			{
				// Debug
				//cerr << "Match: " << m.begin()->str() << endl;
				if(it.handler(bytecode, stack, m[0].str()))
				{
					code = code.substr(m[0].length());
					//cerr << "New Code: " << code << endl;
					//cerr << "New Bytecode: " << bytecode.back().to_text(0) << endl;
					break;
				}
				else
				{
					//cerr << "handler failed: " << m.begin()->str() << endl;
				}
			}
			else
			{
				//cerr << "No match" << endl;
			}
		}
	}

	this->bytecode.push_back(Bytecode(_OP_H));
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
	'N',	/* Y */
	'N',	/* D */
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

inline string Bytecode::to_text(unsigned int pc) const
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
			os << this->operand.dual.offset << ", " << this->operand.dual.mul;
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
	return (address < size) ? address : address % size;
}

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
			memory[wraparound_address(ptr, size)] += operand.offset;
			pc++;
			break;
		case _OP_MUL:
			// Dual Operand
			memory[wraparound_address(ptr + operand.dual.offset, size)] += memory[wraparound_address(ptr, size)] * operand.dual.mul;
			pc++;
			break;
		case _OP_S:
			// Intermediate byte
			memory[wraparound_address(ptr, size)] = operand.byte;
			pc++;
			break;
		case _OP_F:
			// Offset
			for(; memory[wraparound_address(ptr, size)] != 0; ptr = wraparound_address(ptr + operand.offset, size));
			pc++;
			break;
		case _OP_M:
			// Offset
			ptr += operand.offset;
			pc++;
			break;
		case _OP_JE:
			// Jump if equal to 0
			if(memory[wraparound_address(ptr, size)] == 0)
				pc = operand.addr;
			else
				pc++;
			break;
		case _OP_JN:
			// Jump if not equal to 0
			if(memory[wraparound_address(ptr, size)] != 0)
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
			pc++;
			break;
		case _OP_Y:
			// No operand, fork
			pc++;
			break;
		case _OP_D:
			// No opernd, debug
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
