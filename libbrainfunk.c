/* ==================================== *
 * Brainfunk -- A Brainf**k Toolkit     *
 * Neo_Chen			        *
 * ==================================== */

/* Instructions:

	+	++(*ptr)
	-	--(*ptr)
	>	++ptr
	<	--ptr
	[	while(*ptr) {
	]	}
	.	putchar(*ptr)
	,	*ptr = getchar()
*/

/* You need to define
 * 	void output(memory_t c)
 * and
 * 	memory_t input(void)
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <libbrainfunk.h>

extern memory_t *memory;
extern unsigned int ptr;
extern stack_type *stack;
extern unsigned int stack_ptr;
extern code_t *code;
extern unsigned int code_ptr;
extern memory_t *pstack;
extern unsigned int pstack_ptr;

extern int debug;

extern size_t memsize;
extern size_t codesize;
extern size_t stacksize;
extern size_t pstacksize;
extern bitcode_t *bitcode;
extern unsigned int bitcode_ptr;

struct bitcode_ref_s bitcode_ref[OP_INSTS] =
{
	[OP_NOP] =
	{
		.name	= "NOP",
		.format	= "%x: NOP %x"
	},
	[OP_ADD] =
	{
		.name	= "ADD",
		.format = "%x: ADD %x"
	},
	[OP_SUB] =
	{
		.name	= "SUB",
		.format = "%x: SUB %x"
	},
	[OP_FWD] =
	{
		.name	= "FWD",
		.format = "%x: FWD %x"
	},
	[OP_REW] =
	{
		.name	= "REW",
		.format = "%x: REW %x"
	},
	[OP_JEZ] =
	{
		.name	= "JEZ",
		.format = "%x: JEZ %x"
	},
	[OP_JNZ] =
	{
		.name	= "JNZ",
		.format = "%x: JNZ %x"
	},
	[OP_IO] =
	{
		.name	= "IO",
		.format = "%x: IO %x"
	},
	[OP_SET] =
	{
		.name	= "SET",
		.format = "%x: SET %x"
	},
	[OP_POP] =
	{
		.name	= "POP",
		.format = "%x: POP %x"
	},
	[OP_PUSH] =
	{
		.name	= "PUSH",
		.format = "%x: PUSH %x"
	},
	[OP_PSHI] =
	{
		.name	= "PSHI",
		.format = "%x: PSHI %x"
	},
	[OP_ADDS] =
	{
		.name	= "ADDS",
		.format = "%x: ADDS %x"
	},
	[OP_SUBS] =
	{
		.name	= "SUBS",
		.format = "%x: SUBS %x"
	},
	[OP_JMP] =
	{
		.name	= "JMP",
		.format = "%x: JMP %x"
	},
	[OP_JSEZ] =
	{
		.name	= "JSEZ",
		.format = "%x: JSEZ %x"
	},
	[OP_JSNZ] =
	{
		.name	= "JSNZ",
		.format = "%x: JSNZ %x"
	},
	[OP_FRK] =
	{
		.name	= "FRK",
		.format = "%x: FRK %x"
	},
	[OP_HCF] =
	{
		.name	= "HCF",
		.format = "%x: HCF %x"
	},
	[OP_HLT] =
	{
		.name	= "HLT",
		.format = "%x: HLT %x"
	}
};

char vaild_code[256] =
{
	['+']=1,
	['-']=1,
	['>']=1,
	['<']=1,
	['[']=1,
	[']']=1,
	['$']=1,
	['\\']=1,
	['/']=1,
	['(']=1,
	[')']=1,
	['\'']=1,
	['~']=1,
	['!']=1,
	['_']=1
};

int is_code(int c)
{
	if(vaild_code[c & 0xFF])
	{
		return TRUE;
	}
	else
		return FALSE;
}

/* Read Code */
void read_code(char *code, FILE* fp)
{
	unsigned int i=0;
	int c=0;
	while((c=getc(fp)) != EOF)
	{
		if(i >= codesize)
			panic("?CODE");
		if((is_code((char)c) == TRUE) || c == '\t' || c == ' ' || c == '\n')
		code[i++]=(char)c;
	}
}

void push(stack_type *stack, unsigned int *ptr, stack_type content)
{
	if(++(*ptr) >= stacksize)
		panic("?>STACK");
	stack[(*ptr)]=content;
}

stack_type pop(stack_type *stack, unsigned int *ptr)
{
	if(*ptr == 0)
		panic("?<STACK");
	return stack[(*ptr)--];
}

#define pstack_peek	pstack[pstack_ptr]

void pstack_push(memory_t *stack, unsigned int *ptr, memory_t content)
{
	if(++*ptr >= pstacksize)
		panic("?>STACK");
	stack[(*ptr)]=content;
}

memory_t pstack_pop(memory_t *stack, unsigned int *ptr)
{
	if(*ptr == 0)
		panic("?<STACK");
	return stack[(*ptr)--];
}

void bitcodelize(bitcode_t *bitcode, size_t bitcodesize, code_t *text)
{
	unsigned int temp_arg=0;
	unsigned int text_ptr=0;
	unsigned int bitcode_ptr=0;
	int use_stack=FALSE;

	while(text[text_ptr] != '\0')
	{
		if(debug)
			printf("text[%u] == '%c'\n", text_ptr, text[text_ptr]);
		temp_arg=0;

switch_start:	/* Entering stack mode jumps back to here */
		switch(text[text_ptr])
		{
			case '$':	/* Stack Mode */
				use_stack=TRUE;
				text_ptr++;
				goto switch_start;
				break;
			case '+':
				while(text[text_ptr] == '+')
				{
					text_ptr++;
					temp_arg++;
				}
				if(use_stack)
					(bitcode + bitcode_ptr)->op=OP_ADDS;
				else
					(bitcode + bitcode_ptr)->op=OP_ADD;
				(bitcode + bitcode_ptr)->arg=temp_arg;
				bitcode_ptr++;
				break;
			case '-':
				while(text[text_ptr] == '-')
				{
					text_ptr++;
					temp_arg++;
				}
				if(use_stack)
					(bitcode + bitcode_ptr)->op=OP_SUBS;
				else
					(bitcode + bitcode_ptr)->op=OP_SUB;
				(bitcode + bitcode_ptr)->arg=temp_arg;
				bitcode_ptr++;
				break;
			case '>':
				while(text[text_ptr] == '>')
				{
					text_ptr++;
					temp_arg++;
				}
				(bitcode + bitcode_ptr)->op=OP_FWD;
				(bitcode + bitcode_ptr)->arg=temp_arg;
				bitcode_ptr++;
				break;
			case '<':
				while(text[text_ptr] == '<')
				{
					text_ptr++;
					temp_arg++;
				}
				(bitcode + bitcode_ptr)->op=OP_REW;
				(bitcode + bitcode_ptr)->arg=temp_arg;
				bitcode_ptr++;
				break;
			case '[':
				push(stack, &stack_ptr, bitcode_ptr);
				if(use_stack)
					(bitcode + bitcode_ptr)->op=OP_JSEZ;
				else
					(bitcode + bitcode_ptr)->op=OP_JEZ;
				text_ptr++;
				bitcode_ptr++;
				break;
			case ']':
				temp_arg=pop(stack, &stack_ptr);
				if(use_stack)
					(bitcode + bitcode_ptr)->op=OP_JSNZ;
				else
					(bitcode + bitcode_ptr)->op=OP_JNZ;
				(bitcode + bitcode_ptr)->arg=temp_arg;
				(bitcode + temp_arg)->arg=bitcode_ptr;
				text_ptr++;
				bitcode_ptr++;
				break;
			case '.':
				(bitcode + bitcode_ptr)->op=OP_IO;
				if(use_stack)
					(bitcode + bitcode_ptr)->arg=ARG_OUTS;
				else
					(bitcode + bitcode_ptr)->arg=ARG_OUT;
				text_ptr++;
				bitcode_ptr++;
				break;
			case ',':
				(bitcode + bitcode_ptr)->op=OP_IO;
				if(use_stack)
					(bitcode + bitcode_ptr)->arg=ARG_INS;
				else
					(bitcode + bitcode_ptr)->arg=ARG_IN;
				text_ptr++;
				bitcode_ptr++;
				break;
			case '\\':
				(bitcode + bitcode_ptr)->op=OP_POP;
				(bitcode + bitcode_ptr)->arg=0;
				text_ptr++;
				bitcode_ptr++;
				break;
			case '/':
				(bitcode + bitcode_ptr)->op=OP_PUSH;
				(bitcode + bitcode_ptr)->arg=0;
				text_ptr++;
				bitcode_ptr++;
				break;
			case '(':
				push(stack, &stack_ptr, bitcode_ptr);
				if(use_stack)
					(bitcode + bitcode_ptr)->op=OP_JSEZ;
				else
					(bitcode + bitcode_ptr)->op=OP_JEZ;
				text_ptr++;
				bitcode_ptr++;
				break;
			case ')':
				(bitcode + pop(stack, &stack_ptr))->arg=bitcode_ptr;
				text_ptr++;
				/* No jump back or special effect, so we don't increment bitcode_ptr */
				break;
			case '\'':
				sscanf(text + text_ptr + 1, "%x", &temp_arg);
				if(use_stack)
					(bitcode + bitcode_ptr)->op=OP_PSHI;
				else
					(bitcode + bitcode_ptr)->op=OP_SET;
				(bitcode + bitcode_ptr)->arg = temp_arg & 0xFF;
				text_ptr++;
				bitcode_ptr++;
				break;
			case '~':
				(bitcode + bitcode_ptr)->op=OP_FRK;
				text_ptr++;
				bitcode_ptr++;
				break;
			case '!':
				(bitcode + bitcode_ptr)->op=OP_HCF;
				text_ptr++;
				bitcode_ptr++;
				break;
			case '_':
				(bitcode + bitcode_ptr)->op=OP_HLT;
				text_ptr++;
				bitcode_ptr++;
				break;
			default:
				text_ptr++;
				break;
		}
		use_stack=FALSE; /* Exit stack mode */

		if(bitcode_ptr >= bitcodesize)
			panic("?BITCODE");
	}
	(bitcode + bitcode_ptr)->op=OP_HLT;
}

void bitcode_interprete(bitcode_t *bitcode)
{
	switch(bitcode->op)
	{
		case OP_ADD:
			memory[ptr] += bitcode->arg;
			bitcode_ptr++;
			break;
		case OP_ADDS:
			pstack_peek += bitcode->arg;
			bitcode_ptr++;
			break;
		case OP_SUB:
			memory[ptr] -= bitcode->arg;
			bitcode_ptr++;
			break;
		case OP_SUBS:
			pstack_peek -= bitcode->arg;
			bitcode_ptr++;
			break;
		case OP_FWD:
			ptr += bitcode->arg;
			if(ptr >= memsize)
				panic("?>MEM");
			bitcode_ptr++;
			break;
		case OP_REW:
			ptr -= bitcode->arg;
			if(ptr >= memsize)
				panic("?<MEM");
			bitcode_ptr++;
			break;
		case OP_JEZ:
			if(memory[ptr] == 0)
				bitcode_ptr = bitcode->arg;
			else
				bitcode_ptr++;
			break;
		case OP_JSEZ:
			if(pstack_peek == 0)
				bitcode_ptr = bitcode->arg;
			else
				bitcode_ptr++;
			break;
		case OP_JNZ:
			if(memory[ptr] != 0)
				bitcode_ptr = bitcode->arg;
			else
				bitcode_ptr++;
			break;
		case OP_JSNZ:
			if(pstack_peek != 0)
				bitcode_ptr = bitcode->arg;
			else
				bitcode_ptr++;
			break;
		case OP_JMP:
			bitcode_ptr = bitcode->arg;
			break;
		case OP_SET:
			memory[ptr] = (memory_t)bitcode->arg;
			bitcode_ptr++;
			break;
		case OP_PUSH:
			pstack_push(pstack, &pstack_ptr, memory[ptr]);
			bitcode_ptr++;
			break;
		case OP_POP:
			memory[ptr] = pstack_pop(pstack, &pstack_ptr);
			bitcode_ptr++;
			break;
		case OP_PSHI:
			pstack_push(pstack, &pstack_ptr, (memory_t)bitcode->arg);
			bitcode_ptr++;
			break;
		case OP_IO:
			if(bitcode->arg == ARG_OUT)
				output(memory[ptr]);
			else if(bitcode->arg == ARG_IN)
				memory[ptr] = input();
			else if(bitcode->arg == ARG_OUTS)
				output(pstack_pop(pstack, &pstack_ptr));
			else if(bitcode->arg == ARG_INS)
				pstack_push(pstack, &pstack_ptr, input());
			bitcode_ptr++;
			break;
		case OP_FRK:
			memory[ptr] = (memory_t)fork();
			bitcode_ptr++;
			break;
		case OP_HCF:
			*(volatile int*)NULL=TRUE;
			bitcode_ptr++;
			break;
		case OP_NOP:
		case OP_HLT:
		default:
			break;
	}
	if(bitcode_ptr >= codesize)
		panic("?RUNCODE");
	return;
}

void bitcode_disassembly(bitcode_t *bitcode, unsigned int address, char *str, size_t strsize)
{
	snprintf(str, strsize, bitcode_ref[bitcode->op].format, address, bitcode->arg);
	return;
}

void bitcode_disassembly_array_to_fp(bitcode_t *bitcode, FILE *fp)
{
	char temp_str[64];
	unsigned int counter=0;

	do
	{
		bitcode_disassembly(bitcode + counter, counter, temp_str, 64);
		fprintf(fp, "%s\n", temp_str);
		counter++;
	}
	while((bitcode + counter)->op != OP_HLT);
}

void bitcode_assembly(char *str, bitcode_t *bitcode)
{
	unsigned int address=0;
	int op=0;
	unsigned int arg=0;
	int ret=0;

	while(ret != 2 && ret != EOF)
	{
		ret = sscanf(str, bitcode_ref[op].format, &address, &arg);
		if(ret != 2)
			op++;
	}

	if(debug)
		printf("%x, %#x\n", op, arg);

	(bitcode + address)->op = op;
	(bitcode + address)->arg = arg;
}


void bitcode_load_fp(bitcode_t *bitcode, FILE *fp)
{
	char *temp_str=NULL;
	size_t str_cap=0;
	ssize_t str_length=0;

	while((str_length = getline(&temp_str, &str_cap, fp)) > 0)
	{
		if(debug)
			printf("LOAD [%ld]: %s\n", str_length, temp_str);
		bitcode_assembly(temp_str, bitcode);
	}
}
