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

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <libbrainfunk.h>

extern memory_t *memory;
extern arg_t ptr;
extern stack_type *stack;
extern arg_t stack_ptr;
extern code_t *code;
extern arg_t code_ptr;
extern memory_t *pstack;
extern arg_t pstack_ptr;

extern int debug;
extern int compat;

extern size_t memsize;
extern size_t codesize;
extern size_t stacksize;
extern size_t pstacksize;
extern bitcode_t *bitcode;
extern arg_t bitcode_ptr;

struct bitcode_ref_s bitcode_ref[OP_INSTS] =
{
	[OP_HLT] =
	{
		.name	= "HLT",
		.format = "%x: HLT %x",
		.cformat= "L%#x:	hlt(%#x);\n",
		.handler= exec_hlt
	},
	[OP_ADD] =
	{
		.name	= "ADD",
		.format = "%x: ADD %x",
		.cformat= "L%#x:	add(%#x);\n",
		.handler= exec_add
	},
	[OP_SUB] =
	{
		.name	= "SUB",
		.format = "%x: SUB %x",
		.cformat= "L%#x:	sub(%#x);\n",
		.handler= exec_sub
	},
	[OP_FWD] =
	{
		.name	= "FWD",
		.format = "%x: FWD %x",
		.cformat= "L%#x:	fwd(%#x);\n",
		.handler= exec_fwd
	},
	[OP_REW] =
	{
		.name	= "REW",
		.format = "%x: REW %x",
		.cformat= "L%#x:	rew(%#x);\n",
		.handler= exec_rew
	},
	[OP_JEZ] =
	{
		.name	= "JEZ",
		.format = "%x: JEZ %x",
		.cformat= "L%#x:	if(!memory[ptr]) goto L%#x;\n",
		.handler= exec_jez
	},
	[OP_JNZ] =
	{
		.name	= "JNZ",
		.format = "%x: JNZ %x",
		.cformat= "L%#x:	if(memory[ptr]) goto L%#x;\n",
		.handler= exec_jnz
	},
	[OP_IO] =
	{
		.name	= "IO",
		.format = "%x: IO %x",
		.cformat= "L%#x:	io(%#x);\n",
		.handler= exec_io
	},
	[OP_SET] =
	{
		.name	= "SET",
		.format = "%x: SET %x",
		.cformat= "L%#x:	set(%#x);\n",
		.handler= exec_set
	},
	[OP_POP] =
	{
		.name	= "POP",
		.format = "%x: POP %x",
		.cformat= "L%#x:	memory[ptr] = pop(stack, &stack_ptr); /* ARG=%#x */\n",
		.handler= exec_pop
	},
	[OP_PUSH] =
	{
		.name	= "PUSH",
		.format = "%x: PUSH %x",
		.cformat= "L%#x:	push(stack, &stack_ptr, memory[ptr]); /* ARG=%#x */\n",
		.handler= exec_push
	},
	[OP_PSHI] =
	{
		.name	= "PSHI",
		.format = "%x: PSHI %x",
		.cformat= "L%#x:	push(stack, &stack_ptr, %#x);\n",
		.handler= exec_pshi
	},
	[OP_ADDS] =
	{
		.name	= "ADDS",
		.format = "%x: ADDS %x",
		.cformat= "L%#x:	adds(%#x);\n",
		.handler= exec_adds
	},
	[OP_SUBS] =
	{
		.name	= "SUBS",
		.format = "%x: SUBS %x",
		.cformat= "L%#x:	subs(%#x);\n",
		.handler= exec_subs
	},
	[OP_JMP] =
	{
		.name	= "JMP",
		.format = "%x: JMP %x",
		.cformat= "L%#x:	goto L%#x;\n",
		.handler= exec_jmp
	},
	[OP_JSEZ] =
	{
		.name	= "JSEZ",
		.format = "%x: JSEZ %x",
		.cformat= "L%#x:	if(!pstack_peek) goto L%#x;\n",
		.handler= exec_jsez
	},
	[OP_JSNZ] =
	{
		.name	= "JSNZ",
		.format = "%x: JSNZ %x",
		.cformat= "L%#x:	if(pstack_peek) goto L%#x;\n",
		.handler= exec_jsnz
	},
	[OP_FRK] =
	{
		.name	= "FRK",
		.format = "%x: FRK %x",
		.cformat= "L%#x:	frk(%#x);\n",
		.handler= exec_frk
	},
	[OP_HCF] =
	{
		.name	= "HCF",
		.format = "%x: HCF %x",
		.cformat= "L%#x:	hcf(%#x);\n",
		.handler= exec_hcf
	}
};

char valid_code[256] =
{
	[0]=1,
	['+']=1,
	['-']=1,
	['>']=1,
	['<']=1,
	['[']=1,
	[']']=1,
	['.']=1,
	[',']=1,
	['$']=1,
	['\\']=1,
	['/']=1,
	['(']=1,
	[')']=1,
	['\'']=1,
	['~']=1,
	['!']=1,
	['_']=1,
	[255]=0
};

char compat_valid_code[256] =
{
	[0]=1,
	['+']=1,
	['-']=1,
	['>']=1,
	['<']=1,
	['[']=1,
	[']']=1,
	['.']=1,
	[',']=1,
	[255]=0
};

int is_code(int c)
{
	if(compat)
		return compat_valid_code[c & 0xFF];
	else
		return valid_code[c & 0xFF];
}

/* Read Code */
void read_code(char *code, FILE* fp)
{
	arg_t i=0;
	int c=0;
	while((c=getc(fp)) != EOF)
	{
		if(i >= codesize)
			panic("?CODE");
		if(is_code(c) == TRUE)
		code[i++]=(char)c;
	}
}

void push(stack_type *stack, arg_t *ptr, stack_type content)
{
	if(++(*ptr) >= stacksize)
		panic("?>STACK");
	stack[(*ptr)]=content;
}

stack_type pop(stack_type *stack, arg_t *ptr)
{
	if(*ptr == 0)
		panic("?<STACK");
	return stack[(*ptr)--];
}

#define pstack_peek	pstack[pstack_ptr]

void pstack_push(memory_t *stack, arg_t *ptr, memory_t content)
{
	if(++*ptr >= pstacksize)
		panic("?>STACK");
	stack[(*ptr)]=content;
}

memory_t pstack_pop(memory_t *stack, arg_t *ptr)
{
	if(*ptr == 0)
		panic("?<STACK");
	return stack[(*ptr)--];
}

void bitcodelize(bitcode_t *bitcode, size_t bitcodesize, code_t *text)
{
	arg_t temp_arg=0;
	arg_t text_ptr=0;
	arg_t bitcode_ptr=0;
	int use_stack=FALSE;

	while(text[text_ptr] != '\0')
	{
		if(debug)
			printf("text[%u] == '%c'\n", text_ptr, text[text_ptr]);
		temp_arg=0;

		while(is_code(text[text_ptr]) == FALSE)
			text_ptr++;

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
}

void bitcode_interprete(bitcode_t *bitcode)
{
	(bitcode_ref[bitcode->op].handler)(bitcode->arg);
	if(bitcode_ptr >= codesize)
		panic("?RUNCODE");
	return;
}

void bitcode_disassembly(bitcode_t *bitcode, arg_t address, char *str, size_t strsize)
{
	snprintf(str, strsize, bitcode_ref[bitcode->op].format, address, bitcode->arg);
	return;
}

void bitcode_disassembly_array_to_fp(bitcode_t *bitcode, FILE *fp)
{
	char temp_str[64];
	arg_t counter=0;

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
	arg_t address=0;
	int op=0;
	arg_t arg=0;
	int ret=0;

	while(ret != 2)
	{
		if(op >= OP_INSTS)
			panic("?INVALID");
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

#ifndef VISUAL
void panic(char *msg)
{
	fprintf(stderr, "%s\n", msg);
	exit(2);
}

void debug_loop(char *fmt, arg_t location)
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
#endif

void exec_add(arg_t arg)
{
	memory[ptr] += arg;
	bitcode_ptr++;
}

void exec_adds(arg_t arg)
{
	pstack_peek += arg;
	bitcode_ptr++;
}

void exec_sub(arg_t arg)
{
	memory[ptr] -= arg;
	bitcode_ptr++;
}

void exec_subs(arg_t arg)
{
	pstack_peek -= arg;
	bitcode_ptr++;
}

void exec_fwd(arg_t arg)
{
	ptr += arg;
	if(ptr >= memsize)
		panic("?>MEM");
	bitcode_ptr++;
}

void exec_rew(arg_t arg)
{
	ptr -= arg;
	if(ptr >= memsize)
		panic("?<MEM");
	bitcode_ptr++;
}

void exec_jez(arg_t arg)
{
	if(memory[ptr] == 0)
		bitcode_ptr = arg;
	else
		bitcode_ptr++;
}

void exec_jsez(arg_t arg)
{
	if(pstack_peek == 0)
		bitcode_ptr = arg;
	else
		bitcode_ptr++;
}

void exec_jnz(arg_t arg)
{
	if(memory[ptr] != 0)
		bitcode_ptr = arg;
	else
		bitcode_ptr++;
}

void exec_jsnz(arg_t arg)
{
	if(pstack_peek != 0)
		bitcode_ptr = arg;
	else
		bitcode_ptr++;
}

void exec_jmp(arg_t arg)
{
	bitcode_ptr = arg;
}

void exec_set(arg_t arg)
{
	memory[ptr] = (memory_t)arg;
	bitcode_ptr++;
}

void exec_push(arg_t arg)
{
	pstack_push(pstack, &pstack_ptr, memory[ptr]);
	bitcode_ptr++;
	arg++; /* Do useless thing to emit compiler warning */
}

void exec_pop(arg_t arg)
{
	memory[ptr] = pstack_pop(pstack, &pstack_ptr);
	bitcode_ptr++;
	arg++;
}

void exec_pshi(arg_t arg)
{
	pstack_push(pstack, &pstack_ptr, (memory_t)arg);
	bitcode_ptr++;
}

void exec_io(arg_t arg)
{
	switch(arg)
	{
		case ARG_OUT:
			output(memory[ptr]);
			break;
		case ARG_IN:
			memory[ptr] = input();
			break;
		case ARG_INS:
			output(pstack_pop(pstack, &pstack_ptr));
			break;
		case ARG_OUTS:
			pstack_push(pstack, &pstack_ptr, input());
			break;
	}
	bitcode_ptr++;
}

void exec_frk(arg_t arg)
{
			memory[ptr] = (memory_t)fork() | arg;
			bitcode_ptr++;
}

void exec_hcf(arg_t arg)
{
	exit(arg);
}

void exec_hlt(arg_t arg)
{
	cleanup(arg);
}
