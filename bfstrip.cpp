#include <iostream>

int main(void)
{
	char c = 0;
	while(std::cin.get(c))
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
				std::cout << c;
				break;
			default:
				break;
		}
	}
	std::cout << std::endl;
}