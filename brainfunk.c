/* ==================================== *
 * Brainfunk -- A Brainf**k Interpreter *
 * Neo_Chen			        *
 * ==================================== */

/* Instructions:

Brainf**k Original:
	+	++(*ptr)
	-	--(*ptr)
	>	++ptr
	<	--ptr
	[	while(*ptr) {
	]	}
	.	putchar(*ptr)
	,	*ptr = getchar()

 * * * * * * * * * * * * * * * * * * * * * * * * 
 * 預估維護難度： 難於上青天                   *
 * 完成度： 0.d41d8cd98f00b204e9800998ecf8427e *
 *                                             *
 * 程式碼撰寫原則：                            *
 * 	• 不做錯誤處裡 ： 那太無聊了           *
 * 	• 不做有用的註釋 ： 那太浪費時間了     *
 * 	• 堅持用奇怪的方法達成簡單的事         *
 * * * * * * * * * * * * * * * * * * * * * * * *
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>

#define POWTWO(x) (1 << x)

#ifndef SIZEDEF
#define STACKSIZE 1024
#define MEMSIZE	262144
#define CODESIZE 65536
#endif

#define TRUE 1
#define FALSE 0
typedef uint8_t memory_t;
typedef unsigned int stack_t;
typedef char code_t;

/* init */

memory_t *memory;
unsigned int ptr=0;
stack_t *stack;
unsigned int stack_ptr=0;
code_t *code;
unsigned int code_ptr=0;

int debug=0;

void panic(char *msg)
{
	puts(msg);
	exit(2);
}

// Read Code
void read_code(FILE* fp)
{
	int i=0, c=0;
	while((c=getc(fp)) != EOF)
	{
#ifndef FAST
		if(i >= CODESIZE)
			panic("?CODE");
#endif
		code[i++]=(char)c;
	}
}

void push(stack_t *stack, unsigned int *ptr, stack_t content)
{
#ifdef FAST
	if(*ptr >= STACKSIZE)
		panic("?>STACK");
#endif
	stack[(*ptr)++]=content;
}

stack_t pop(stack_t *stack, unsigned int *ptr)
{
#ifdef FAST
	if(*ptr >= STACKSIZE)
		panic("?<STACK");
#endif
	return stack[--(*ptr)];
}

void debug_output(void)
{
	fprintf(stderr, "code=%u:%c\n", code_ptr, code[code_ptr]);
	fprintf(stderr, "stack=%u:0x%0x\n", stack_ptr, stack[stack_ptr]);
	fprintf(stderr, "ptr=%0x:0x%0x\n", ptr, memory[ptr]);
	fprintf(stderr, "--------\n");
	fflush(NULL);
}

void jump_to_next_matching(void)
{
	int nest_level=0;
	while(code[code_ptr] != '\0')
	{
		if(code[code_ptr] == '[')
			++nest_level;
		else if(code[code_ptr] == ']')
			--nest_level;
		else if(nest_level == 0)
			return;
		code_ptr++;
	}
}

void interprete(unsigned char c)
{
	switch(c)
	{
		case '+':
			memory[ptr]++;
			++code_ptr;
			break;
		case '-':
			memory[ptr]--;
			++code_ptr;
			break;
		case '>':
			ptr++;
#ifdef FAST
			if(ptr >= MEMSIZE)
				panic("?>MEM");
#endif
			++code_ptr;
			break;
		case '<':
			ptr--;
#ifndef FAST
			if(ptr >= MEMSIZE)
				panic("?<MEM");
#endif
			++code_ptr;
			break;
		case '[':
			if(memory[ptr] == 0)
			{
				jump_to_next_matching(); /* Skip everything until reached matching "]" */
#ifndef FAST
				if(debug)
					fprintf(stderr, "[:%d\n", code_ptr);
#endif
			}
			else
			{
				push(stack, &stack_ptr, code_ptr); /* Push current PC */
				code_ptr++;
			}
			break;
		case ']':
			if(memory[ptr] != 0) /* if not equals to 0 */
			{
				code_ptr=pop(stack, &stack_ptr);
#ifndef FAST
				if(debug)
					fprintf(stderr, "]:%d\n", code_ptr);
#endif
			}
			else
			{
				code_ptr++;
				pop(stack, &stack_ptr);
			}
			break;
		case ',':
			memory[ptr]=(char)getchar();
			++code_ptr;
			break;
		case '.':
			putchar(memory[ptr]);
			fflush(NULL);
			++code_ptr;
			break;
		default:
			++code_ptr;
			break;
	}
}

int main(int argc, char **argv)
{
	/* Init */
	memory	= calloc(MEMSIZE, sizeof(memory_t));
	stack	= calloc(STACKSIZE, sizeof(stack_t));
	code	= calloc(CODESIZE, sizeof(code_t));
#ifndef FAST
	if(debug)
	{
		fprintf(stderr, "memory = %p\n", memory);
		fprintf(stderr, "stack = %p\n\n", stack);
		fflush(NULL);
	}
#endif
	int i=0;
	for(i=0; i < CODESIZE; ++i) code[i]='\0';

	/* Parse Argument */
	FILE *corefile;
	int opt;

	if(!(argc >= 2))
		puts("argc < 2!");
	else
	{
		while((opt = getopt(argc, argv, "hdf:c:")) != -1)
		{
			switch(opt)
			{
				case 'f':
					if(strcmp(optarg, "-"))
					{
						if((corefile = fopen(optarg, "r")) == NULL)
						{
							perror(optarg);
							exit(8);
						}
						read_code(corefile);
					}
					else
						read_code(stdin);
					break;
				case 'c':
					strncpy(code, optarg, CODESIZE);
					break;
				case 'h':
					printf("Usage: %s [-h] [-f file] [-c code] [-d]\n", argv[0]);
					break;
				case 'd':
					puts("Enabled Debug verbose message");
					debug = TRUE;
					break;
				default:
					exit(1);
					break;

			}
		}
	}

	while(code[code_ptr] != '\0')
	{
#ifndef FAST
		if(debug)
			debug_output();
#endif
		interprete(code[code_ptr]);
	}
	return 0;
}
