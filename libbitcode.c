/* ==================================== *
 * Brainfunk -- A Brainf**k Toolkit     *
 * Neo_Chen			        *
 * ==================================== */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <libbrainfunk.h>
#include <libbitcode.h>

extern memory_t *memory;
extern unsigned int ptr;
extern stack_type *stack;
extern unsigned int stack_ptr;
extern memory_t *pstack;
extern unsigned int pstack_ptr;

extern int debug;

extern size_t memsize;
extern size_t codesize;
extern size_t stacksize;
extern size_t pstacksize;
extern bitcode_t *bitcode;
extern unsigned int bitcode_ptr;

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
			break;
		case OP_JSEZ:
			if(pstack_peek == 0)
				bitcode_ptr = bitcode->arg;
			break;
		case OP_JNZ:
			if(memory[ptr] != 0)
				bitcode_ptr = bitcode->arg;
			break;
		case OP_JSNZ:
			if(pstack_peek != 0)
				bitcode_ptr = bitcode->arg;
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
	switch(bitcode->op)
	{
		case OP_ADD:
			snprintf(str, strsize, "%#x: ADD %#x", address, bitcode->arg);
			break;
		case OP_SUB:
			snprintf(str, strsize, "%#x: SUB %#x", address, bitcode->arg);
			break;
		case OP_FWD:
			snprintf(str, strsize, "%#x: FWD %#x", address, bitcode->arg);
			break;
		case OP_REW:
			snprintf(str, strsize, "%#x: REW %#x", address, bitcode->arg);
			break;
		case OP_JEZ:
			snprintf(str, strsize, "%#x: JEZ %#x", address, bitcode->arg);
			break;
		case OP_JNZ:
			snprintf(str, strsize, "%#x: JNZ %#x", address, bitcode->arg);
			break;
		case OP_IO:
			if(bitcode->arg == ARG_OUT)
				snprintf(str, strsize, "%#x: IO OUT", address);
			else if(bitcode->arg == ARG_IN)
				snprintf(str, strsize, "%#x: IO IN", address);
			else if(bitcode->arg == ARG_OUTS)
				snprintf(str, strsize, "%#x: IO OUTS", address);
			else if(bitcode->arg == ARG_INS)
				snprintf(str, strsize, "%#x: IO INS", address);
			break;
		case OP_SET:
			snprintf(str, strsize, "%#x: SET %#x", address, bitcode->arg);
			break;
		case OP_POP:
			snprintf(str, strsize, "%#x: POP %#x", address, bitcode->arg);
			break;
		case OP_PUSH:
			snprintf(str, strsize, "%#x: PUSH %#x", address, bitcode->arg);
			break;
		case OP_PSHI:
			snprintf(str, strsize, "%#x: PSHI %#x", address, bitcode->arg);
			break;
		case OP_ADDS:
			snprintf(str, strsize, "%#x: ADDS %#x", address, bitcode->arg);
			break;
		case OP_SUBS:
			snprintf(str, strsize, "%#x: SYBS %#x", address, bitcode->arg);
			break;
		case OP_JSEZ:
			snprintf(str, strsize, "%#x: JSEZ %#x", address, bitcode->arg);
			break;
		case OP_JSNZ:
			snprintf(str, strsize, "%#x: JSNZ %#x", address, bitcode->arg);
			break;
		case OP_JMP:
			snprintf(str, strsize, "%#x: JMP %#x", address, bitcode->arg);
			break;
		case OP_FRK:
			snprintf(str, strsize, "%#x: FRK %#x", address, bitcode->arg);
			break;
		case OP_HCF:
			snprintf(str, strsize, "%#x: HCF %#x", address, bitcode->arg);
			break;
		case OP_NOP:
			snprintf(str, strsize, "%#x: NOP %#x", address, bitcode->arg);
			break;
		case OP_HLT:
			snprintf(str, strsize, "%#x: HLT %#x", address, bitcode->arg);
			break;
	}
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
	char temp_op_str[16];
	char temp_arg_str[32];
	char op=0;
	unsigned int arg=0;

	sscanf(str, "%x: %s %s\n", &address, temp_op_str, temp_arg_str);

	if(!strncmp(temp_op_str, "ADD", 16))
		op=OP_ADD;
	else if(!strncmp(temp_op_str, "ADDS", 16))
		op=OP_ADDS;
	else if(!strncmp(temp_op_str, "SUB", 16))
		op=OP_SUB;
	else if(!strncmp(temp_op_str, "SUBS", 16))
		op=OP_SUBS;
	else if(!strncmp(temp_op_str, "FWD", 16))
		op=OP_FWD;
	else if(!strncmp(temp_op_str, "REW", 16))
		op=OP_REW;
	else if(!strncmp(temp_op_str, "JEZ", 16))
		op=OP_JEZ;
	else if(!strncmp(temp_op_str, "JSEZ", 16))
		op=OP_JSEZ;
	else if(!strncmp(temp_op_str, "JNZ", 16))
		op=OP_JNZ;
	else if(!strncmp(temp_op_str, "JSNZ", 16))
		op=OP_JSNZ;
	else if(!strncmp(temp_op_str, "JMP", 16))
		op=OP_JMP;
	else if(!strncmp(temp_op_str, "IO", 16))
		op=OP_IO;
	else if(!strncmp(temp_op_str, "SET", 16))
		op=OP_SET;
	else if(!strncmp(temp_op_str, "POP", 16))
		op=OP_POP;
	else if(!strncmp(temp_op_str, "PUSH", 16))
		op=OP_PUSH;
	else if(!strncmp(temp_op_str, "PSHI", 16))
		op=OP_PSHI;
	else if(!strncmp(temp_op_str, "FRK", 16))
		op=OP_FRK;
	else if(!strncmp(temp_op_str, "HCF", 16))
		op=OP_HCF;
	else if(!strncmp(temp_op_str, "HLT", 16))
		op=OP_HLT;
	else if(!strncmp(temp_op_str, "NOP", 16))
		op=OP_NOP;

	if(op == OP_IO)
	{
		if(!strncmp(temp_arg_str, "IN", 32))
			arg=ARG_IN;
		else if(!strncmp(temp_arg_str, "OUT", 32))
			arg=ARG_OUT;
		else if(!strncmp(temp_arg_str, "INS", 32))
			arg=ARG_INS;
		else if(!strncmp(temp_arg_str, "OUTS", 32))
			arg=ARG_OUTS;
	}
	else
		sscanf(temp_arg_str, "%x", &arg);

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
