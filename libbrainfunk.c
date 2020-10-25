#include <libbrainfunk.h>

#define EXEC(name)	static int exec_ ## name(brainfunk_t cpu)
#define EXEC_HANDLER_DEF(name)	exec_ ## name

#define SCAN(name) \
	static int scan_ ## name(char *text, size_t len, brainfunk_t cpu, pcstack_t pcstack)

#define SCAN_HANDLER_DEF(name, text) \
	{						\
		.regexp = text,			\
		.scan = scan_ ## name			\
	}

char opname[_OP_INSTS][_OPLEN] =
{
	"x",
	"a",
	"c",
	"s",
	"m",
	"je",
	"jn",
	"io",
	"f",
	"d",
	"h"
};

/* operand type of the opcode,
 *
 * N => None
 * O => Offset
 * A => Address
 * I => Intermediate
 */

char opcode_type[_OP_INSTS] =
{
	'N',	/* X */
	'O',	/* A */
	'O',	/* C */
	'I',	/* S */
	'O',	/* M */
	'A',	/* JE */
	'A',	/* JN */
	'I',	/* IO */
	'N',	/* F */
	'N',	/* D */
	'N'	/* H */
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
	assert(stack->ptr > 0);
	return stack->stack[--(stack->ptr)];
}

void pcstack_push(pcstack_t stack, addr_t data)
{
	assert(stack->ptr <= stack->size);
	stack->stack[stack->ptr++] = data;
	return;
}

void pcstack_destroy(pcstack_t *stack)
{
	free((*stack)->stack);
	free(*stack);
	*stack = NULL;
}

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

static inline void _operand_to_str(op_t op, arg_t arg, char *buf, size_t len)
{
	switch(opcode_type[op])
	{
		case 'N':
			snprintf(buf, len, "NONE");
			break;
		case 'O':
			snprintf(buf, len, "%zd", arg.offset);
			break;
		case 'I':
			snprintf(buf, len, "%hhx", arg.im);
			break;
		case 'A':
			snprintf(buf, len, "%zu", arg.addr);
			break;
		default:
			assert(0 != 0);
	}
}

static inline void _debug_print(brainfunk_t cpu)
{
	char buf[_MAXLEN];
	_operand_to_str(cpu->code[cpu->pc].op, cpu->code[cpu->pc].arg, buf, _MAXLEN);
	fprintf(stderr, ">> %ld:\t%s\t%s\n", cpu->pc, opname[cpu->code[cpu->pc].op], buf);
	fprintf(stderr, ">> \tMEM[%lu] = %#hhx\n", cpu->ptr, cpu->mem[cpu->ptr]);
	return;
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
	/* *(Current + offset) += arg */
	cpu->mem[cpu->ptr + cpu->code[cpu->pc].arg.offset] += cpu->mem[cpu->ptr];
	cpu->pc++;
	return _CONT;
}

EXEC(s)
{
	/* Current Cell = arg */
	cpu->mem[cpu->ptr] = cpu->code[cpu->pc].arg.im;
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
	switch(cpu->code[cpu->pc].arg.im)
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
	_debug_print(cpu);
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
	EXEC_HANDLER_DEF(s),
	EXEC_HANDLER_DEF(m),
	EXEC_HANDLER_DEF(je),
	EXEC_HANDLER_DEF(jn),
	EXEC_HANDLER_DEF(io),
	EXEC_HANDLER_DEF(f),
	EXEC_HANDLER_DEF(d),
	EXEC_HANDLER_DEF(h)
};

static inline int brainfunk_step(brainfunk_t cpu)
{
	return exec_handler[cpu->code[cpu->pc].op](cpu);
}

void brainfunk_execute(brainfunk_t cpu)
{
	assert(cpu->pc < cpu->codelen);
	if(cpu->debug)
	{
		while(brainfunk_step(cpu) != _HALT)
		{
			_debug_print(cpu);
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
		cpu->codelen++;
	}
}

void bitcode_dump(brainfunk_t cpu, int format, FILE *fp)
{
	addr_t pc = 0;
	char *fmt;
	char operand[_MAXLEN];

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
		_operand_to_str(cpu->code[pc].op, cpu->code[pc].arg, operand, _MAXLEN);
		fprintf(fp, fmt, pc, opname[cpu->code[pc].op], operand);
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

size_t _count_continus(char *text, size_t len, char *symbolset)
{
	size_t i=0;
	size_t ctr=0;
	while(i < len)
	{
		if(text[i] == symbolset[0])
			ctr++;
		else if(text[i] == symbolset[1])
			ctr--;
		i++;
	}
	return ctr;
}

SCAN(smul)
{
	ssize_t ctr=0;

	/* First we need to validate if this is a true "multiply" operation */
	ctr = _count_continus(text, len, "><");
	if(ctr != 0)
		return FALSE;


	if(text[1] == '-')
	{
	}
	else
	{
	}

	return FALSE;
}

SCAN(sc)
{
	return FALSE;
}

SCAN(s0)
{
	cpu->code[cpu->pc].op = _OP_S;
	cpu->code[cpu->pc].arg.im = 0;

	cpu->pc++;
	return TRUE;
}

SCAN(a)
{
	offset_t offset=0;

	offset = _count_continus(text, len, "+-");

	cpu->code[cpu->pc].op = _OP_A;
	cpu->code[cpu->pc].arg.offset = offset;

	cpu->pc++;
	return TRUE;
}

SCAN(m)
{
	offset_t offset=0;

	offset = _count_continus(text, len, "><");

	cpu->code[cpu->pc].op = _OP_M;
	cpu->code[cpu->pc].arg.offset = offset;

	cpu->pc++;
	return TRUE;
}

SCAN(je)
{
	cpu->code[cpu->pc].op = _OP_JE;
	pcstack_push(pcstack, cpu->pc);

	cpu->pc++;
	return TRUE;
}

SCAN(jn)
{
	addr_t last_pc = pcstack_pop(pcstack);
	cpu->code[cpu->pc].op = _OP_JN;

	/* Skip the je & jn instructions themselves, this improves speed a little */
	cpu->code[cpu->pc].arg.addr = last_pc + 1;
	cpu->code[last_pc].arg.addr = cpu->pc + 1;

	cpu->pc++;
	return TRUE;
}

SCAN(io)
{
	if(*text == ',')
		cpu->code[cpu->pc].arg.im = _IO_IN;
	else if(*text == '.')
		cpu->code[cpu->pc].arg.im = _IO_OUT;

	cpu->code[cpu->pc++].op = _OP_IO;
	return TRUE;
}

SCAN(f)
{
	cpu->code[cpu->pc++].op = _OP_F;
	return TRUE;
}

SCAN(d)
{
	cpu->code[cpu->pc++].op = _OP_D;
	return TRUE;
}

SCAN(h)
{
	cpu->code[cpu->pc++].op = _OP_H;
	return TRUE;
}

scan_handler_t scan_handler[] =
{
		SCAN_HANDLER_DEF(smul, "^\\[-(>+[+-]+)+<+\\]"),	/* S 0 & MUL (C+) */
		SCAN_HANDLER_DEF(smul, "^\\[(>+[+-]+)+<+-\\]"),	/* S 0 & MUL (C+) */
		SCAN_HANDLER_DEF(sc, "^\\[-(>+[+-])+<+\\]"),	/* S 0 & C */
		SCAN_HANDLER_DEF(sc, "^\\[(>+[+-])+<+-\\]"),	/* S 0 & C */
		SCAN_HANDLER_DEF(s0, "^\\[\\-\\]"),	/* S 0 */
		SCAN_HANDLER_DEF(a, "^[+-]+"),	/* A */
		SCAN_HANDLER_DEF(m, "^[<>]+"),	/* M */
		SCAN_HANDLER_DEF(je, "^\\["),	/* JE */
		SCAN_HANDLER_DEF(jn, "^\\]"),	/* JN */
		SCAN_HANDLER_DEF(io, "^\\."),	/* IO OUT */
		SCAN_HANDLER_DEF(io, "^\\,"),	/* IO IN */
		SCAN_HANDLER_DEF(f, "^\\~"),	/* F */
		SCAN_HANDLER_DEF(h, "^%"),		/* H */
		SCAN_HANDLER_DEF(d, "^\\#")	/* D */
};

#define _SCAN_HANDLERS	(sizeof(scan_handler)/sizeof(scan_handler_t))

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
		strncpy(buf, text, match.rm_eo);
		buf[match.rm_eo] = 0;
	}
	(*len) = match.rm_eo;
	assert(match.rm_so == 0);
	regfree(&preg);
	return TRUE;
}

/*
 * Convert plain text Brainfuck code into bitcode,
 * scan_handler uses cpu->pc to remember their current position
 */

void bitcode_convert(brainfunk_t cpu, char *text)
{
	unsigned int try = 0;
	size_t len = 0;
	size_t pos = 0;

	pcstack_t pcstack = pcstack_create(_PCSTACK_SIZE);

	while(text[pos] != '\0')
	{
		try = 0;
		while(try < _SCAN_HANDLERS)
		{
			if(regex_cmp(text + pos, scan_handler[try].regexp, &len) == TRUE)
				if(scan_handler[try].scan(text + pos, len, cpu, pcstack) == TRUE)
					break;
			try++;
		}
		/* assert(try > _SCAN_HANDLERS); */
		pos += len;
	}

	cpu->codelen = cpu->pc;
	cpu->code[cpu->pc].op = _OP_H;
	cpu->pc = 0;
	pcstack_destroy(&pcstack);
}

void quit(int32_t arg)
{
	exit(arg);
}
