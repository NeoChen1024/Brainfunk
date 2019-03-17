#include <libbrainfunk.h>

#define DEFINE(name)			\
	.exec = &exec_ ## name,		\
	.scan = &scan_ ## name

#define EXEC(name)	int exec_ ## name(brainfunk_t cpu)
#define SCAN(name)	arg_t scan_ ## name(bitcode_t code, arg_t pc, arg_t *textptr, char *text)

handler_t handler[OP_INSTS] =
{
	[OP_HLT] =
	{
		DEFINE(hlt)
	},
	[OP_ALU] =
	{
		DEFINE(alu)
	},
	[OP_IO] =
	{
		DEFINE(io)
	}
};

EXEC(hlt)
{
	return HALT;
}

SCAN(hlt)
{
	if(text[*textptr] == '_')
	{
		code->op = OP_HLT;
		(*textptr)++;
		return 1;
	}
	else
		return LEXERR;
}

EXEC(io)
{
	switch(cpu->code[cpu->pc].arg)
	{
		case IO_IN:
			cpu->mem[cpu->ptr] = io_in();
			break;
		case IO_OUT:
			io_out(cpu->mem[cpu->ptr]);
			break;
		case IO_INS:
		case IO_OUTS:
			break;
	}
	return CONT;
}

SCAN(io)
{
	if(text[*textptr] == '.')
	{
		code[pc].op = OP_IO;
		code[pc].arg = IO_OUT;
		(*textptr)++;
		return 1;
	}
	else if (text[*textptr] == ',')
	{
		code[pc].op = OP_IO;
		code[pc].arg = IO_IN;
		(*textptr)++;
		return 1;
	}
	else
		return LEXERR;
}

EXEC(alu)
{
	cpu->mem[cpu->ptr] += cpu->code[cpu->pc].arg;
	return CONT;
}

SCAN(alu)
{
	arg_t plus=0;
	arg_t minus=0;
	while(text[*textptr] == '+' || text[*textptr] == '-')
	{
		if(text[*textptr] == '+')
			plus++;
		else if(text[*textptr] == '-')
			minus++;
		(*textptr)++;
	}
	if(plus == 0 && minus == 0)
		return LEXERR;
	else
	{
		code[pc].op = OP_ALU;
		code[pc].arg = plus - minus;
		return 1;
	}
}
