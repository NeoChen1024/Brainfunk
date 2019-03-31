#include <libbrainfunk.h>

void bitcode_convert(brainfunk_t cpu, char *text)
{
	arg_t textptr=0;
	arg_t pc=0;
	arg_t ret=1;
	int try=0;

	pcstack_t pcstack = pcstack_create(PCSTACK_SIZE);

	while(text[textptr] != '\0')
	{
		try=0;
		while(try < OP_INSTS)
		{
			ret = handler[try++].scan(
				cpu->code, pc, pcstack, &textptr, text);
			if(ret != LEXERR)
			{
				pc += ret;
				break;
			}
		}
	}

	cpu->codelen = pc;
	pcstack_destroy(&pcstack);
}
