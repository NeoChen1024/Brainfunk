#include <libbrainfunk.h>
#define DEFINE(name)			\
	.exec = &exec_ ## name,		\
	.scan = &scan_ ## name

#define EXEC(name)	int exec_ ## name(brainfunk_t cpu)
#define SCAN(name)	arg_t scan_ ## name(bitcode_t code, arg_t *pc, pcstack_t pcstack, arg_t *textptr, char *text)

#define _DEF(name)	\
	EXEC(name);	\
	SCAN(name)

_DEF(hlt);
_DEF(alu);
_DEF(alus);
_DEF(set);
_DEF(pop);
_DEF(push);
_DEF(pshi);
_DEF(mov);
_DEF(stp);
_DEF(jmp);
_DEF(jez);
_DEF(jnz);
_DEF(jsez);
_DEF(jsnz);
_DEF(io);
_DEF(frk);
_DEF(inv);
