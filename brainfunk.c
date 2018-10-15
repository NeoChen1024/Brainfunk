/* ==================================== *
 * Brainfunk -- A Brainf**k Interpreter *
 * Neo_Chen			        *
 * ==================================== */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <libbrainfunk.h>

/* init */

memory_t *memory;
unsigned int ptr=0;
stack_type *stack;
unsigned int stack_ptr=0;
code_t *code;
unsigned int code_ptr=0;

int debug=0;

unsigned int memsize=DEF_MEMSIZE;
unsigned int codesize=DEF_CODESIZE;
unsigned int stacksize=DEF_STACKSIZE;

int main(int argc, char **argv)
{
	/* Init */
	memory	= calloc(MEMSIZE, sizeof(memory_t));
	code	= calloc(CODESIZE, sizeof(code_t));
	stack	= calloc(STACKSIZE, sizeof(stack_type));

	/* Parse Argument */
	FILE *corefile;
	int opt;

	if(!(argc >= 2))
	{
		puts("argc < 2!");
	}
	else
	{
		while((opt = getopt(argc, argv, "hdf:c:s:")) != -1)
		{
			switch(opt)
			{
				case 's': /* Read size from argument */
#ifndef FAST
					sscanf(optarg, "%u,%u,%u", &memsize, &codesize, &stacksize);
					if(memsize == 0 || codesize == 0 || stacksize == 0)
						panic("?SIZE=0");
					free(memory);
					free(code);
					free(stack);
					memory	= calloc(memsize, sizeof(memory_t));
					code	= calloc(codesize, sizeof(code_t));
					stack	= calloc(stacksize, sizeof(stack_type));
#endif
					break;
				case 'f': /* File */
					if(strcmp(optarg, "-"))
					{
						if((corefile = fopen(optarg, "r")) == NULL)
						{
							perror(optarg);
							exit(8);
						}
						read_code(corefile);
						fclose(corefile);
					}
					else
						read_code(stdin);
					break;
				case 'c': /* Code */
					strncpy(code, optarg, CODESIZE);
					break;
				case 'h': /* Help */
					printf("Usage: %s [-h] [-f file] [-c code] [-s memsize,codesize,stacksize] [-d]\n", argv[0]);
					break;
				case 'd': /* Debug */
					puts("Enabled Debug verbose message");
					debug = TRUE;
					break;
				default:
					exit(1);

			}
		}
	}

	if(debug)
	{
		fprintf(stderr, "memory	= %p[%d]\n", memory, MEMSIZE);
		fprintf(stderr, "code	= %p[%d]\n", code, CODESIZE);
		fprintf(stderr, "stack	= %p[%d]\n\n", stack, STACKSIZE);
		fflush(NULL);
	}

	while(code[code_ptr] != '\0')
	{
#ifndef FAST
		if(debug)
			debug_output();
#endif
		interprete(code[code_ptr]);
	}

	free(memory);
	free(code);
	free(stack);
	return 0;
}
