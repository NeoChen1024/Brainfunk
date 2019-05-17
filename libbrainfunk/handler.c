#include <libbrainfunk.h>

#define ADV	1	/* Advance 1 character at a time */

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

#define DIFF	-1
#define SAME	0

int lexcmp(char *code, char *lex)
{
	arg_t i=0;
	while(lex[i] != '\0')
	{
		if(code[i] != lex[i])
			return DIFF;
		else
			i++;
	}
	return SAME;
}

EXEC(hlt)
{
	return HALT;
}

SCAN(hlt)
{
	if(text[*textptr] == '_')
	{
		code->op = OP_HLT;
		++*textptr;
		++*pc;
		return ADV;
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
		code[*pc].op = OP_ALU;
		code[*pc].arg = plus - minus;
		++*pc;
		return ADV;
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
	if(lexcmp(text + *textptr, "[-]") == SAME)
	{
		code[*pc].op = OP_SET;
		code[*pc].arg = 0;
		*textptr += 3; /* 3 characters, so increment by 3 */
		++*pc;
		return ADV;
	}
	return LEXERR;
}

EXEC(set)
{
	cpu->mem[cpu->ptr] = cpu->code[cpu->pc].arg;
	return CONT;
}

EXEC(pop)
{
	cpu->mem[cpu->ptr] = pop(cpu);
	return CONT;
}

SCAN(pop)
{
	if(text[*textptr] == '\\')
	{
		code[*pc].op = OP_POP;
		code[*pc].arg = 0;
		++*textptr;
		++*pc;
		return ADV;
	}
	return LEXERR;
}

EXEC(push)
{
	push(cpu, cpu->mem[cpu->ptr]);
	return CONT;
}

SCAN(push)
{
	if(text[*textptr] == '/')
	{
		code[*pc].op = OP_PUSH;
		code[*pc].arg = 0;
		++*textptr;
		++*pc;
		return ADV;
	}
	return LEXERR;
}

EXEC(pshi)
{
	push(cpu, cpu->code[cpu->pc].arg);
	return CONT;
}

SCAN(pshi)
{
	return LEXERR;
}

SCAN(mov)
{
	arg_t forward=0;
	arg_t backward=0;
	while(text[*textptr] == '>' || text[*textptr] == '<')
	{
		if(text[*textptr] == '>')
			forward++;
		else if(text[*textptr] == '<')
			backward++;
		(*textptr)++;
	}
	if(forward == 0 && backward == 0)
		return LEXERR;
	else
	{
		code[*pc].op = OP_MOV;
		code[*pc].arg = forward - backward;
		++*pc;
		return ADV;
	}
}

EXEC(mov)
{
	if(cpu->ptr + cpu->code[cpu->pc].arg < 0)
		cpu->ptr += cpu->code[cpu->pc].arg + cpu->size.mem;
	else if(cpu->ptr + cpu->code[cpu->pc].arg >= cpu->size.mem)
		cpu->ptr += cpu->code[cpu->pc].arg - cpu->size.mem;
	else
		cpu->ptr += cpu->code[cpu->pc].arg;
	return CONT;
}

EXEC(stp)
{
	cpu->ptr = cpu->code[cpu->pc].arg;
	return CONT;
}

SCAN(stp)
{
	return LEXERR;
}

EXEC(jmp)
{
	cpu->pc = cpu->code[cpu->pc].arg;
	return CONT;
}

SCAN(jmp)
{
	return LEXERR;
}

SCAN(jez)
{
	if(text[(*textptr)] == '[')
	{
		code[*pc].op = OP_JEZ;
		pcstack_push(pcstack, *pc);
		++*textptr;
		++*pc;
		return ADV;
	}
	else
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
	arg_t temp_pc=0;

	if(text[(*textptr)] == ']')
	{
		temp_pc = pcstack_pop(pcstack);
		code[*pc].op = OP_JNZ;
		code[*pc].arg = temp_pc;
		code[temp_pc].arg = *pc;
		++*textptr;
		++*pc;
		return ADV;
	}
	else
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
	if(pop(cpu) == 0)
		cpu->pc = cpu->code[cpu->pc].arg;
	return CONT;
}

SCAN(jsez)
{
	return LEXERR;
}

EXEC(jsnz)
{
	if(pop(cpu) != 0)
		cpu->pc = cpu->code[cpu->pc].arg;
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
			push(cpu, io_in());
			break;
		case IO_OUTS:
			io_out(pop(cpu));
			break;
	}
	return CONT;
}

SCAN(io)
{
	if(text[*textptr] == '.')
	{
		code[*pc].op = OP_IO;
		code[*pc].arg = IO_OUT;
		(*textptr)++;
		++*pc;
		return ADV;
	}
	else if (text[*textptr] == ',')
	{
		code[*pc].op = OP_IO;
		code[*pc].arg = IO_IN;
		(*textptr)++;
		++*pc;
		return ADV;
	}
	else
		return LEXERR;
}

EXEC(frk)
{
	cpu->mem[cpu->ptr] = (data_t)fork();
	return CONT;
}

SCAN(frk)
{
	if(text[(*textptr)++] == '~')
	{
		code[*pc].op = OP_FRK;
		++*pc;
		return ADV;
	}
	else
		return LEXERR;
}


SCAN(inv)
{
	return LEXERR;
}

EXEC(inv)
{
	panic("?INV");
	return HALT;
}
