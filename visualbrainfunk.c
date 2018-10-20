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
#include <getopt.h>
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
int delay=1000*100; /* Delay 100ms */

unsigned int memsize=DEF_MEMSIZE;
unsigned int codesize=DEF_CODESIZE;
unsigned int stacksize=DEF_STACKSIZE;

#define IO_WINDOW io_win
#define CODE_WINDOW code_win
#define MEM_WINDOW mem_win
#define REG_WINDOW reg_win
#define STACK_WINDOW stack_win

#define MSG_COLOR 1
#define MEM_COLOR 2
#define CODE_COLOR 3
#define REG_COLOR 4
#define IO_COLOR 5
#define STACK_COLOR 6

#define CODE_WINDOW_WIDTH 60

WINDOW *io_win;
WINDOW *code_win;
WINDOW *mem_win;
WINDOW *reg_win;
WINDOW *stack_win;

void wait_input(char *msg)
{
	wattron(IO_WINDOW, COLOR_PAIR(MSG_COLOR) | A_BOLD);
	wprintw(IO_WINDOW, "%s", msg);
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
		puts("?ARG");
	}
	else
	{
		while((opt = getopt(argc, argv, "hdf:c:s:t:")) != -1)
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
					if((corefile = fopen(optarg, "r")) == NULL)
					{
						perror(optarg);
						exit(8);
					}
					read_code(corefile);
					fclose(corefile);
					break;
				case 'c': /* Code */
					strncpy(code, optarg, CODESIZE);
					break;
				case 't': /* Delay (in msec) */
					sscanf(optarg, "%d", &delay);
					delay *= 1000;
					break;
				case 'h': /* Help */
					printf("Usage: %s [-h] [-f file] [-c code] [-s memsize,codesize,stacksize] [-d] [-t msec]\n", argv[0]);
					break;
				case 'd': /* Debug */
					wprintw(IO_WINDOW, "DEBUG=1");
					wrefresh(IO_WINDOW);
					debug = TRUE;
					break;
				default:
					exit(1);

			}
		}
	}

}

void print_stack(stack_type *target, unsigned int pointer)
{
	unsigned int count=0;
	wclear(STACK_WINDOW);
	for(count=0; count <= pointer; count++)
	{
		if((pointer - count) <= 14)
			wprintw(STACK_WINDOW, "stack[%4u] == %u\n", count, target[count]);
	}
	wrefresh(STACK_WINDOW);
}

void print_reg(void)
{
	wclear(REG_WINDOW);
	wprintw(REG_WINDOW, "PTR       == %u\n", ptr);
	wprintw(REG_WINDOW, "CODE_PTR  == %u\n", code_ptr);
	if(is_code(code[code_ptr]))
		wprintw(REG_WINDOW, "CODE      == \'%c\'\n", code[code_ptr]);
	else
		wprintw(REG_WINDOW, "CODE      == \' \'\n");
	wprintw(REG_WINDOW, "STACK_PTR == %u\n", stack_ptr);
	wrefresh(REG_WINDOW);
}

void print_mem(void)
{
	int count;
	wclear(MEM_WINDOW);
	for(count=-8; count <= 7 ; count++)
	{
		if(((signed)ptr + count) < 0)
			waddstr(MEM_WINDOW, "    |");
		else if((ptr + count) >= MEMSIZE)
			waddstr(MEM_WINDOW, "    |");
		else
			wprintw(MEM_WINDOW, " %02x |", memory[ptr+count]);
	}
	for(count=0; count <= 7; count++)
	{
		waddstr(MEM_WINDOW, "     ");
	}
	wprintw(MEM_WINDOW, " ^^ PTR=%u", ptr);
	wrefresh(MEM_WINDOW);
}

void calc_line_col(char *target, unsigned int pointer, unsigned int *line, unsigned int *col)
{
	unsigned int count=0;
	(*line)=0;
	(*col)=0;
	while(count != pointer)
	{
		if(target[count] == '\n')
		{
			(*line)++;
			(*col)=0;
		}
		else
		{
			(*col)++;
			if(*col >= CODE_WINDOW_WIDTH)
			{
				(*col) %= CODE_WINDOW_WIDTH;
				(*line)++;
			}
		}
		count++;
	}
	return;
}

void print_skipline(char *str, int line, unsigned int *y)
{
	int temp=0;
	int col=0;
	int row=0;

	if(line <= 5)
		waddnstr(CODE_WINDOW, code, CODESIZE);
	else
		while(str[temp] != '\0')
		{
			if(str[temp] == '\n')
			{
				row++;
				col=0;
			}
			else
				col++;
			if(col >= CODE_WINDOW_WIDTH)
			{
				row++;
				col %= CODE_WINDOW_WIDTH;
			}
			if(row == line)
			{
				(*y) -= row - 1;
				waddstr(CODE_WINDOW, code + temp);
				return;
			}
			temp++;
		}
	return;
}

void print_code(void)
{
	unsigned int x=0, y=0;
	wclear(CODE_WINDOW);
	calc_line_col(code, code_ptr, &y, &x);
	print_skipline(code, y, &y);
	mvwchgat(CODE_WINDOW, y, x, 1, A_REVERSE, 0, NULL);
	wrefresh(CODE_WINDOW);
}

int main(int argc, char **argv)
{
	memory	= calloc(MEMSIZE, sizeof(memory_t));
	code	= calloc(CODESIZE, sizeof(code_t));
	stack	= calloc(STACKSIZE, sizeof(stack_type));

	initscr();

	if (! has_colors())
	{
		endwin();
		puts("?COLOR");
		exit(1);
	}

	start_color();

/* Window Layout
 *
 * 0		59	79
 * +-------------+-------+ 0
 * |         MEM         |	(BLACK on WHITE)
 * +-------------+-------+ 4
 * |             |       |
 * |    CODE     |  REG  |	(BG == YELLOW) : (BG == GREEN)
 * |             |       |
 * |-------------+-------+ 9
 * |             |       |
 * |     IO      | STACK |	(YELLOW / BLUE) : (BLACK / CYAN)
 * |             |       |
 * +-------------+-------+ 23
 */

	MEM_WINDOW	= newwin(4, 80, 0, 0);
	CODE_WINDOW	= newwin(6, 60, 4, 0);
	IO_WINDOW	= newwin(13, 60, 10, 0);
	REG_WINDOW	= newwin(6, 20, 4, 60);
	STACK_WINDOW	= newwin(13, 20, 10, 60);

	init_pair(MSG_COLOR, COLOR_BLACK, COLOR_WHITE);
	init_pair(MEM_COLOR, COLOR_BLACK, COLOR_WHITE);
	init_pair(CODE_COLOR, COLOR_BLACK, COLOR_YELLOW);
	init_pair(REG_COLOR, COLOR_BLACK, COLOR_GREEN);
	init_pair(IO_COLOR, COLOR_WHITE, COLOR_BLUE);
	init_pair(STACK_COLOR, COLOR_BLACK, COLOR_CYAN);

	parse_argument(argc, argv);
	wprintw(IO_WINDOW, "memory	= %p[%d]\n", memory, MEMSIZE);
	wprintw(IO_WINDOW, "code	= %p[%d]\n", code, CODESIZE);
	wprintw(IO_WINDOW, "stack	= %p[%d]\n", stack, STACKSIZE);

	wbkgd(MEM_WINDOW, COLOR_PAIR(MEM_COLOR));
	wbkgd(CODE_WINDOW, COLOR_PAIR(CODE_COLOR));
	wbkgd(REG_WINDOW, COLOR_PAIR(REG_COLOR));
	wbkgd(IO_WINDOW, COLOR_PAIR(IO_COLOR));
	wbkgd(STACK_WINDOW, COLOR_PAIR(STACK_COLOR));

	wrefresh(MEM_WINDOW);
	wrefresh(CODE_WINDOW);
	wrefresh(REG_WINDOW);
	wrefresh(IO_WINDOW);
	wrefresh(STACK_WINDOW);

	wait_input("?START");
	wclear(IO_WINDOW);
	wrefresh(IO_WINDOW);

	scrollok(IO_WINDOW, TRUE);

	while(code[code_ptr] != '\0')
	{
		if(is_code(code[code_ptr]))
		{
			print_reg();
			print_mem();
			print_code();
			if(delay)
				usleep(delay);
			interprete(code[code_ptr]);
		}
		else
			code_ptr++;
	}

	wait_input("?HALT");
	clean_up();
	return 0;
}
