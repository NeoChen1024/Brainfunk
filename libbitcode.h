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

#define OP_NOP	0x00
#define OP_ADD	0x01
#define OP_SUB	0x02
#define OP_FWD	0x03
#define OP_REW	0x04
#define OP_JEZ	0x05
#define OP_JNZ	0x06
#define OP_IO	0x07
#define OP_HLT	0x08

#define ARG_IN	1
#define ARG_OUT	0

struct bitcode_struct
{
	unsigned char op;
	unsigned int arg;
};

typedef struct bitcode_struct bitcode_t;

#define DEF_BITCODESIZE 65536

void bitcodelize(bitcode_t *bitcode, code_t *text);
void bitcode_interprete(bitcode_t *bitcode);
void bitcode_disassembly(bitcode_t *bitcode, unsigned int address, char *str, size_t strsize);
void bitcode_disassembly_array_to_fp(bitcode_t *bitcode, FILE *fp);
void bitcode_assembly(char *str, bitcode_t *bitcode);
void bitcode_load_fp(bitcode_t *bitcode, FILE *fp);
