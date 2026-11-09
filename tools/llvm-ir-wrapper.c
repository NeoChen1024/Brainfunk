#include <stdint.h>
#include <stdio.h>

#define BRAINFUNK_TAPE_SIZE 65536u

typedef int32_t (*brainfunk_read_fn)(void *context);
typedef void (*brainfunk_write_fn)(void *context, uint32_t byte);

extern int32_t brainfunk_program(uint8_t *tape,
                                 uint64_t tape_size,
                                 void *io_context,
                                 brainfunk_read_fn read_byte,
                                 brainfunk_write_fn write_byte);

static int32_t read_byte(void *context)
{
    (void)context;
    return getchar();
}

static void write_byte(void *context, uint32_t byte)
{
    (void)context;
    putchar((unsigned char)byte);
    fflush(stdout);
}

int main(void)
{
    static uint8_t tape[BRAINFUNK_TAPE_SIZE];

    return brainfunk_program(tape,
                             BRAINFUNK_TAPE_SIZE,
                             NULL,
                             read_byte,
                             write_byte);
}
