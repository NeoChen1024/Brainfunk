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
#include <libbitcode.h>

memory_t *memory;
unsigned int ptr=0;
stack_type *stack;
unsigned int stack_ptr=0;
code_t *code;
unsigned int code_ptr=0;
bitcode_t *bitcode;
unsigned int bitcode_ptr=0;
memory_t *pstack;
unsigned int pstack_ptr=0;

size_t memsize=DEF_MEMSIZE;
size_t codesize=DEF_CODESIZE;
size_t stacksize=DEF_STACKSIZE;
size_t bitcodesize=DEF_BITCODESIZE;
size_t pstacksize=DEF_PSTACKSIZE;

int debug=0;

void panic(char *msg)
{
	fprintf(stderr, "%s\n", msg);
	exit(2);
}

/* Placeholder Functions */
void output(memory_t c)
{
	return;
}

memory_t input(void)
{
	return 0;
}

void debug_loop(char *fmt, unsigned int location)
{
	return;
}

void print_head(FILE *fp)
{
	fputs(
	"#include <stdio.h>\n"
	"#include <stdlib.h>\n"
	"#include <unistd.h>\n"
	"#include <libstdbfc.h>\n\n"
	"int main(void)\n"
	"{\n"
	"\tmemory = calloc(MEMSIZE, sizeof(char));\n\n"
	"\tsetvbuf(stdout, NULL, _IONBF, 0);\n"
	"\tsetvbuf(stdin, NULL, _IONBF, 0);\n\n", fp);
}

void print_tail(FILE *fp)
{
	fputs(
	"}\n", fp);
}

void print_c(bitcode_t *bitcode, unsigned int address, char *str, size_t strsize)
{
	switch(bitcode->op)
	{
		case OP_ADD:
			snprintf(str, strsize, "L%#x:\tadd(%#x);\n", address, bitcode->arg);
			break;
		case OP_ADDS:
			snprintf(str, strsize, "L%#x:\tadds(%#x);\n", address, bitcode->arg);
			break;
		case OP_SUB:
			snprintf(str, strsize, "L%#x:\tsub(%#x);\n", address, bitcode->arg);
			break;
		case OP_SUBS:
			snprintf(str, strsize, "L%#x:\tsubs(%#x);\n", address, bitcode->arg);
			break;
		case OP_FWD:
			snprintf(str, strsize, "L%#x:\tfwd(%#x);\n", address, bitcode->arg);
			break;
		case OP_REW:
			snprintf(str, strsize, "L%#x:\trew(%#x);\n", address, bitcode->arg);
			break;
		case OP_JEZ:
			snprintf(str, strsize, "L%#x:\tif(!memory[ptr]) goto L%#x;\n", address, bitcode->arg);
			break;
		case OP_JSEZ:
			snprintf(str, strsize, "L%#x:\tif(!peek) goto L%#x;\n", address, bitcode->arg);
			break;
		case OP_JNZ:
			snprintf(str, strsize, "L%#x:\tif(memory[ptr]) goto L%#x;\n", address, bitcode->arg);
			break;
		case OP_JSNZ:
			snprintf(str, strsize, "L%#x:\tif(peek) goto L%#x;\n", address, bitcode->arg);
			break;
		case OP_JMP:
			snprintf(str, strsize, "L%#x:\tgoto L%#x;\n", address, bitcode->arg);
			break;
		case OP_IO:
			if(bitcode->arg == ARG_OUT)
				snprintf(str, strsize, "L%#x:\tout(memory[ptr]);\n", address);
			else if(bitcode->arg == ARG_IN)
				snprintf(str, strsize, "L%#x:\tmemory[ptr] = in();\n", address);
			else if(bitcode->arg == ARG_OUTS)
				snprintf(str, strsize, "L%#x:\tout(pop(stack, &stack_ptr));\n", address);
			else if(bitcode->arg == ARG_INS)
				snprintf(str, strsize, "L%#x:\tpush(stack, &stack_ptr, in());\n", address);
			break;
		case OP_SET:
			snprintf(str, strsize, "L%#x:\tset(%#x);\n", address, bitcode->arg);
			break;
		case OP_POP:
			snprintf(str, strsize, "L%#x:\tmemory[ptr] = pop(stack, &stack_ptr); /* ARG=%#x */\n", address, bitcode->arg);
			break;
		case OP_PUSH:
			snprintf(str, strsize, "L%#x:\tpush(stack, &stack, memory[ptr]); /* ARG=%#x */\n", address, bitcode->arg);
			break;
		case OP_PSHI:
			snprintf(str, strsize, "L%#x:\tpush(stack, &stack, %#x);\n", address, bitcode->arg);
			break;
		case OP_FRK:
			snprintf(str, strsize, "L%#x:\tfrk(); /* ARG=%#x */\n", address, bitcode->arg);
			break;
		case OP_HCF:
			snprintf(str, strsize, "L%#x:\thcf(%#x);\n", address, bitcode->arg);
			break;
		case OP_NOP:
			snprintf(str, strsize, "L%#x:\t/* NOP %#x */\n", address, bitcode->arg);
			break;
		case OP_HLT:
			snprintf(str, strsize, "L%#x:\thlt(%#x);\n", address, bitcode->arg);
			break;
	}
	return;
}

int main(int argc, char **argv)
{
	code	= calloc(codesize, sizeof(code_t));
	bitcode	= calloc(bitcodesize, sizeof(bitcode_t));
	stack	= calloc(stacksize, sizeof(stack_type));

	int load_bitcode=0;
	FILE *corefile=NULL;
	FILE *outfile=stdout;
	int opt;

	if(argc <= 2)
	{
		panic("?ARG");
	}
	else
	{
		while((opt = getopt(argc, argv, "hdf:s:c:o:b:")) != -1)
		{
			switch(opt)
			{
				case 'd':
					debug=1;
					break;
				case 's': /* Read size from argument */
					sscanf(optarg, "%zu,%zu", &codesize, &bitcodesize);
					if(codesize == 0 || bitcodesize == 0)
						panic("?SIZE=0");
					free(memory);
					free(code);
					code	= calloc(codesize, sizeof(code_t));
					bitcode	= calloc(bitcodesize, sizeof(bitcode_t));
					break;
				case 'c':
					strncpy(code, optarg, codesize);
					break;
				case 'f':
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
				case 'o':
					if(strcmp(optarg, "-"))
					{
						if((outfile = fopen(optarg, "w")) == NULL)
						{
							perror(optarg);
							exit(8);
						}
					}
					else
						outfile=stdout;
					break;
				case 'h':
				default:
					printf("Usage: %s [-h] -f infile [-b bitcode] [-o outfile] [-s codesize,bitcodesize] [-d]\n", argv[0]);
					exit(1);
			}
		}
	}

	if(!load_bitcode)
		bitcodelize(bitcode, bitcodesize, code);

	char outstr[256];

	print_head(outfile);
	do
	{
		print_c(bitcode + bitcode_ptr, bitcode_ptr, outstr, 256);
		fputs(outstr, outfile);
	}
	while((bitcode + bitcode_ptr++)->op != OP_HLT);
	print_tail(outfile);

	free(bitcode);
	free(stack);
	free(code);
}
