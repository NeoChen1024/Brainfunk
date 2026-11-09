/* ====================================== *\
|* bf.c -- A simple Brainfuck Interpreter *|
|* Neo_Chen				  *|
\* ====================================== */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <getopt.h>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

using std::vector;
using std::string;

inline constexpr std::size_t STACKSIZE = 1ULL << 12;
inline constexpr std::size_t MEMSIZE = 1ULL << 20;
inline constexpr std::size_t CODESIZE = 1ULL << 16;

using memory_t = std::uint8_t;
using arg_t = std::size_t;

/* init */

static vector<memory_t> memory;
static arg_t ptr=0;
static vector<string::const_iterator> stack;
static string code;

[[nodiscard]] constexpr bool is_code(char c) noexcept
{
	constexpr std::string_view instructions = "+-><[].,";
	return instructions.find(c) != std::string_view::npos;
}

void panic(std::string_view msg, bool condition)
{
	if(condition)
	{
		fwrite(msg.data(), 1, msg.size(), stderr);
		exit(EXIT_FAILURE);
	}
}

static void validate_code(std::string_view code)
{
	std::ptrdiff_t level = 0;

	/* Validate that the '[' and ']'s is matched */
	for(auto &c : code)
	{
		if(c == '[')
			++level;
		else if(c == ']')
		{
			--level;
			panic("Unmatched Loop!\n", level < 0);
		}
	}

	panic("Unmatched Loop!\n", level != 0);
}


// Read Code
static void read_code(FILE* fp)
{
	int c;
	code.clear();
	while((c = getc(fp)) != EOF)
	{
		if(is_code(static_cast<char>(c)))
			code += static_cast<char>(c);
	}

	validate_code(code);
}

void interprete(const string &code)
{
	auto c = code.cbegin();
	std::ranges::fill(memory, memory_t{0});
	ptr = 0;
	stack.clear();

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
					std::ptrdiff_t level = 1;
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
			{
				int input = getc(stdin);
				memory[ptr] = input == EOF ? 0 : static_cast<uint8_t>(input);
				break;
			}
		case '.':
			putc(memory.at(ptr), stdout);
			break;
		default:
			break;
	}
	c++; // next instruction
	}
}

[[noreturn]] static void help(char **argv, int status = 0)
{
	printf("Usage: %s [-h] [-f file] [-s code]\n", argv[0]);
	exit(status);
}

int main(int argc, char **argv)
{
	/* Init */
	memory.resize(MEMSIZE);
	stack.reserve(STACKSIZE);
	code.reserve(CODESIZE);

	/* Disable Buffering */
	setvbuf(stdin, NULL, _IONBF, 0);
	setvbuf(stdout, NULL, _IONBF, 0);

	/* Parse Argument */
	FILE *codefile;
	int opt;

	if(!(argc >= 2))
	{
		help(argv, 1);
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
							codefile = fopen(optarg, "r");
							if(codefile == nullptr)
						{
							perror(optarg);
							exit(EXIT_FAILURE);
						}
							read_code(codefile);
							fclose(codefile);
					}
					break;
				case 's':
					code = string(optarg);
					validate_code(code);
					break;
				case 'h':
					help(argv);
					break;
				default:
					exit(EXIT_FAILURE);
			}
		}
	}

	interprete(code);
	return 0;
}
