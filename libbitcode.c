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

extern int debug;

extern unsigned int memsize;
extern unsigned int codesize;
extern unsigned int stacksize;
extern bitcode_t *bitcode;
extern unsigned int bitcode_ptr;

void bitcodelize(bitcode_t *bitcode, size_t bitcodesize, code_t *text)
{
	unsigned int temp_arg=0;
	unsigned int text_ptr=0;
	unsigned int bitcode_ptr=0;
	while(text[text_ptr] != '\0')
	{
		if(debug)
			printf("text[%u] == '%c'\n", text_ptr, text[text_ptr]);
		temp_arg=0;
		switch(text[text_ptr])
		{
			case '+':
				while(text[text_ptr] == '+')
				{
					text_ptr++;
					temp_arg++;
				}
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
				(bitcode + bitcode_ptr)->op=OP_JEZ;
				text_ptr++;
				bitcode_ptr++;
				break;
			case ']':
				temp_arg=pop(stack, &stack_ptr);
				(bitcode + bitcode_ptr)->op=OP_JNZ;
				(bitcode + bitcode_ptr)->arg=temp_arg;
				(bitcode + temp_arg)->arg=bitcode_ptr;
				text_ptr++;
				bitcode_ptr++;
				break;
			case '.':
				(bitcode + bitcode_ptr)->op=OP_IO;
				(bitcode + bitcode_ptr)->arg=ARG_OUT;
				text_ptr++;
				bitcode_ptr++;
				break;
			case ',':
				(bitcode + bitcode_ptr)->op=OP_IO;
				(bitcode + bitcode_ptr)->arg=ARG_IN;
				text_ptr++;
				bitcode_ptr++;
				break;
			default:
				text_ptr++;
				break;
		}

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
			break;
		case OP_SUB:
			memory[ptr] -= bitcode->arg;
			break;
		case OP_FWD:
			ptr += bitcode->arg;
			if(ptr >= memsize)
				panic("?>MEM");
			break;
		case OP_REW:
			ptr -= bitcode->arg;
			if(ptr >= memsize)
				panic("?<MEM");
			break;
		case OP_JEZ:
			if(memory[ptr] == 0)
				bitcode_ptr = bitcode->arg;
			break;
		case OP_JNZ:
			if(memory[ptr] != 0)
				bitcode_ptr = bitcode->arg;
			break;
		case OP_IO:
			if(bitcode->arg == ARG_OUT)
				output(memory[ptr]);
			else
				memory[ptr] = input();
			break;
		case OP_NOP:
		case OP_HLT:
		default:
			break;
	}
	bitcode_ptr++;
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
	char temp_op_str[64];
	char temp_arg_str[32];
	char op=0;
	unsigned int arg=0;

	sscanf(str, "%x: %s %s\n", &address, temp_op_str, temp_arg_str);

	if(!strncmp(temp_op_str, "ADD", 64))
		op=OP_ADD;
	else if(!strncmp(temp_op_str, "SUB", 64))
		op=OP_SUB;
	else if(!strncmp(temp_op_str, "FWD", 64))
		op=OP_FWD;
	else if(!strncmp(temp_op_str, "REW", 64))
		op=OP_REW;
	else if(!strncmp(temp_op_str, "JEZ", 64))
		op=OP_JEZ;
	else if(!strncmp(temp_op_str, "JNZ", 64))
		op=OP_JNZ;
	else if(!strncmp(temp_op_str, "IO", 64))
		op=OP_IO;
	else if(!strncmp(temp_op_str, "HLT", 64))
		op=OP_HLT;
	else if(!strncmp(temp_op_str, "NOP", 64))
		op=OP_NOP;

	if(op == OP_IO)
	{
		if(!strncmp(temp_arg_str, "IN", 32))
			arg=ARG_IN;
		else if(!strncmp(temp_arg_str, "OUT", 32))
			arg=ARG_OUT;
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
