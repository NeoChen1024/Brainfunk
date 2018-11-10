/* ==================================== *
 * Brainfunk -- A Brainf**k Toolkit     *
 * Neo_Chen			        *
 * ==================================== */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <libbrainfunk.h>

/* init */

memory_t *memory;
unsigned int ptr=0;
stack_type *stack;
unsigned int stack_ptr=0;
code_t *code;
unsigned int code_ptr=0;
int debug=0;

size_t memsize=DEF_MEMSIZE;
size_t codesize=DEF_CODESIZE;
size_t stacksize=DEF_STACKSIZE;

size_t bitcodesize=DEF_BITCODESIZE;
bitcode_t *bitcode;
unsigned int bitcode_ptr=0;
memory_t *pstack;
unsigned int pstack_ptr=0;
size_t pstacksize=DEF_PSTACKSIZE;

void debug_function(void)
{
	char disassembly_code[64];

	bitcode_disassembly(bitcode + bitcode_ptr, bitcode_ptr, disassembly_code, 64);
	fprintf(stderr, "code=%s\n", disassembly_code);
	fprintf(stderr, "stack=%u:0x%0x\n", stack_ptr, stack[stack_ptr]);
	fprintf(stderr, "ptr=%0x:0x%0x\n", ptr, memory[ptr]);
	fprintf(stderr, "--------\n");
	fflush(NULL);
}

void panic(char *msg)
{
	fprintf(stderr, "%s\n", msg);
	exit(2);
}

void debug_loop(char *fmt, unsigned int location)
{
	fprintf(stderr, fmt, location);
}

void output(memory_t c)
{
	putchar(c);
}

memory_t input(void)
{
	return (memory_t)getchar();
}

int main(int argc, char **argv)
{
	/* Init */
	memory	= calloc(memsize, sizeof(memory_t));
	code	= calloc(codesize, sizeof(code_t));
	stack	= calloc(stacksize, sizeof(stack_type));
	bitcode	= calloc(bitcodesize, sizeof(bitcode_t));
	pstack	= calloc(pstacksize, sizeof(memory_t));
	int load_bitcode=0;
	setvbuf(stdin, NULL, _IONBF, 0);
	setvbuf(stdout, NULL, _IONBF, 0);

	/* Parse Argument */
	FILE *corefile;
	int opt;

	if(!(argc >= 2))
	{
		puts("?ARG");
	}
	else
	{
		while((opt = getopt(argc, argv, "hdf:c:s:b:")) != -1)
		{
			switch(opt)
			{
				case 's': /* Read size from argument */
					sscanf(optarg, "%zu,%zu,%zu,%zu", &memsize, &codesize, &pstacksize, &bitcodesize);
					if(memsize == 0 || codesize == 0 || stacksize == 0 || bitcodesize == 0)
						panic("?SIZE=0");
					free(memory);
					free(code);
					free(stack);
					memory	= calloc(memsize, sizeof(memory_t));
					code	= calloc(codesize, sizeof(code_t));
					stack	= calloc(stacksize, sizeof(stack_type));
					bitcode	= calloc(bitcodesize, sizeof(bitcode_t));
					break;
				case 'f': /* File */
					if(strcmp(optarg, "-"))
					{
						if((corefile = fopen(optarg, "r")) == NULL)
						{
							perror(optarg);
							exit(8);
						}
						read_code(code, corefile);
						fclose(corefile);
					}
					else
						read_code(code, stdin);
					break;
				case 'b':
					if(strcmp(optarg, "-"))
					{
						if((corefile = fopen(optarg, "r")) == NULL)
						{
							perror(optarg);
							exit(8);
						}
						bitcode_load_fp(bitcode, corefile);
						fclose(corefile);
					}
					else
						bitcode_load_fp(bitcode, stdin);
					load_bitcode=1;
					break;
				case 'c': /* Code */
					strncpy(code, optarg, codesize);
					break;
				case 'h': /* Help */
					printf("Usage: %s [-h] [-f file] [-c code] [-s memsize,codesize,pstacksize,bitcodesize] [-d]\n", argv[0]);
					break;
				case 'd': /* Debug */
					puts("DEBUG=TRUE");
					debug = TRUE;
					break;
				default:
					exit(1);

			}
		}
	}

	if(debug)
	{
		fprintf(stderr, "memory	= %p[%zu]\n", memory, memsize);
		fprintf(stderr, "code	= %p[%zu]\n", code, codesize);
		fprintf(stderr, "pstack = %p[%zu]\n", pstack, pstacksize);
		fflush(NULL);
	}

	if(!load_bitcode)
		bitcodelize(bitcode, bitcodesize, code);

	if(debug)
		bitcode_disassembly_array_to_fp(bitcode, stdout);

	while((bitcode + bitcode_ptr)->op != OP_HLT)
	{
		if(debug)
			debug_function();
		bitcode_interprete(bitcode + bitcode_ptr);
		if(bitcode_ptr >= bitcodesize)
			panic("?EXEC");
	}

	free(memory);
	free(code);
	free(stack);
	free(bitcode);
	free(pstack);
	return 0;
}
