/* ==================================== *
 * Brainfunk -- A Brainf**k Toolkit     *
 * Neo_Chen			        *
 * ==================================== */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#define DEF_MEMSIZE 262144
#define DEF_CODESIZE 65536
#define DEF_STACKSIZE 1024
#define DEF_BITCODESIZE 65536
#define DEF_PSTACKSIZE	65536

#define TRUE 1
#define FALSE 0

#define OP_NOP	0x00
#define OP_ADD	0x01
#define OP_SUB	0x02
#define OP_FWD	0x03
#define OP_REW	0x04
#define OP_JEZ	0x05
#define OP_JNZ	0x06
#define OP_IO	0x07
#define OP_SET	0x08
#define OP_POP	0x09
#define OP_PUSH	0x0A
#define OP_PSHI	0x0B
#define OP_ADDS	0x0C
#define OP_SUBS	0x0D
#define OP_JMP	0x0E
#define OP_JSEZ	0x0F
#define OP_JSNZ	0x10
#define OP_FRK	0x11
#define OP_HCF	0x12
#define OP_HLT	0x13

#define ARG_IN	0
#define ARG_OUT	1
#define ARG_INS	2
#define ARG_OUTS	3

struct bitcode_struct
{
	unsigned char op;
	unsigned int arg;
};

typedef struct bitcode_struct bitcode_t;
typedef uint8_t memory_t;
typedef unsigned int stack_type;
typedef char code_t;

void panic(char *msg);
void read_code(FILE* fp);
void push(stack_type *stack, unsigned int *ptr, stack_type content);
stack_type pop(stack_type *stack, unsigned int *ptr);
void debug_output(void);
void jump_to_next_matching(void);
void interprete(code_t c);
void output(memory_t c);
memory_t input(void);
void debug_loop(char *fmt, unsigned int location);
int is_code(char c);

#ifdef VISUAL
void print_stack(stack_type *target, unsigned int pointer);
#endif

void bitcodelize(bitcode_t *bitcode, size_t bitcodesize, code_t *text);
void bitcode_interprete(bitcode_t *bitcode);
void bitcode_disassembly(bitcode_t *bitcode, unsigned int address, char *str, size_t strsize);
void bitcode_disassembly_array_to_fp(bitcode_t *bitcode, FILE *fp);
void bitcode_assembly(char *str, bitcode_t *bitcode);
void bitcode_load_fp(bitcode_t *bitcode, FILE *fp);
