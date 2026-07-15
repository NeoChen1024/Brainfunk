#include <iostream>
#include <string_view>

int main()
{
	constexpr std::string_view instructions = "+-><[].,";
	char c = 0;
	while(std::cin.get(c))
	{
		if (instructions.find(c) != std::string_view::npos)
			std::cout << c;
	}
	std::cout << '\n';
}
