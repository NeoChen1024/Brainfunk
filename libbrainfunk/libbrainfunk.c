#include <libbrainfunk.h>

char opname[OP_INSTS][16] =
{
	"HLT",
	"ALU",
	"ALUS",
	"SET",
	"POP",
	"PUSH",
	"PSHI",
	"MOV",
	"STP",
	"JMP",
	"JEZ",
	"JNZ",
	"JSEZ",
	"JSNZ",
	"IO",
	"FRK",
	"INV"
};

void panic(char *msg)
{
	fprintf(stderr, "%s\n", msg);
	quit(8);
}

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

void brainfunk_execute(brainfunk_t cpu)
{
	int status = CONT;
	if(cpu->debug)
	{
		while(status != HALT)
		{
			fprintf(stderr, "%lld:\t%s\t%lld\n", cpu->pc, opname[cpu->code[cpu->pc].op], cpu->code[cpu->pc].arg);
			fprintf(stderr, "\tPC = %lld\n", cpu->pc);
			cpu->pc += status = handler[cpu->code[cpu->pc].op].exec(cpu);
			fprintf(stderr, "\tMEM[%lld] = %hhx\n", cpu->ptr, cpu->mem[cpu->ptr]);
		}
	}
	else
	{
		while(status != HALT)
		{
			cpu->pc += status = handler[cpu->code[cpu->pc].op].exec(cpu);
		}
	}
	return;
}

void bitcode_read(brainfunk_t cpu, FILE *fp)
{
	uint8_t op=0;
	int32_t arg=0;
	long long int pc=0;
	long long int codelength=0;
	while(fscanf(fp, "%lld:\t%hhu\t%d\n", &pc, &op, &arg) > 0)
	{
		cpu->code[pc].op = op;
		cpu->code[pc].arg = arg;

		if(pc > codelength)
			codelength = pc + 1;
	}
	cpu->codelen = codelength;
}

void bitcode_dump(brainfunk_t cpu, FILE *fp)
{
	long long int pc = 0;
	while(pc <= cpu->codelen)
	{
		fprintf(fp, "%lld:\t%s\t%lld\n", pc, opname[cpu->code[pc].op], cpu->code[pc].arg);
		pc++;
	}
}

int codefilter(FILE *fp)
{
	int code=0;
	while((code = getc(fp)) != EOF)
	{
		switch(code)
		{
			case '+':
			case '-':
			case '>':
			case '<':
			case '[':
			case ']':
			case '.':
			case ',':
				return code;
				break;
		}
	}
	return code;	/* EOF */
}

char *brainfunk_readtext(FILE *fp, size_t size)
{
	char *code = malloc(size);
	int c=0;
	size_t ptr=0;

	while((c = codefilter(fp)) != EOF)
	{
		code[ptr++] = (char)c;

		if((ptr + 1) >= size) /* 1 extra space for NUL */
			code = realloc(code, size += 4096);
	}
	code[ptr] = '\0';
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
			{
				break;
			}
		}
	}

	cpu->codelen = pc;
	pcstack_destroy(&pcstack);
}

void quit(int32_t arg)
{
	exit(arg);
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

data_t pop(brainfunk_t cpu)
{
	if(cpu->sp == 0)
		return 0;
	return cpu->stack[--(cpu->sp)];
}
