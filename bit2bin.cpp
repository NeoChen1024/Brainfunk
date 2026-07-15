#include "libbrainfunk.hpp"
#include <charconv>
#include <cctype>
#include <cstdio>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

using std::fprintf;
using std::string;

namespace {

std::optional<Opcode> parse_opcode(std::string_view name)
{
    for (std::size_t i = 0; i < static_cast<std::size_t>(Opcode::Count); ++i) {
        const auto opcode = static_cast<Opcode>(i);
        if (opcode_name(opcode) == name)
            return opcode;
    }
    return std::nullopt;
}

struct ParsedLine {
	addr_t address = 0;
	string op;
	string arg;
};

std::string_view trim(std::string_view text)
{
	while(!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
		text.remove_prefix(1);
	while(!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
		text.remove_suffix(1);
	return text;
}

bool parse_addr(std::string_view text, addr_t &value)
{
	text = trim(text);
	if(text.empty())
		return false;

	unsigned long long parsed = 0;
	auto *first = text.data();
	auto *last = text.data() + text.size();
	auto result = std::from_chars(first, last, parsed, 10);
	if(result.ec != std::errc{} || result.ptr != last)
		return false;
	if(parsed > std::numeric_limits<addr_t>::max())
		return false;

	value = static_cast<addr_t>(parsed);
	return true;
}

bool parse_byte(std::string_view text, memory_t &value)
{
	text = trim(text);
	if(text.empty())
		return false;

	unsigned int parsed = 0;
	auto *first = text.data();
	auto *last = text.data() + text.size();
	auto result = std::from_chars(first, last, parsed, 10);
	if(result.ec != std::errc{} || result.ptr != last || parsed > 0xFF)
		return false;

	value = static_cast<memory_t>(parsed);
	return true;
}

bool parse_offset(std::string_view text, offset_t &value)
{
	text = trim(text);
	if(text.empty())
		return false;

	long long parsed = 0;
	auto *first = text.data();
	auto *last = text.data() + text.size();
	auto result = std::from_chars(first, last, parsed, 10);
	if(result.ec != std::errc{} || result.ptr != last)
		return false;
	if(parsed < std::numeric_limits<offset_t>::min() ||
	   parsed > std::numeric_limits<offset_t>::max())
		return false;

	value = static_cast<offset_t>(parsed);
	return true;
}

template <unsigned Bits>
    requires (Bits > 0 && Bits < 32)
bool fits_signed_bits(offset_t value)
{
    const offset_t min = -(offset_t{1} << (Bits - 1));
    const offset_t max = (offset_t{1} << (Bits - 1)) - 1;
    return value >= min && value <= max;
}

template <unsigned Bits>
    requires (Bits > 0 && Bits < 32)
std::uint32_t encode_signed(offset_t value)
{
    const std::uint32_t mask = (std::uint32_t{1} << Bits) - 1;
	return static_cast<std::uint32_t>(value) & mask;
}

bool parse_line(std::string_view line, ParsedLine &parsed, std::size_t line_no)
{
	line = trim(line);
	if(line.empty())
		return false;

	auto colon = line.find(':');
	if(colon == std::string_view::npos) {
		fprintf(stderr, "Error: missing ':' on line %zu\n", line_no);
		return false;
	}
	if(!parse_addr(line.substr(0, colon), parsed.address)) {
		fprintf(stderr, "Error: invalid address on line %zu\n", line_no);
		return false;
	}

	auto rest = trim(line.substr(colon + 1));
	if(rest.empty()) {
		fprintf(stderr, "Error: missing opcode on line %zu\n", line_no);
		return false;
	}

	auto op_end = std::size_t{0};
	while(op_end < rest.size() &&
	      !std::isspace(static_cast<unsigned char>(rest[op_end])))
		++op_end;

	parsed.op = string(rest.substr(0, op_end));
	parsed.arg = string(trim(rest.substr(op_end)));
	return true;
}

bool emit(addr_t address, const string &op, std::string_view arg, FILE *fd)
{
	std::uint32_t inst = 0;

    const auto opcode = parse_opcode(op);
    if(!opcode) {
		fprintf(stderr, "Error: unknown opcode %s at %zu\n", op.c_str(), address);
		return false;
	}

    inst |= static_cast<std::uint32_t>(*opcode) << 20;

	// None
	if(op == "X" || op == "H")
	{
		if(!trim(arg).empty()) {
			fprintf(stderr, "Error: unexpected operand for %s at %zu\n", op.c_str(), address);
			return false;
		}
	}
	// Imm
	else if(op == "A" || op == "S" || op == "IO")
	{
		memory_t imm = 0;
		if(!parse_byte(arg, imm)) {
			fprintf(stderr, "Error: invalid immediate for %s at %zu\n", op.c_str(), address);
			return false;
		}

		inst |= imm;
	}
	// Dual
	else if(op == "MUL")
	{
		auto comma = arg.find(',');
		if(comma == std::string_view::npos) {
			fprintf(stderr, "Error: invalid dual operand for MUL at %zu\n", address);
			return false;
		}

		offset_t offset = 0;
		memory_t mul = 0;
		if(!parse_offset(arg.substr(0, comma), offset) ||
		   !parse_byte(arg.substr(comma + 1), mul)) {
			fprintf(stderr, "Error: invalid dual operand for MUL at %zu\n", address);
			return false;
		}

		inst |= mul;
        if(!fits_signed_bits<12>(offset)) {
			fprintf(stderr, "Error: offset %zd at %zu does not fit in 12 bits\n",
			        offset, address);
			return false;
		}
        inst |= encode_signed<12>(offset) << 8;
	}
	// Offset
	else if(op == "F" || op == "M" || op == "JE" || op == "JN")
	{
		offset_t offset = 0;
		if(!parse_offset(arg, offset)) {
			fprintf(stderr, "Error: invalid offset for %s at %zu\n", op.c_str(), address);
			return false;
		}

        if(!fits_signed_bits<20>(offset)) {
			fprintf(stderr, "Error: offset %zd at %zu does not fit in 20 bits\n",
			        offset, address);
			return false;
		}
        inst |= encode_signed<20>(offset);
	}

	fprintf(fd, "%06x\n", inst);
	return true;
}

} // namespace

int main()
{
	size_t inst_ctr = 0;
	int exit_status = 0;
	std::string line;
	std::size_t line_no = 0;

	while(std::getline(std::cin, line))
	{
		++line_no;
		if(trim(line).empty())
			continue;

		ParsedLine parsed;
		if(!parse_line(line, parsed, line_no)) {
			exit_status = 1;
			continue;
		}
		if(emit(parsed.address, parsed.op, parsed.arg, stdout)) {
			inst_ctr++;
		} else {
			exit_status = 1;
		}
	}

	fprintf(stderr, "Assembled %zu instruction(s)\n", inst_ctr);
	return exit_status;
}
