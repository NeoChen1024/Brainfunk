#include <libbrainfunk.h>

void brainfunk_destroy(brainfunk_t *brainfunk);
brainfunk_t brainfunk_init(size_t codesize, size_t memsize, size_t stacksize, int debug);
void brainfunk_execute(brainfunk_t bf);
void bitcode_dump(brainfunk_t cpu, FILE *fp);
void bitcode_read(brainfunk_t cpu, FILE *fp);
char *brainfunk_readtext(FILE *fp, size_t size);
void brainfunk_dumptext(char *code, FILE *fp);
void bitcode_convert(brainfunk_t cpu, char *text);
void quit(int32_t arg);

pcstack_t pcstack_create(size_t size);
arg_t pcstack_pop(pcstack_t stack);
void pcstack_push(pcstack_t stack, arg_t data);
void pcstack_destroy(pcstack_t *stack);

/* These functions must be provided externally */
extern data_t io_in(void);
extern void io_out(data_t data);
