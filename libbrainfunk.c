#include <libbrainfunk.h>

#define DEFINE(name)			\
	.exec = &exec_ ## name,		\
	.scan = &scan_ ## name

#define EXEC(name)	static int exec_ ## name(brainfunk_t cpu)
#define SCAN(name)	arg_t scan_ ## name(bitcode_t code, arg_t *pc, pcstack_t pcstack, arg_t *textptr, char *text)

char opname[OP_INSTS][OPLEN] =
{
	"hlt",
	"alu",
	"alus",
	"set",
	"pop",
	"push",
	"pshi",
	"mov",
	"stp",
	"jmp",
	"jez",
	"jnz",
	"jsez",
	"jsnz",
	"io",
	"frk",
	"inv"
};

int opcode(char *name)
{
	int i=0;
	while(strcmp(name, opname[i]) != 0 && i < OP_INSTS) ++i;
	return i;
}

void panic(char *msg)
{
	fprintf(stderr, "%s\n", msg);
	quit(8);
}

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

pcstack_t pcstack_create(size_t size)
{
	pcstack_t stack = calloc(1, sizeof(struct _pcstack));
	stack->stack = calloc(size, sizeof(arg_t));
	stack->ptr = 0;
	stack->size = size;

	return stack;
}

arg_t pcstack_pop(pcstack_t stack)
{
	if(stack->ptr == 0)
		panic("?PCSTACK_UNDERFLOW");
	return stack->stack[--(stack->ptr)];
}

void pcstack_push(pcstack_t stack, arg_t data)
{
	if((size_t)stack->ptr >= stack->size)
		panic("?PCSTACK_OVERFLOW");
	stack->stack[stack->ptr++] = data;
	return;
}

void pcstack_destroy(pcstack_t *stack)
{
	free((*stack)->stack);
	free(*stack);
	*stack = NULL;
}

/* Ignore Overflow and Underflow */
void push(brainfunk_t cpu, data_t data)
{
	if((size_t)cpu->sp >= cpu->size.stack)
		return;
	cpu->stack[cpu->sp++] = data;
}

static inline data_t pop(brainfunk_t cpu)
{
	if(cpu->sp == 0)
		return 0;
	return cpu->stack[--(cpu->sp)];
}

static inline data_t peek(brainfunk_t cpu)
{
	return cpu->stack[cpu->sp - 1];
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
	cpu->pc++;
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
	push(cpu, peek(cpu) + cpu->code[cpu->pc].arg);
	cpu->pc++;
	return CONT;
}

SCAN(alus)
{
	arg_t plus=0;
	arg_t minus=0;
	if(lexcmp(text + *textptr, "$+") == SAME || lexcmp(text + *textptr, "$-") == SAME)
	{
		++*textptr;
		while(text[*textptr] == '+' || text[*textptr] == '-')
		{
			if(text[*textptr] == '+')
				plus++;
			else if(text[*textptr] == '-')
				minus++;
			(*textptr)++;
		}

		code[*pc].op = OP_ALUS;
		code[*pc].arg = plus - minus;
		++*pc;
		return ADV;
	}
	else
		return LEXERR;
}

EXEC(set)
{
	cpu->mem[cpu->ptr] = cpu->code[cpu->pc].arg;
	cpu->pc++;
	return CONT;
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

EXEC(pop)
{
	cpu->mem[cpu->ptr] = pop(cpu);
	cpu->pc++;
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
	cpu->pc++;
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
	cpu->pc++;
	return CONT;
}

SCAN(pshi)
{
	return LEXERR;
}

EXEC(mov)
{
	if(cpu->ptr + cpu->code[cpu->pc].arg < 0)
		cpu->ptr += cpu->code[cpu->pc].arg + cpu->size.mem;
	else if(cpu->ptr + cpu->code[cpu->pc].arg >= (arg_t)cpu->size.mem)
		cpu->ptr += cpu->code[cpu->pc].arg - cpu->size.mem;
	else
		cpu->ptr += cpu->code[cpu->pc].arg;
	cpu->pc++;
	return CONT;
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

EXEC(stp)
{
	cpu->ptr = cpu->code[cpu->pc].arg;
	cpu->pc++;
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

EXEC(jez)
{
	if(cpu->mem[cpu->ptr] == 0)
		cpu->pc = cpu->code[cpu->pc].arg;
	else
		cpu->pc++;
	return CONT;
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

EXEC(jnz)
{
	if(cpu->mem[cpu->ptr] != 0)
		cpu->pc = cpu->code[cpu->pc].arg;
	else
		cpu->pc++;
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

EXEC(jsez)
{
	if(pop(cpu) == 0)
		cpu->pc = cpu->code[cpu->pc].arg;
	else
		cpu->pc++;
	return CONT;
}

SCAN(jsez)
{
	if(lexcmp(text + *textptr, "$[") == SAME)
	{
		code[*pc].op = OP_JSEZ;
		pcstack_push(pcstack, *pc);
		(*textptr) += 2;	/* "$[" is 2 chars */
		++*pc;
		return ADV;
	}
	else
		return LEXERR;
}

EXEC(jsnz)
{
	if(pop(cpu) != 0)
		cpu->pc = cpu->code[cpu->pc].arg;
	else
		cpu->pc++;
	return CONT;
}

SCAN(jsnz)
{
	arg_t temp_pc=0;

	if(lexcmp(text + *textptr, "$]") == SAME)
	{
		temp_pc = pcstack_pop(pcstack);
		code[*pc].op = OP_JSNZ;
		code[*pc].arg = temp_pc;
		code[temp_pc].arg = *pc;
		(*textptr) += 2;	/* "$]" is 2 chars */
		++*pc;
		return ADV;
	}
	else
		return LEXERR;
}

EXEC(io)
{
	switch(cpu->code[cpu->pc].arg)
	{
		case IO_IN:
			cpu->mem[cpu->ptr] = io_in(cpu->debug);
			break;
		case IO_OUT:
			io_out(cpu->mem[cpu->ptr], cpu->debug);
			break;
		case IO_INS:
			push(cpu, io_in(cpu->debug));
			break;
		case IO_OUTS:
			io_out(pop(cpu), cpu->debug);
			break;
	}
	cpu->pc++;
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
	pid_t p = fork();
	if((p & 0xFF) == 0)
		cpu->mem[cpu->ptr] = (data_t)p + 1;
	else
		cpu->mem[cpu->ptr] = (data_t)p;
	cpu->pc++;
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

EXEC(inv)
{
	panic("?INV");
	return HALT;
}

SCAN(inv)
{
	return LEXERR;
}

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

brainfunk_t brainfunk_init(size_t codesize, size_t memsize, size_t stacksize, int debug)
{
	/* Allocate itself */
	brainfunk_t brainfunk = calloc(1, sizeof(struct _bf));

	brainfunk->code = calloc(codesize, sizeof(code_t));
	brainfunk->size.code = codesize;

	brainfunk->mem = calloc(memsize, sizeof(data_t));
	brainfunk->size.mem = memsize;

	brainfunk->stack = calloc(stacksize, sizeof(data_t));
	brainfunk->size.stack = stacksize;

	brainfunk->debug = debug;

	/* Insert a Invaild Instruction */
	brainfunk->code[codesize - 1].op = OP_INV;

	return brainfunk;
}

void brainfunk_destroy(brainfunk_t *brainfunk)
{
	free((*brainfunk)->code);
	free((*brainfunk)->mem);
	free((*brainfunk)->stack);
	free(*brainfunk);
	*brainfunk = NULL;
}

static inline int brainfunk_step(brainfunk_t cpu)
{
	return handler[cpu->code[cpu->pc].op].exec(cpu);
}

void brainfunk_execute(brainfunk_t cpu)
{
	if(cpu->debug)
	{
		while(brainfunk_step(cpu) != HALT)
		{
			fprintf(stderr, "%lld:\t%s\t%lld\n", cpu->pc, opname[cpu->code[cpu->pc].op], cpu->code[cpu->pc].arg);
			fprintf(stderr, "\tMEM[%lld] = %#hhx\n", cpu->ptr, cpu->mem[cpu->ptr]);
		}
	}
	else
	{
		while(brainfunk_step(cpu) != HALT);
	}
	return;
}

void bitcode_read(brainfunk_t cpu, FILE *fp)
{
	char op[OPLEN];
	int32_t arg=0;
	long long int addr=0;

	while(fscanf(fp, "%lld:\t%s\t%d\n", &addr, op, &arg) > 0)
	{
		cpu->code[addr].op = opcode(op);
		cpu->code[addr].arg = arg;
		cpu->codelen++;
	}

	cpu->codelen++;
}

void bitcode_dump(brainfunk_t cpu, int format, FILE *fp)
{
	long long int pc = 0;
	char *fmt;

	if(format == FORMAT_C)
	{
		fmt = "\tL%lld:\t\t\t%s(%lld);\n";

		fputs(	"#include <libstdbfc.h>\n\n"
			"int main(void)\n"
			"{\n"
			"\tinit();\n",
			fp);
	}
	else if(format == FORMAT_PLAIN)
	{
		fmt = "%lld:\t\t%s\t%lld\n";
	}
	else
		panic("?INVALID_DUMP_FORMAT");

	while(pc <= cpu->codelen)
	{
		fprintf(fp, fmt, pc, opname[cpu->code[pc].op], cpu->code[pc].arg);
		pc++;
	}

	if(format == FORMAT_C)
	{
		fputs("\treturn 0;\n"
			"}\n",
			fp);
	}
}

static inline int iscode(int c, int compat)
{
	switch(c)
	{
		case '+':
		case '-':
		case '>':
		case '<':
		case '[':
		case ']':
		case '.':
		case ',':
			return TRUE;
		case '$':
		case '\\':
		case '/':
		case '~':
		case '_':
		case '(':
		case ')':
			if(compat)
				return FALSE;
			else
				return TRUE;
		default:
			return FALSE; 
	}
}

char *brainfunk_readtext(FILE *fp, int compat, size_t size)
{
	char *code = malloc(size);
	int c=0;
	size_t i=0;

	while((c = getc(fp)) != EOF)
	{
		if(!iscode(c, compat))
			continue;
		code[i++] = (char)c;

		if((i+1) >= size)
			panic("?CODESIZE");
	}
	code[i] = '\0';
	return code;
}

void brainfunk_dumptext(char *code, FILE *fp)
{
	int counter=0;
	arg_t ptr=0;

	while(code[ptr] != '\0')
	{
		putc(code[ptr++], fp);
		counter++;
		if(counter >= (1<<6))
		{
			putc('\n', fp);
			counter=0;
		}
	}
	putc('\n', fp);
}

void bitcode_convert(brainfunk_t cpu, char *text)
{
	arg_t textptr=0;
	arg_t pc=0;
	arg_t ret=0;
	int try=0;

	pcstack_t pcstack = pcstack_create(PCSTACK_SIZE);

	while(text[textptr] != '\0')
	{
		try=0;
		while(try < OP_INSTS)
		{
			ret = handler[try++].scan(cpu->code, &pc, pcstack, &textptr, text);
			if(ret != LEXERR)
				break;
		}
	}

	cpu->codelen = pc;
	pcstack_destroy(&pcstack);
}

void quit(int32_t arg)
{
	exit(arg);
}
