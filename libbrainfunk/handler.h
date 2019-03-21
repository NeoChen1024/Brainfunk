#include <libbrainfunk.h>

#define _DEF(name)								\
	int exec_ ## name(brainfunk_t cpu);					\
	arg_t scan_ ## name(bitcode_t code, arg_t pc, arg_t *textptr, char *text)

_DEF(hlt);
_DEF(io);
_DEF(alu);
_DEF(mov);
_DEF(jez);
_DEF(jnz);
_DEF(inv);
_DEF(set);
