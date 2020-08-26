#include <libbrainfunk.h>

#define EXEC(name)	static int exec_ ## name(brainfunk_t cpu)
#define EXEC_HANDLER_DEF(name)	exec_ ## name

char opname[_OP_INSTS][_OPLEN] =
{
	"X",
	"A",
	"C",
	"MUL",
	"S",
	"M",
	"P",
	"J",
	"JE",
	"JN",
	"IO",
	"F",
	"D",
	"H"
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

addr_t pcstack_pop(pcstack_t stack)
{
	assert(stack->ptr == 0);
	return stack->stack[--(stack->ptr)];
}

void pcstack_push(pcstack_t stack, addr_t data)
{
	assert(stack->ptr >= stack->size);
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
	return _HALT;
}

EXEC(a)
{
	/* Current Cell += arg */
	cpu->mem[cpu->ptr] += cpu->code[cpu->pc].arg.offset;
	cpu->pc++;
	return _CONT;
}

EXEC(c)
{
	cpu->mem[cpu->ptr + cpu->code[cpu->pc].arg.offset] = cpu->mem[cpu->ptr];
	cpu->pc++;
	return _CONT;
}

EXEC(mul)
{
	cpu->mem[cpu->ptr + cpu->code[cpu->pc].arg.dual.offset] += cpu->mem[cpu->ptr] * cpu->code[cpu->pc].arg.dual.m;
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
	if((offset_t)cpu->ptr + cpu->code[cpu->pc].arg.offset < 0)
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
	cpu->ptr = cpu->code[cpu->pc].arg.addr;
	assert(cpu->ptr < cpu->size.mem);
	cpu->pc++;
	return _CONT;
}

EXEC(j)
{
	cpu->pc = cpu->code[cpu->pc].arg.addr;
	assert(cpu->pc < cpu->size.code);
	return _CONT;
}

EXEC(je)
{
	if(cpu->mem[cpu->ptr] == 0)
		cpu->pc = cpu->code[cpu->pc].arg.addr;
	else
		cpu->pc++;
	assert(cpu->pc < cpu->size.code);
	return _CONT;
}

EXEC(jn)
{
	if(cpu->mem[cpu->ptr] != 0)
		cpu->pc = cpu->code[cpu->pc].arg.addr;
	else
		cpu->pc++;
	assert(cpu->pc < cpu->size.code);
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
	return _CONT;
}

EXEC(x)
{
	panic("?INV");
	return _HALT;
}

exec_handler_t exec_handler[_OP_INSTS] =
{
	EXEC_HANDLER_DEF(x),
	EXEC_HANDLER_DEF(a),
	EXEC_HANDLER_DEF(c),
	EXEC_HANDLER_DEF(mul),
	EXEC_HANDLER_DEF(s),
	EXEC_HANDLER_DEF(m),
	EXEC_HANDLER_DEF(p),
	EXEC_HANDLER_DEF(j),
	EXEC_HANDLER_DEF(je),
	EXEC_HANDLER_DEF(jn),
	EXEC_HANDLER_DEF(io),
	EXEC_HANDLER_DEF(f),
	EXEC_HANDLER_DEF(d),
	EXEC_HANDLER_DEF(h)
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
	addr_t pc = 0;
	char *fmt;

	if(format == FORMAT_C)
	{
		fmt = "\tL%lld:\t\t\t%s(%s);\n";

		fputs(	"#include <libstdbfc.h>\n\n"
			"int main(void)\n"
			"{\n"
			"\tinit();\n",
			fp);
	}
	else if(format == FORMAT_PLAIN)
	{
		fmt = "%lld:\t\t%s\t%s\n";
	}
	else
		panic("?INVALID_DUMP_FORMAT");

	while(pc < cpu->codelen)
	{
		/* TODO: different data type
		fprintf(fp, fmt, pc, opname[cpu->code[pc].op], cpu->code[pc].arg);
		*/
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
	addr_t ptr=0;

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

scan_handler_t scan_handler[_OP_INSTS] =
{
};

char regexps[][_MAXLEN] =
{
	"^\\[-(>+[+-]+)+<+\\]",	/* MUL */
	"^\\[(>+[+-]+)+<+-\\]",	/* MUL */
	"^\\[-(>+[+-])+<+\\]",	/* C */
	"^\\[(>+[+-])+<+-\\]",	/* C */
	"^\\[\\-\\]",		/* S 0 */
	"^[+-]+",		/* A */
	"^[<>]+",		/* M */
	"^\\[",			/* JE */
	"^\\]",			/* JN */
	"^\\.",			/* IO OUT */
	"^\\,",			/* IO IN */
	"^\\~",			/* F */
	"^%",			/* H */
	"^\\#"			/* D */
};

#define _SCAN_FORMS (sizeof(regexps)/sizeof(regexps[0]))

int regex_cmp(char *text, char *regex_str, size_t *len)
{
	int ret = 0;
	regex_t preg;
	regmatch_t match;

	char buf[_MAXLEN];

	ret = regcomp(&preg, regex_str, REG_EXTENDED);
	assert(ret == 0);

	ret = regexec(&preg, text, 1, &match, 0);
	if(ret == REG_NOMATCH)
		return FALSE;
	else if(ret == 0)
	{
		strncpy(buf, text, (size_t)match.rm_eo);
		buf[match.rm_eo] = 0;
		printf("MATCHED! (%d)\t-> %s\n", match.rm_eo, buf);
	}
	(*len) = match.rm_eo;
	assert(match.rm_so == 0);
	regfree(&preg);
	return TRUE;
}

void bitcode_convert(brainfunk_t cpu, char *text)
{
	unsigned int try = 0;
	size_t len = 0;
	size_t pos = 0;

	pcstack_t pcstack = pcstack_create(_PCSTACK_SIZE);

	while(text[pos] != '\0')
	{
		try = 0;
		while(regex_cmp(text + pos, regexps[try], &len) != TRUE && try <= _SCAN_FORMS)
			try++;
		/*scan_handler[try - 1](text, len, cpu, pcstack); */

		pos += len;
	}

	cpu->codelen = cpu->pc;
	cpu->pc = 0;
	pcstack_destroy(&pcstack);
}

void quit(int32_t arg)
{
	exit(arg);
}
