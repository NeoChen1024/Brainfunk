/* ====================================== *\
|* bf.c -- A simple Brainfuck Interpreter *|
|* Neo_Chen				  *|
\* ====================================== */

#include <stdint.h>
#include <unistd.h>
#include <getopt.h>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <cstring>

using std::vector;
using std::string;

#define STACKSIZE (1ULL<<12)
#define MEMSIZE (1ULL<<20)
#define CODESIZE (1ULL<<16)

#define _INLINE	static inline

#define ERR_MSG_ALLOC	("Unable to allocate memory!\n")

typedef uint8_t memory_t;
typedef unsigned int arg_t;

/* init */

static vector<memory_t> memory;
static arg_t ptr=0;
static vector<string::iterator> stack;
static string code;

_INLINE bool is_code(char c)
{
	switch(c)
	{
		case '+':
		case '-':
		case '>':
		case '<':
		case '[':
		case ']':
		case '.':
		case ',':
			return true;
		default:
			return false;
	}
}

void panic(string msg, int condition)
{
	if(condition)
	{
		fputs(msg.c_str(), stderr);
		exit(EXIT_FAILURE);
	}
}

static void validate_code(string &code)
{
	ssize_t level = 0;

	/* Validate that the '[' and ']'s is matched */
	for(auto &c : code)
	{
		if(c == '[')
			++level;
		else if(c == ']')
			--level;
	}
	
	panic("Unmatched Loop!\n", level != 0);
}


// Read Code
static void read_code(FILE* fp)
{
	int c;
	code = "";
	while((c = getc(fp)) != EOF)
	{
		if(is_code(c))
			code += c;
	}
	code += '\0';

	validate_code(code);
}

void interprete(string &code)
{
	string::iterator c = code.begin();
	std::fill(memory.begin(), memory.end(), 0);

	while(c != code.end())
	{
	switch(*c)
	{
		case '+':
			memory[ptr]++;
			break;
		case '-':
			memory[ptr]--;
			break;
		case '>':
			ptr++;
			ptr %= MEMSIZE;
			break;
		case '<':
			ptr--;
			ptr %= MEMSIZE;
			break;
		case '[':
			if(memory[ptr] == 0)
			{
				ssize_t level = 1;
				while(level != 0)
				{
					c++;
					if(*c == '[')
						level++;
					else if(*c == ']')
						level--;
				}
			}
			else
				stack.emplace_back(c); /* Push PC */
			break;
		case ']':
			if(memory[ptr] != 0) /* if not equals to 0 */
				c = stack.back(); /* Peek */
			else
				stack.pop_back(); /* Drop */
			break;
		case ',':
			memory[ptr] = (uint8_t)getc(stdin);
			break;
		case '.':
			putc(memory.at(ptr), stdout);
			break;
		default:
			break;
	}
	c++; // next instruction
	}
}

static void help(int argc, char **argv)
{
	printf("Usage: %s [-h] [-f file] [-s code]\n", argv[0]);
}

int main(int argc, char **argv)
{
	/* Init */
	memory.resize(MEMSIZE);
	stack.resize(STACKSIZE);
	code.resize(CODESIZE);

	/* Disable Buffering */
	setvbuf(stdin, NULL, _IONBF, 0);
	setvbuf(stdout, NULL, _IONBF, 0);

	/* Parse Argument */
	FILE *codefile;
	int opt;

	if(!(argc >= 2))
	{
		help(argc, argv);
	}
	else
	{
		while((opt = getopt(argc, argv, "hf:s:")) != -1)
		{
			switch(opt)
			{
				case 'f':
					if(string(optarg) == "-")
						read_code(stdin);
					else
					{
						if((codefile = fopen(optarg, "r")) == NULL)
						{
							perror(optarg);
							exit(EXIT_FAILURE);
						}
						read_code(codefile);
					}
					break;
				case 's':
					code = string(optarg);
					validate_code(code);
					break;
				case 'h':
					help(argc, argv);
					break;
				default:
					exit(EXIT_FAILURE);
			}
		}
	}

	interprete(code);
	return 0;
}
