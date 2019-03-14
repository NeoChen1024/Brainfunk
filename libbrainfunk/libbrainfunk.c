#include <libbrainfunk.h>

brainfunk_t brainfunk_init(size_t codesize, size_t memsize, size_t stacksize)
{
	/* Allocate itself */
	brainfunk_t brainfunk = calloc(1, sizeof(struct _bf));

	brainfunk->code = calloc(codesize, sizeof(code_t));
	brainfunk->size.code = codesize;

	brainfunk->mem = calloc(memsize, sizeof(data_t));
	brainfunk->size.mem = memsize;

	brainfunk->stack = calloc(stacksize, sizeof(data_t));
	brainfunk->size.stack = stacksize;

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
