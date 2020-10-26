#include <libbrainfunk.h>

#define EXEC(name)	static int exec_ ## name(brainfunk_t cpu)
#define EXEC_HANDLER_DEF(name)	exec_ ## name

#define SCAN(name) \
	static int scan_ ## name(char *text, size_t len, brainfunk_t cpu, pcstack_t pcstack)

#define SCAN_HANDLER_DEF(name, text) \
	{						\
		.regexp = text,				\
		.scan = scan_ ## name			\
	}

static char opname[_OP_INSTS][_OPLEN] =
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

static char opcode_type[_OP_INSTS] =
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

void panic(char *msg)
{
	fprintf(stderr, "%s\n", msg);
	quit(8);
}

static pcstack_t pcstack_create(size_t size)
{
	pcstack_t stack = calloc(1, sizeof(struct _pcstack));
	stack->stack = calloc(size, sizeof(size_t));
	stack->ptr = 0;
	stack->size = size;

	return stack;
}

INLINE addr_t pcstack_pop(pcstack_t stack)
{
	assert(stack->ptr > 0);
	return stack->stack[--(stack->ptr)];
}

INLINE void pcstack_push(pcstack_t stack, addr_t data)
{
	assert(stack->ptr <= stack->size);
	stack->stack[stack->ptr++] = data;
	return;
}

static void pcstack_destroy(pcstack_t *stack)
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

static inline void _operand_to_str(op_t op, arg_t *arg, char *buf, size_t len)
{
	switch(opcode_type[op])
	{
		case 'N':
			buf[0] = '\0';
			break;
		case 'O':
			snprintf(buf, len, "%zd", arg->offset);
			break;
		case 'M':
			snprintf(buf, len, "%d, %d", arg->dual.mul, arg->dual.offset);
			break;
		case 'I':
			snprintf(buf, len, "%hhu", arg->im);
			break;
		case 'A':
			snprintf(buf, len, "%zu", arg->addr);
			break;
		default:
			assert(0 != 0);
	}
}

INLINE void debug_print(brainfunk_t cpu)
{
	char buf[_MAXLEN];
	_operand_to_str(cpu->code[cpu->pc].op, &cpu->code[cpu->pc].arg, buf, _MAXLEN);
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

EXEC(mul)
{
	/* *(Current + offset) += Current * mul */
	if(cpu->mem[cpu->ptr] != 0)
		cpu->mem[cpu->ptr + cpu->code[cpu->pc].arg.dual.offset] += cpu->mem[cpu->ptr] * cpu->code[cpu->pc].arg.dual.mul;
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

EXEC(f)
{
	/* Move until find 0 */
	while(cpu->mem[cpu->ptr] != 0)
	{
		if((offset_t)cpu->ptr + cpu->code[cpu->pc].arg.offset < 0)
			cpu->ptr += cpu->code[cpu->pc].arg.offset + cpu->size.mem;
		else if(cpu->ptr + cpu->code[cpu->pc].arg.offset >= cpu->size.mem)
			cpu->ptr += cpu->code[cpu->pc].arg.offset - cpu->size.mem;
		else
			cpu->ptr += cpu->code[cpu->pc].arg.offset;
	}

	cpu->pc++;
	return _CONT;
}

EXEC(m)
{
	/* Wrap-around, Pointer += arg */
	if(unlikely((offset_t)cpu->ptr + cpu->code[cpu->pc].arg.offset < 0))
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
	int c = 0;
	switch(cpu->code[cpu->pc].arg.im)
	{
		case _IO_IN:
			c = io_in(cpu->debug);
			cpu->mem[cpu->ptr] = c == EOF ? 0 : c;
			break;
		case _IO_OUT:
			io_out(cpu->mem[cpu->ptr], cpu->debug);
			break;
	}
	cpu->pc++;
	return _CONT;
}

EXEC(y)
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
	debug_print(cpu);
	cpu->pc++;
	return _CONT;
}

EXEC(x)
{
	panic("?INV");
	return _HALT;
}

static exec_handler_t exec_handler[_OP_INSTS] =
{
	EXEC_HANDLER_DEF(x),
	EXEC_HANDLER_DEF(a),
	EXEC_HANDLER_DEF(mul),
	EXEC_HANDLER_DEF(s),
	EXEC_HANDLER_DEF(f),
	EXEC_HANDLER_DEF(m),
	EXEC_HANDLER_DEF(je),
	EXEC_HANDLER_DEF(jn),
	EXEC_HANDLER_DEF(io),
	EXEC_HANDLER_DEF(y),
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
		while(likely(brainfunk_step(cpu) != _HALT))
			debug_print(cpu);
	}
	else
	{
		while(likely(brainfunk_step(cpu) != _HALT));
	}
	return;
}

void bitcode_dump(brainfunk_t cpu, int format, FILE *fp)
{
	addr_t pc = 0;
	char *fmt;
	char operand[_MAXLEN];

	if(format == BITCODE_FORMAT_C)
	{
		fmt = "\tL%lld:\t\t\t%s(%s);\n";

		fputs(	"#include <libstdbfc.h>\n\n"
			"int main(void)\n"
			"{\n"
			"\tinit();\n",
			fp);
	}
	else if(format == BITCODE_FORMAT_PLAIN)
	{
		fmt = "%lld:\t%s\t%s\n";
	}
	else
		panic("?INVALID_DUMP_FORMAT");

	while(pc < cpu->codelen)
	{
		_operand_to_str(cpu->code[pc].op, &cpu->code[pc].arg, operand, _MAXLEN);
		fprintf(fp, fmt, pc, opname[cpu->code[pc].op], operand);
		pc++;
	}

	if(format == BITCODE_FORMAT_C)
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
		case '!':
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

/* The "Compiler" part */

INLINE int regex_cmp(char *text, regex_t *preg, size_t *len)
{
	int ret = 0;
	regmatch_t match;

	ret = regexec(preg, text, 1, &match, 0);
	if(ret == REG_NOMATCH)
		return FALSE;
	(*len) = match.rm_eo;

	assert(likely(match.rm_so == 0));
	return TRUE;
}

INLINE size_t count_continus(char *text, size_t len, char *symbolset)
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

INLINE int contain(char *symbolset, char c)
{
	size_t i=0;
	while(symbolset[i++] != '\0')
		if(symbolset[i] == c)
			return TRUE;
	return FALSE;
}

INLINE void count_mul_offset(char *text, size_t len, int32_t *mul, int32_t *offset, int32_t lastoffset)
{
	*mul = count_continus(text, len, "+-");
	*offset = count_continus(text, len, "><") + lastoffset;
	return;
}

SCAN(smul)
{
	ssize_t i=0;
	int pairs=0;
	int mode=0;
	int32_t mul[_MAXLEN];
	int32_t offset[_MAXLEN];
	size_t match_len=0;

	regex_t preg;
	int ret=0;

	/* First we need to validate if it goes back to where it was */
	i = count_continus(text, len, "><");
	if(i != 0)
		return FALSE;

	i = 0; /* Reuse it as index */

	/* Basically, the text will look either like:
	 *
	 *	1. [->>++++>>>>++++++++<<--<<<<]
	 *
	 *	or
	 *
	 *	2. [>>+++++>>>>+++<<---<<<<-]
	 */

	if(text[1] == '-')
	{
		text += 2;
		mode = 1;
	}
	else
	{
		text += 1;
		mode = 2;
	}

	ret = regcomp(&preg, "^[><]+[+-]+", REG_EXTENDED);
	assert(ret == 0);
	while(regex_cmp(text + i, &preg, &match_len) == TRUE)
	{
		count_mul_offset(text + i, match_len, &mul[pairs], &offset[pairs], pairs == 0 ? 0 : offset[pairs - 1]);
		pairs++;
		i += match_len;
	}

	assert(pairs > 0);

	/* Omit the last false pair in mode 2 */
	if(mode == 2)
		pairs--;

	for(i = 0; i < pairs; i++) /* Reuse i again, this time as another index */
	{
		cpu->code[cpu->pc + i].op = _OP_MUL;
		cpu->code[cpu->pc + i].arg.dual.mul = mul[i];
		cpu->code[cpu->pc + i].arg.dual.offset = offset[i];
	}

	/* Insert a "S 0" to get correct behavior */
	cpu->code[cpu->pc + pairs].op = _OP_S;
	cpu->code[cpu->pc + pairs].arg.im = 0;

	cpu->pc += pairs + 1;

	regfree(&preg);
	return TRUE;
}

SCAN(s0)
{
	cpu->code[cpu->pc].op = _OP_S;
	cpu->code[cpu->pc].arg.im = 0;

	cpu->pc++;
	return TRUE;
}

SCAN(f)
{
	offset_t offset = count_continus(text, len, "><");

	cpu->code[cpu->pc].op = _OP_F;
	cpu->code[cpu->pc].arg.offset = offset;
	cpu->pc++;
	return TRUE;
}

SCAN(a)
{
	offset_t offset=0;

	offset = count_continus(text, len, "+-");

	cpu->code[cpu->pc].op = _OP_A;
	cpu->code[cpu->pc].arg.offset = offset;

	cpu->pc++;
	return TRUE;
}

SCAN(m)
{
	offset_t offset=0;
	offset = count_continus(text, len, "><");

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

SCAN(y)
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

SCAN(split)
{
	return TRUE;
}

static scan_handler_t scan_handler[] =
{
		SCAN_HANDLER_DEF(split,	"^!"),	/* A */
		SCAN_HANDLER_DEF(smul,	"^\\[-([<>]+[+-]+)+[<>]+\\]"),	/* S 0 & MUL */
		SCAN_HANDLER_DEF(smul,	"^\\[([<>]+[+-]+)+[<>]+-\\]"),	/* S 0 & MUL */
		SCAN_HANDLER_DEF(f,	"^\\[[><]+\\]"),	/* F + / - */
		SCAN_HANDLER_DEF(s0,	"^\\[\\-\\]"),	/* S 0 */
		SCAN_HANDLER_DEF(a,	"^[+-]+"),	/* A */
		SCAN_HANDLER_DEF(m,	"^[<>]+"),	/* M */
		SCAN_HANDLER_DEF(je,	"^\\["),	/* JE */
		SCAN_HANDLER_DEF(jn,	"^\\]"),	/* JN */
		SCAN_HANDLER_DEF(io,	"^\\."),	/* IO OUT */
		SCAN_HANDLER_DEF(io,	"^\\,"),	/* IO IN */
		SCAN_HANDLER_DEF(y,	"^\\~"),	/* Y */
		SCAN_HANDLER_DEF(h,	"^%"),	/* H */
		SCAN_HANDLER_DEF(d,	"^\\#")	/* D */
};

#define _SCAN_HANDLERS	(sizeof(scan_handler)/sizeof(scan_handler_t))

static regex_t _preg[_SCAN_HANDLERS];

/*
 * Convert plain text Brainfuck code into bitcode,
 * scan_handler uses cpu->pc to remember the current position
 */

void bitcode_convert(brainfunk_t cpu, char *text)
{
	unsigned int i = 0;
	int ret = 0;
	unsigned int try = 0;
	size_t len = 0;
	size_t pos = 0;

	pcstack_t pcstack = pcstack_create(_PCSTACK_SIZE);

	for(i = 0; i < _SCAN_HANDLERS; i++)
	{
		ret = regcomp(&_preg[i], scan_handler[i].regexp, REG_EXTENDED);
		assert(ret == 0);
	}

	while(unlikely(text[pos] != '\0'))
	{
		try = 0;
		while(try < _SCAN_HANDLERS)
		{
			if(regex_cmp(text + pos, &_preg[try], &len) == TRUE)
				if(scan_handler[try].scan(text + pos, len, cpu, pcstack) == TRUE)
					break;
			try++;
		}
		pos += len;
	}

	cpu->code[cpu->pc].op = _OP_H;
	cpu->codelen = cpu->pc + 1; /* Add 1 because we inserted that halt instruction */
	cpu->pc = 0;
	pcstack_destroy(&pcstack);

	/* Free all compiled RegEx */
	for(i = 0; i < _SCAN_HANDLERS; i++)
		regfree(&_preg[i]);
}

void quit(int32_t arg)
{
	exit(arg);
}
