#include <libbrainfunk.h>

void panic(char *msg)
{
	fprintf(stderr, "%s\n", msg);
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
		fprintf(stderr, "%lld:\t%hhd\t%lld\n", cpu->pc, cpu->code[cpu->pc].op, cpu->code[cpu->pc].arg);
		while(status != HALT)
		{
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
		fprintf(fp, "%lld:\t%hhu\t%lld\n", pc, cpu->code[pc].op, cpu->code[pc].arg);
		pc++;
	}
}

void bitcode_readcode(brainfunk_t cpu, FILE *fp)
{
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
	return stack->stack[--(stack->ptr)];
}

void pcstack_push(pcstack_t stack, arg_t data)
{
	stack->stack[stack->ptr++] = data;
	return;
}

void pcstack_destroy(pcstack_t *stack)
{
	free((*stack)->stack);
	free(*stack);
	*stack = NULL;
}
