#include <libbrainfunk.h>

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
			status = handler[cpu->code[cpu->pc].op].exec(cpu);
		}
	}
	else
	{
		while(status != HALT)
		{
			status = handler[cpu->code[cpu->pc].op].exec(cpu);
		}
	}
	return;
}

void bitcode_read(brainfunk_t cpu, FILE *fp)
{
	int op=0;
	long long int arg=0;
	long long int pc;
	long long int codelength;
	while(fscanf(fp, "%lld:\t%hhd\t%lld\n", &pc, &op, &arg) > 0)
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
	char str[STRLENGTH];
	long long int pc = 0;
	while(pc <= cpu->codelen)
	{
		fprintf(fp, "%lld:\t%hd\t%lld\n", pc, cpu->code[pc].op, cpu->code[pc].arg);
		pc++;
	}
}

void quit(int32_t arg)
{
	exit(arg);
}
