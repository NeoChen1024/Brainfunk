#include <libbrainfunk.h>

#define _DEF(name)								\
	int exec_ ## name(brainfunk_t cpu);					\
	size_t scan_ ## name(bitcode_t code, char *text)

_DEF(hlt);
