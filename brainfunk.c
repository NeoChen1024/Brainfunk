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
#ifdef BITCODE
#	include <libbitcode.h>
#endif

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

#ifdef BITCODE
unsigned int bitcodesize=DEF_BITCODESIZE;
bitcode_t *bitcode;
unsigned int bitcode_ptr=0;
#endif

void debug_function(void)
{
#ifdef BITCODE
	char disassembly_code[64];

	bitcode_disassembly(bitcode + bitcode_ptr, bitcode_ptr, disassembly_code, 64);
	fprintf(stderr, "code=%s\n", disassembly_code);
#else
	fprintf(stderr, "code=%u:%c\n", code_ptr, code[code_ptr]);
#endif
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
	fflush(NULL);
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
#ifdef BITCODE
	bitcode	= calloc(bitcodesize, sizeof(bitcode_t));
	int load_bitcode=0;
#endif

	/* Parse Argument */
	FILE *corefile;
	int opt;

	if(!(argc >= 2))
	{
		puts("?ARG");
	}
	else
	{
#ifdef BITCODE
		while((opt = getopt(argc, argv, "hdf:c:s:b:")) != -1)
#else
		while((opt = getopt(argc, argv, "hdf:c:s:")) != -1)
#endif
		{
			switch(opt)
			{
				case 's': /* Read size from argument */
#ifdef BITCODE
					sscanf(optarg, "%u,%u,%u,%u", &memsize, &codesize, &stacksize, &bitcodesize);
					if(memsize == 0 || codesize == 0 || stacksize == 0 || bitcodesize == 0)
#else
					sscanf(optarg, "%u,%u,%u", &memsize, &codesize, &stacksize);
					if(memsize == 0 || codesize == 0 || stacksize == 0)
#endif
						panic("?SIZE=0");
					free(memory);
					free(code);
					free(stack);
					memory	= calloc(memsize, sizeof(memory_t));
					code	= calloc(codesize, sizeof(code_t));
					stack	= calloc(stacksize, sizeof(stack_type));
#ifdef BITCODE
					bitcode	= calloc(bitcodesize, sizeof(bitcode_t));
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
#ifdef BITCODE
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
#endif
				case 'c': /* Code */
					strncpy(code, optarg, codesize);
					break;
				case 'h': /* Help */
#ifdef BITCODE
					printf("Usage: %s [-h] [-f file] [-c code] [-s memsize,codesize,stacksize,bitcodesize] [-d]\n", argv[0]);
#else
					printf("Usage: %s [-h] [-f file] [-c code] [-s memsize,codesize,stacksize] [-d]\n", argv[0]);
#endif
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
		fprintf(stderr, "memory	= %p[%d]\n", memory, memsize);
		fprintf(stderr, "code	= %p[%d]\n", code, codesize);
		fprintf(stderr, "stack	= %p[%d]\n\n", stack, stacksize);
		fflush(NULL);
	}

#ifdef BITCODE
	if(!load_bitcode)
		bitcodelize(bitcode, code);

	if(debug)
		bitcode_disassembly_array_to_fp(bitcode, stdout);

	while((bitcode + bitcode_ptr)->op != OP_HLT)
	{
		if(debug)
			debug_function();
		bitcode_interprete(bitcode + bitcode_ptr);
	}

#else
	while(code[code_ptr] != '\0')
	{
		if(debug)
			debug_function();
		interprete(code[code_ptr]);
	}
#endif
	free(memory);
	free(code);
	free(stack);
#ifdef BITCODE
	free(bitcode);
#endif
	return 0;
}
