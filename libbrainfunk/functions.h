#include <libbrainfunk.h>

void brainfunk_destroy(brainfunk_t *brainfunk);
brainfunk_t brainfunk_init(size_t codesize, size_t memsize, size_t stacksize, int debug);
void brainfunk_execute(brainfunk_t bf);
void bitcode_dump(brainfunk_t cpu, FILE *fp);
void bitcode_read(brainfunk_t cpu, FILE *fp);
void quit(int32_t arg);

/* These functions must be provided externally */
extern data_t io_in(void);
extern void io_out(data_t data);
