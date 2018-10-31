/* ==================================== *
 * Brainfunk -- A Brainf**k Interpreter *
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

#define TRUE 1
#define FALSE 0

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
