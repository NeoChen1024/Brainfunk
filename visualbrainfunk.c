/* ==================================== *
 * Brainfunk -- A Brainf**k Interpreter *
 * Neo_Chen			        *
 * ==================================== */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <ncurses.h>
#include <libbrainfunk.h>

/* init */

memory_t *memory;
unsigned int ptr=0;
stack_type *stack;
unsigned int stack_ptr=0;
code_t *code;
unsigned int code_ptr=0;

int debug=0;

unsigned int memsize=DEF_MEMSIZE;
unsigned int codesize=DEF_CODESIZE;
unsigned int stacksize=DEF_STACKSIZE;

#define IO_WINDOW io_win
#define CODE_WINDOW code_win
#define MEM_WINDOW mem_win
#define REG_WINDOW reg_win
#define STACK_WINDOW stack_win
#define WBORDER_DRAW(name) \
	wborder(name, '|', '|', '-', '-', '+', '+', '+', '+')

#define MSG_COLOR 1

WINDOW *io_win;
WINDOW *code_win;
WINDOW *mem_win;
WINDOW *reg_win;
WINDOW *stack_win;

void debug_output(void)
{
	wprintw(IO_WINDOW, "code=%u:%c\n", code_ptr, code[code_ptr]);
	wprintw(IO_WINDOW, "stack=%u:0x%0x\n", stack_ptr, stack[stack_ptr]);
	wprintw(IO_WINDOW, "ptr=%0x:0x%0x\n", ptr, memory[ptr]);
	wprintw(IO_WINDOW, "--------\n");
	wrefresh(IO_WINDOW);
}

void wait_input(char *msg)
{
	wattron(IO_WINDOW, COLOR_PAIR(MSG_COLOR) | A_BOLD);
	wprintw(IO_WINDOW, msg);
	wattroff(IO_WINDOW, COLOR_PAIR(MSG_COLOR) | A_BOLD);
	wrefresh(IO_WINDOW);
	wgetch(IO_WINDOW);
}

void panic(char *msg)
{
	wait_input(msg);
	wrefresh(IO_WINDOW);
	endwin();
	exit(2);
}

void debug_loop(char *fmt, unsigned int location)
{
	wprintw(IO_WINDOW, fmt, location);
	wrefresh(IO_WINDOW);
}

void output(memory_t c)
{
	waddch(IO_WINDOW, c);
	wrefresh(IO_WINDOW);
}

memory_t input(void)
{
	return (memory_t)wgetch(IO_WINDOW);
	wrefresh(IO_WINDOW);
}

void clean_up(void)
{
	endwin();
	free(memory);
	free(code);
	free(stack);
}

void parse_argument(int argc, char **argv)
{
	FILE *corefile;
	int opt;

	if(!(argc >= 2))
	{
		puts("argc < 2!");
	}
	else
	{
		while((opt = getopt(argc, argv, "hdf:c:s:")) != -1)
		{
			switch(opt)
			{
				case 's': /* Read size from argument */
					sscanf(optarg, "%u,%u,%u", &memsize, &codesize, &stacksize);
					if(memsize == 0 || codesize == 0 || stacksize == 0)
						panic("?SIZE=0");
					free(memory);
					free(code);
					free(stack);
					memory	= calloc(memsize, sizeof(memory_t));
					code	= calloc(codesize, sizeof(code_t));
					stack	= calloc(stacksize, sizeof(stack_type));
					break;
				case 'f': /* File */
					if(strcmp(optarg, "-"))
					{
						if((corefile = fopen(optarg, "r")) == NULL)
						{
							perror(optarg);
							exit(8);
						}
						read_code(corefile);
						fclose(corefile);
					}
					else
						read_code(stdin);
					break;
				case 'c': /* Code */
					strncpy(code, optarg, CODESIZE);
					break;
				case 'h': /* Help */
					printf("Usage: %s [-h] [-f file] [-c code] [-s memsize,codesize,stacksize] [-d]\n", argv[0]);
					break;
				case 'd': /* Debug */
					wprintw(IO_WINDOW, "DEBUG=1");
					debug = TRUE;
					break;
				default:
					exit(1);

			}
		}
	}

}

int main(int argc, char **argv)
{
	/* Init */
	memory	= calloc(MEMSIZE, sizeof(memory_t));
	code	= calloc(CODESIZE, sizeof(code_t));
	stack	= calloc(STACKSIZE, sizeof(stack_type));

	/* Parse Argument */
	initscr();
	start_color();

	init_pair(MSG_COLOR, COLOR_YELLOW, COLOR_BLUE);

/* Window Layout
 *
 * 0		49	79
 * +-------------+-------+ 0
 * |         MEM         |
 * +-------------+-------+ 4
 * |             |       |
 * |    CODE     |  REG  |
 * |             |       |
 * |-------------+-------+ 10
 * |             |       |
 * |     IO      | STACK |
 * |             |       |
 * +-------------+-------+ 23
 */

	MEM_WINDOW	= newwin(4, 80, 0, 0);
	CODE_WINDOW	= newwin(6, 50, 4, 0);
	IO_WINDOW	= newwin(13, 50, 10, 0);
	REG_WINDOW	= newwin(6, 30, 4, 50);
	STACK_WINDOW	= newwin(13, 30, 10, 50);

	WBORDER_DRAW(MEM_WINDOW);
	WBORDER_DRAW(CODE_WINDOW);
	WBORDER_DRAW(IO_WINDOW);
	WBORDER_DRAW(REG_WINDOW);
	WBORDER_DRAW(STACK_WINDOW);

	parse_argument(argc, argv);
	wprintw(IO_WINDOW, "memory	= %p[%d]\n", memory, MEMSIZE);
	wprintw(IO_WINDOW, "code	= %p[%d]\n", code, CODESIZE);
	wprintw(IO_WINDOW, "stack	= %p[%d]\n\n", stack, STACKSIZE);

	wrefresh(MEM_WINDOW);
	wrefresh(CODE_WINDOW);
	wrefresh(REG_WINDOW);
	wrefresh(IO_WINDOW);
	wrefresh(STACK_WINDOW);

	wait_input("?START");

	scrollok(IO_WINDOW, TRUE);

	while(code[code_ptr] != '\0')
	{
		if(debug)
			debug_output();
		interprete(code[code_ptr]);
	}

	wait_input("?HALT");
	clean_up();
	return 0;
}
