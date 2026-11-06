/* bit2bin -- assemble bitcode text into 32-bit bincode, one hex word per line.
 * doc/bincode.txt is the normative description of the output. */
#include "ctre.hpp"
#include "libbrainfunk.hpp"

#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace {

constexpr unsigned OPCODE_SHIFT = 28;
constexpr unsigned NARROW_SHIFT = 20;
constexpr unsigned ADDR_SHIFT   = 8;

std::optional<Opcode> parse_opcode(std::string_view name)
{
	for (std::size_t i = 0; i < static_cast<std::size_t>(Opcode::Count); ++i) {
		const auto opcode = static_cast<Opcode>(i);
		if (opcode_name(opcode) == name)
			return opcode;
	}
	return std::nullopt;
}

std::optional<std::int64_t> parse_number(std::string_view text)
{
	std::int64_t value = 0;
	const auto *last = text.data() + text.size();
	const auto result = std::from_chars(text.data(), last, value, 10);
	if (result.ec != std::errc{} || result.ptr != last)
		return std::nullopt;
	return value;
}

/* Splits "12, -3, 255" into at most three decimal operands. */
template <std::size_t Count>
std::optional<std::array<std::int64_t, Count>> parse_operands(std::string_view text)
{
	std::array<std::string_view, 3> fields{};
	std::size_t found = 0;

	if (Count == 0 && ctre::match<"">(text)) {
		found = 0;
	} else if (auto one = ctre::match<"(-?[0-9]++)">(text)) {
		fields[0] = one.get<1>().to_view();
		found = 1;
	} else if (auto two = ctre::match<"(-?[0-9]++)\\s*+,\\s*+(-?[0-9]++)">(text)) {
		fields[0] = two.get<1>().to_view();
		fields[1] = two.get<2>().to_view();
		found = 2;
	} else if (auto three = ctre::match<"(-?[0-9]++)\\s*+,\\s*+(-?[0-9]++)\\s*+,\\s*+(-?[0-9]++)">(text)) {
		fields[0] = three.get<1>().to_view();
		fields[1] = three.get<2>().to_view();
		fields[2] = three.get<3>().to_view();
		found = 3;
	} else {
		return std::nullopt;
	}

	if (found != Count)
		return std::nullopt;

	std::array<std::int64_t, Count> values{};
	for (std::size_t i = 0; i < Count; ++i) {
		const auto value = parse_number(fields[i]);
		if (!value)
			return std::nullopt;
		values[i] = *value;
	}
	return values;
}

/* Two's-complement field of `bits` bits, left unshifted. */
std::uint32_t signed_field(std::int64_t value, unsigned bits)
{
	return static_cast<std::uint32_t>(value) & ((std::uint32_t{1} << bits) - 1);
}

bool fits_byte(std::int64_t value)
{
	return value >= 0 && value <= 0xFF;
}

bool report_range(std::string_view name, std::int64_t value, unsigned bits, addr_t address)
{
	std::fprintf(stderr, "Error: %.*s %lld at %zu does not fit in %u bits\n",
	             static_cast<int>(name.size()), name.data(),
	             static_cast<long long>(value), address, bits);
	return false;
}

bool check_signed(std::string_view name, std::int64_t value, unsigned bits, addr_t address)
{
	return fits_signed(value, bits) || report_range(name, value, bits, address);
}

bool check_byte(std::string_view name, std::int64_t value, addr_t address)
{
	return fits_byte(value) || report_range(name, value, 8, address);
}

bool report_operands(Opcode opcode, addr_t address)
{
	const auto name = opcode_name(opcode);
	std::fprintf(stderr, "Error: invalid operands for %.*s at %zu\n",
	             static_cast<int>(name.size()), name.data(), address);
	return false;
}

/* Encodes one instruction.  Bits a layout leaves reserved stay zero. */
std::optional<std::uint32_t> encode(Opcode opcode, std::string_view operands, addr_t address)
{
	std::uint32_t word = static_cast<std::uint32_t>(opcode) << OPCODE_SHIFT;

	switch (opcode_layout(opcode)) {
	case OperandLayout::None:
		if (!parse_operands<0>(operands)) {
			report_operands(opcode, address);
			return std::nullopt;
		}
		return word;

	case OperandLayout::Data: {
		const auto values = parse_operands<2>(operands);
		if (!values) {
			report_operands(opcode, address);
			return std::nullopt;
		}
		const auto [addr_offset, data] = *values;
		if (!check_signed("addr_offset", addr_offset, ADDR_OFFSET_BITS, address) ||
		    !check_byte("data", data, address))
			return std::nullopt;
		if (opcode == Opcode::IO && data != IO_IN && data != IO_OUT) {
			report_operands(opcode, address);
			return std::nullopt;
		}
		return word | signed_field(addr_offset, ADDR_OFFSET_BITS) << ADDR_SHIFT |
		       static_cast<std::uint32_t>(data);
	}

	case OperandLayout::Mul: {
		const auto values = parse_operands<3>(operands);
		if (!values) {
			report_operands(opcode, address);
			return std::nullopt;
		}
		const auto [addr_offset, src_offset, factor] = *values;
		if (!check_signed("addr_offset", addr_offset, ADDR_OFFSET_BITS, address) ||
		    !check_signed("src_offset", src_offset, SRC_OFFSET_BITS, address) ||
		    !check_byte("factor", factor, address))
			return std::nullopt;
		return word | signed_field(src_offset, SRC_OFFSET_BITS) << NARROW_SHIFT |
		       signed_field(addr_offset, ADDR_OFFSET_BITS) << ADDR_SHIFT |
		       static_cast<std::uint32_t>(factor);
	}

	case OperandLayout::Branch:
	case OperandLayout::Scan: {
		const auto values = parse_operands<2>(operands);
		if (!values) {
			report_operands(opcode, address);
			return std::nullopt;
		}
		const auto [test_offset, wide] = *values;
		const std::string_view wide_name =
			opcode_layout(opcode) == OperandLayout::Scan ? "step" : "jump_offset";
		if (!check_signed("test_offset", test_offset, TEST_OFFSET_BITS, address) ||
		    !check_signed(wide_name, wide, LONG_OFFSET_BITS, address))
			return std::nullopt;
		return word | signed_field(test_offset, TEST_OFFSET_BITS) << NARROW_SHIFT |
		       signed_field(wide, LONG_OFFSET_BITS);
	}

	case OperandLayout::Move: {
		const auto values = parse_operands<1>(operands);
		if (!values) {
			report_operands(opcode, address);
			return std::nullopt;
		}
		const auto [move_offset] = *values;
		if (!check_signed("move_offset", move_offset, LONG_OFFSET_BITS, address))
			return std::nullopt;
		return word | signed_field(move_offset, LONG_OFFSET_BITS);
	}
	}
	return std::nullopt;
}

} // namespace

int main()
{
	std::size_t inst_ctr = 0;
	int exit_status = 0;
	std::string line;
	std::size_t line_no = 0;

	while (std::getline(std::cin, line)) {
		++line_no;
		if (ctre::match<"\\s*+">(line))
			continue;

		/* "<address>: <mnemonic> <operands>" */
		const auto parsed =
			ctre::match<"\\s*+([0-9]++)\\s*+:\\s*+([A-Z]++)\\s*+(.*?)\\s*+">(line);
		if (!parsed) {
			std::fprintf(stderr, "Error: malformed instruction on line %zu\n", line_no);
			exit_status = 1;
			continue;
		}

		const auto address_value = parse_number(parsed.get<1>().to_view());
		if (!address_value) {
			std::fprintf(stderr, "Error: invalid address on line %zu\n", line_no);
			exit_status = 1;
			continue;
		}
		const auto address = static_cast<addr_t>(*address_value);

		const auto name = parsed.get<2>().to_view();
		const auto opcode = parse_opcode(name);
		if (!opcode) {
			std::fprintf(stderr, "Error: unknown opcode %.*s at %zu\n",
			             static_cast<int>(name.size()), name.data(), address);
			exit_status = 1;
			continue;
		}

		if (const auto word = encode(*opcode, parsed.get<3>().to_view(), address)) {
			std::printf("%08x\n", *word);
			inst_ctr++;
		} else {
			exit_status = 1;
		}
	}

	std::fprintf(stderr, "Assembled %zu instruction(s)\n", inst_ctr);
	return exit_status;
}
