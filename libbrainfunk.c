#include <libbrainfunk.h>
#define EXEC(name)	static int exec_ ## name(brainfunk_t cpu)
#define HANDLER_DEF(name)	exec_ ## name

char opname[_OP_INSTS][_OPLEN] =
{
	"H",
	"A",
	"S",
	"M",
	"P",
	"J",
	"JE",
	"JN",
	"IO",
	"F",
	"D",
	"I"
};

op_t opcode(char *name)
{
	int i=0;
	while(strcmp(name, opname[i]) != 0 || i < _OP_INSTS) ++i;
	return i;
}

void panic(char *msg)
{
	fprintf(stderr, "%s\n", msg);
	quit(8);
}

pcstack_t pcstack_create(size_t size)
{
	pcstack_t stack = calloc(1, sizeof(struct _pcstack));
	stack->stack = calloc(size, sizeof(size_t));
	stack->ptr = 0;
	stack->size = size;

	return stack;
}

ptr_t pcstack_pop(pcstack_t stack)
{
	if(stack->ptr == 0)
		panic("?PCSTACK_UNDERFLOW");
	return stack->stack[--(stack->ptr)];
}

void pcstack_push(pcstack_t stack, ptr_t data)
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

EXEC(h)
{
	/* Mute the warning */
	size_t a=0;
	a=cpu->pc;

	return _HALT;
}

EXEC(a)
{
	/* Current Cell += arg */
	cpu->mem[cpu->ptr] += cpu->code[cpu->pc].arg.data;
	cpu->pc++;
	return _CONT;
}

EXEC(s)
{
	/* Current Cell = arg */
	cpu->mem[cpu->ptr] = cpu->code[cpu->pc].arg.data;
	cpu->pc++;
	return _CONT;
}

EXEC(m)
{
	/* Wrap-around, Pointer += arg */
	if(cpu->ptr + cpu->code[cpu->pc].arg.offset < 0)
		cpu->ptr += cpu->code[cpu->pc].arg.offset + cpu->size.mem;
	else if(cpu->ptr + cpu->code[cpu->pc].arg.offset >= cpu->size.mem)
		cpu->ptr += cpu->code[cpu->pc].arg.offset - cpu->size.mem;
	else
		cpu->ptr += cpu->code[cpu->pc].arg.offset;
	cpu->pc++;
	return _CONT;
}

EXEC(p)
{
	/* Pointer = arg */
	cpu->ptr = cpu->code[cpu->pc].arg.ptr;
	cpu->pc++;
	return _CONT;
}

EXEC(j)
{
	cpu->pc = cpu->code[cpu->pc].arg.ptr;
	return _CONT;
}

EXEC(je)
{
	if(cpu->mem[cpu->ptr] == 0)
		cpu->pc = cpu->code[cpu->pc].arg.ptr;
	else
		cpu->pc++;
	return _CONT;
}

EXEC(jn)
{
	if(cpu->mem[cpu->ptr] != 0)
		cpu->pc = cpu->code[cpu->pc].arg.ptr;
	else
		cpu->pc++;
	return _CONT;
}

EXEC(io)
{
	switch(cpu->code[cpu->pc].arg.data)
	{
		case _IO_IN:
			cpu->mem[cpu->ptr] = io_in(cpu->debug);
			break;
		case _IO_OUT:
			io_out(cpu->mem[cpu->ptr], cpu->debug);
			break;
	}
	cpu->pc++;
	return _CONT;
}

EXEC(f)
{
	pid_t p = fork();
	if(p != 0 && (p & 0xFF) == 0)
		cpu->mem[cpu->ptr] = 0xFF;
	else
		cpu->mem[cpu->ptr] = (data_t)p;
	cpu->pc++;
	return _CONT;
}

EXEC(d)
{
	/* Mute the warning */
	size_t a=0;
	a=cpu->pc;

	return _CONT;
}

EXEC(i)
{
	/* Mute the warning */
	size_t a=0;
	a=cpu->pc;

	panic("?INV");
	return _HALT;
}

handler_t exec_handler[_OP_INSTS] =
{
	HANDLER_DEF(h),
	HANDLER_DEF(a),
	HANDLER_DEF(s),
	HANDLER_DEF(m),
	HANDLER_DEF(p),
	HANDLER_DEF(j),
	HANDLER_DEF(je),
	HANDLER_DEF(jn),
	HANDLER_DEF(io),
	HANDLER_DEF(f),
	HANDLER_DEF(d),
	HANDLER_DEF(i)
};

brainfunk_t brainfunk_init(size_t codesize, size_t memsize, int debug)
{
	/* Allocate itself */
	brainfunk_t brainfunk = calloc(1, sizeof(struct _bf));

	brainfunk->code = calloc(codesize, sizeof(code_t));
	brainfunk->size.code = codesize;

	brainfunk->mem = calloc(memsize, sizeof(data_t));
	brainfunk->size.mem = memsize;

	brainfunk->debug = debug;

	/* Insert a Invaild Instruction */
	brainfunk->code[codesize - 1].op = _OP_I;

	return brainfunk;
}

void brainfunk_destroy(brainfunk_t *brainfunk)
{
	free((*brainfunk)->code);
	free((*brainfunk)->mem);
	free(*brainfunk);
	*brainfunk = NULL;
}

static inline int brainfunk_step(brainfunk_t cpu)
{
	return exec_handler[cpu->code[cpu->pc].op](cpu);
}

void brainfunk_execute(brainfunk_t cpu)
{
	if(cpu->debug)
	{
		while(brainfunk_step(cpu) != _HALT)
		{
			/* TODO: different data type printout
			fprintf(stderr, "%lld:\t%s\t%lld\n", cpu->pc, opname[cpu->code[cpu->pc].op], cpu->code[cpu->pc].arg);
			*/
			fprintf(stderr, "\tMEM[%zd] = %#hhx\n", cpu->ptr, cpu->mem[cpu->ptr]);
		}
	}
	else
	{
		while(brainfunk_step(cpu) != _HALT);
	}
	return;
}

void bitcode_read(brainfunk_t cpu, FILE *fp)
{
	char op[_OPLEN];
	long long int addr=0;
	char arg[_MAXLEN];

	while(fscanf(fp, "%lld:\t%s\t%s\n", &addr, op, arg) > 0)
	{
		cpu->code[addr].op = opcode(op);
		/* TODO: different data type */
		cpu->codelen++;
	}
}

void bitcode_dump(brainfunk_t cpu, int format, FILE *fp)
{
	ptr_t pc = 0;
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

	while(pc < cpu->codelen)
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
		case '~':
		case '%':
		case '#':
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
	ptr_t ptr=0;

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
	ptr_t textptr=0;
	ptr_t pc = 0;

	pcstack_t pcstack = pcstack_create(_PCSTACK_SIZE);

	while(text[textptr] != '\0')
	{
	}

	cpu->codelen = pc;
	pcstack_destroy(&pcstack);
}

void quit(int32_t arg)
{
	exit(arg);
}
