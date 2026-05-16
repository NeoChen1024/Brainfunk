#include "libbrainfunk.hpp"
#include <charconv>
#include <cctype>
#include <cstdio>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <string_view>

using std::fprintf;
using std::string;

const std::map<string, memory_t> opcodes = {
	{"X", 0x00},
	{"A", 0x01},
	{"S", 0x02},
	{"MUL", 0x03},
	{"F", 0x04},
	{"M", 0x05},
	{"JE", 0x06},
	{"JN", 0x07},
	{"IO", 0x08},
	{"H", 0x09}
};

namespace {

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

bool fits_signed_bits(offset_t value, int bits)
{
	const offset_t min = -(offset_t{1} << (bits - 1));
	const offset_t max = (offset_t{1} << (bits - 1)) - 1;
	return value >= min && value <= max;
}

std::uint32_t encode_signed(offset_t value, int bits)
{
	const std::uint32_t mask = (std::uint32_t{1} << bits) - 1;
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

	auto opcode = opcodes.find(op);
	if(opcode == opcodes.end()) {
		fprintf(stderr, "Error: unknown opcode %s at %zu\n", op.c_str(), address);
		return false;
	}

	inst |= static_cast<std::uint32_t>(opcode->second) << 20;

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
		if(!fits_signed_bits(offset, 12)) {
			fprintf(stderr, "Warning: offset %zd at %zu is too large\n", offset, address);
		}
		inst |= encode_signed(offset, 12) << 8;
	}
	// Offset
	else if(op == "F" || op == "M" || op == "JE" || op == "JN")
	{
		offset_t offset = 0;
		if(!parse_offset(arg, offset)) {
			fprintf(stderr, "Error: invalid offset for %s at %zu\n", op.c_str(), address);
			return false;
		}

		if(!fits_signed_bits(offset, 20)) {
			fprintf(stderr, "Warning: offset %zd at %zu is too large\n", offset, address);
		}
		inst |= encode_signed(offset, 20);
	}

	fprintf(fd, "%06x\n", inst);
	return true;
}

} // namespace

int main(int argc, char **argv)
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
