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

memory_t *memory;
arg_t ptr=0;
stack_type *stack;
arg_t stack_ptr=0;
code_t *code;
arg_t code_ptr=0;
bitcode_t *bitcode;
arg_t bitcode_ptr=0;
memory_t *pstack;
arg_t pstack_ptr=0;

size_t memsize=DEF_MEMSIZE;
size_t codesize=DEF_CODESIZE;
size_t stacksize=DEF_STACKSIZE;
size_t bitcodesize=DEF_BITCODESIZE;
size_t pstacksize=DEF_PSTACKSIZE;

int debug=0;
int compat=0;

FILE *corefile=NULL;
FILE *outfile=NULL;

void print_head(FILE *fp)
{
	fputs(
	"#include <stdio.h>\n"
	"#include <stdlib.h>\n"
	"#include <unistd.h>\n"
	"#include <libstdbfc.h>\n\n"
	"int main(void)\n"
	"{\n"
	"\tmemory = calloc(MEMSIZE, sizeof(char));\n"
	"\tpstack = calloc(MEMSIZE, sizeof(char));\n\n"
	"\tsetvbuf(stdout, NULL, _IONBF, 0);\n"
	"\tsetvbuf(stdin, NULL, _IONBF, 0);\n\n", fp);
}

void print_tail(FILE *fp)
{
	fputs(
	"}\n", fp);
}

void cleanup(arg_t arg)
{
	fclose(outfile);
	free(bitcode);
	free(stack);
	free(code);
	exit(arg);
}

int main(int argc, char **argv)
{
	code	= calloc(codesize, sizeof(code_t));
	bitcode	= calloc(bitcodesize, sizeof(bitcode_t));
	stack	= calloc(stacksize, sizeof(stack_type));

	int load_bitcode=0;
	outfile=stdout;
	int opt;

	if(argc <= 2)
	{
		panic("?ARG");
	}
	else
	{
		while((opt = getopt(argc, argv, "hmdf:s:c:o:b:")) != -1)
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
				case 'm': /* Compat */
					compat=1;
					break;
				case 'h':
				default:
					printf("Usage: %s [-h] [-m] -f infile [-b bitcode] [-o outfile] [-s codesize,bitcodesize] [-d]\n", argv[0]);
					exit(1);
			}
		}
	}

	if(!load_bitcode)
		bitcodelize(bitcode, bitcodesize, code);

	print_head(outfile);

	do
	{
		fprintf(outfile, bitcode_ref[(bitcode + bitcode_ptr)->op].cformat, bitcode_ptr, (bitcode + bitcode_ptr)->arg);
	}
	while((bitcode + bitcode_ptr++)->op != OP_HLT);
	print_tail(outfile);
}
