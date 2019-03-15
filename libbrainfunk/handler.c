#include <libbrainfunk.h>

#define DEFINE(name)			\
	.exec = exec_ ## name,		\
	.scan = scan_ ## name

#define EXEC(name)	int exec_ ## name(brainfunk_t cpu)
#define SCAN(name)	size_t scan_ ## name(bitcode_t code, char *text)

handler_t handler[OP_INSTS] =
{
	[OP_HLT] =
	{
		DEFINE(hlt)
	}
};

EXEC(hlt)
{
	quit(cpu->code[cpu->pc].arg);
	return HALT;
}

SCAN(hlt)
{
	if(*text == '_')
		code->op = OP_HLT;
	return 1;
}
