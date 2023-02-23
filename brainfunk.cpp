#include "libbrainfunk.hpp"
#include <getopt.h>

using std::fstream;
using std::cin;
using std::cout;
using std::string;
using std::endl;
using std::cerr;

/* Read code and filter out unnecessary characters */
void readcode(string &code, string filename)
{
	fstream input;
	input.open(filename);
	if (!input.is_open())
	{
		perror(filename.c_str());
		exit(1);
	}

	char c;
	while(input.get(c))
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
				code += c;
				break;
			default:
				break;
		}
	}
	input.close();
}

void helpmsg(int argc, char **argv)
{
	cerr << "Usage: " << argv[0] << " [-h] [-m mode] [-s code string] [-f code] [-o out]" << endl;
}

int main(int argc, char **argv)
{
	string code;
	string mode = "bf";
	ostream *output = &cout;

	bool valid = false;
	int opt;
	while((opt = getopt(argc, argv, "hm:s:f:o:")) != -1)
	{
		switch(opt)
		{
			case 'f':
				readcode(code, optarg);
				valid = true;
				break;
			case 's':
				code = optarg;
				valid = true;
				break;
			case 'h':
				helpmsg(argc, argv);
				break;
			case 'm':
				mode = optarg;
				break;
			case 'o': // Output file
				try
				{
					output = new fstream(optarg, fstream::out);
				}
				catch(const std::exception& e)
				{
					std::cerr << e.what() << '\n';
				}
				break;
			default:
				break;
		}
	}

	if(!valid)
	{
		helpmsg(argc, argv);
		return 1;
	}

	class Brainfunk bf(MEMSIZE);

	if(mode == "xx")
	{
		return 0;
	}
	bf.translate(code);

	if(mode == "bf")
	{
		bf.run();
	}
	else if(mode == "bit")
	{
		bf.dump(*output, FMT_BIT);
	}
	else if(mode == "bfc")
	{
		bf.dump(*output, FMT_C);
	}
	else
	{
		cerr << "Unknown mode: " << mode << endl;
		return 1;
	}
}
