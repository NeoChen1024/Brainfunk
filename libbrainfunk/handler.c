#include <libbrainfunk.h>

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
	[OP_ALUS] =
	{
		DEFINE(alus)
	},
	[OP_SET] =
	{
		DEFINE(set)
	},
	[OP_POP] =
	{
		DEFINE(pop)
	},
	[OP_PUSH] =
	{
		DEFINE(push)
	},
	[OP_PSHI] =
	{
		DEFINE(pshi)
	},
	[OP_MOV] =
	{
		DEFINE(mov)
	},
	[OP_STP] =
	{
		DEFINE(stp)
	},
	[OP_JMP] =
	{
		DEFINE(jmp)
	},
	[OP_JEZ] =
	{
		DEFINE(jez)
	},
	[OP_JNZ] =
	{
		DEFINE(jnz)
	},
	[OP_JSEZ] =
	{
		DEFINE(jsez)
	},
	[OP_JSNZ] =
	{
		DEFINE(jsnz)
	},
	[OP_IO] =
	{
		DEFINE(io)
	},
	[OP_FRK] =
	{
		DEFINE(frk)
	},
	[OP_INV] =
	{
		DEFINE(inv)
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

EXEC(alus)
{
	return CONT;
}

SCAN(alus)
{
	return LEXERR;
}

SCAN(set)
{
	return LEXERR;
}

EXEC(set)
{
	cpu->mem[cpu->ptr] = cpu->code[cpu->pc].arg;
	return CONT;
}

EXEC(pop)
{
	return CONT;
}

SCAN(pop)
{
	return LEXERR;
}

EXEC(push)
{
	return CONT;
}

SCAN(push)
{
	return LEXERR;
}

EXEC(pshi)
{
	return CONT;
}

SCAN(pshi)
{
	return LEXERR;
}

SCAN(mov)
{
	return LEXERR;
}

EXEC(mov)
{
	cpu->ptr += cpu->code[cpu->pc].arg;
	return CONT;
}

EXEC(stp)
{
	return CONT;
}

SCAN(stp)
{
	return LEXERR;
}

EXEC(jmp)
{
	return CONT;
}

SCAN(jmp)
{
	return LEXERR;
}

SCAN(jez)
{
	return LEXERR;
}

EXEC(jez)
{
	if(cpu->mem[cpu->ptr] == 0)
		cpu->pc = cpu->code[cpu->pc].arg;
	return CONT;
}

SCAN(jnz)
{
	return LEXERR;
}

EXEC(jnz)
{
	if(cpu->mem[cpu->ptr] != 0)
		cpu->pc = cpu->code[cpu->pc].arg;
	return CONT;
}

EXEC(jsez)
{
	return CONT;
}

SCAN(jsez)
{
	return LEXERR;
}

EXEC(jsnz)
{
	return CONT;
}

SCAN(jsnz)
{
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

EXEC(frk)
{
	return CONT;
}

SCAN(frk)
{
	return LEXERR;
}


SCAN(inv)
{
	return LEXERR;
}

EXEC(inv)
{
	return HALT;
}
