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

void bitcodelize(bitcode_t *bitcode, code_t *text)
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
				break;
			case '-':
				while(text[text_ptr] == '-')
				{
					text_ptr++;
					temp_arg++;
				}
				(bitcode + bitcode_ptr)->op=OP_SUB;
				(bitcode + bitcode_ptr)->arg=temp_arg;
				break;
			case '>':
				while(text[text_ptr] == '>')
				{
					text_ptr++;
					temp_arg++;
				}
				(bitcode + bitcode_ptr)->op=OP_FWD;
				(bitcode + bitcode_ptr)->arg=temp_arg;
				break;
			case '<':
				while(text[text_ptr] == '<')
				{
					text_ptr++;
					temp_arg++;
				}
				(bitcode + bitcode_ptr)->op=OP_REW;
				(bitcode + bitcode_ptr)->arg=temp_arg;
				break;
			case '[':
				push(stack, &stack_ptr, bitcode_ptr);
				(bitcode + bitcode_ptr)->op=OP_JEZ;
				text_ptr++;
				break;
			case ']':
				temp_arg=pop(stack, &stack_ptr);
				(bitcode + bitcode_ptr)->op=OP_JNZ;
				(bitcode + bitcode_ptr)->arg=temp_arg;
				(bitcode + temp_arg)->arg=bitcode_ptr;
				text_ptr++;
				break;
			case '.':
				(bitcode + bitcode_ptr)->op=OP_IO;
				(bitcode + bitcode_ptr)->arg=ARG_OUT;
				text_ptr++;
				break;
			case ',':
				(bitcode + bitcode_ptr)->op=OP_IO;
				(bitcode + bitcode_ptr)->arg=ARG_IN;
				text_ptr++;
				break;
			default:
				(bitcode + bitcode_ptr)->op=OP_NOP;
				text_ptr++;
				break;
		}
		bitcode_ptr++;
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
