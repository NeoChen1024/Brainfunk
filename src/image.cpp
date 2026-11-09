/* Program images: bincode as words, as text, as a listing, and as image files, plain or
 * packed with word-level LZ.  doc/bincode.txt describes them. */
#include "brainfunk/libbrainfunk.hpp"
#include "ctre.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t MAX_LITERAL = 128, MIN_COPY = 2, MAX_COPY = 129;
/* How many earlier places with the same two words the compressor tries. */
constexpr std::size_t MAX_CHAIN = 64;
/* Bits 1:0 of the flags byte; the rest are reserved. */
constexpr std::uint8_t COMPRESSION_MASK = 0x03;
/* Flags bits 3:2: 0 says nothing about end of input, otherwise the EofMode plus one. */
constexpr std::uint8_t EOF_MASK  = 0x0C;
constexpr int          EOF_SHIFT = 2;

void put_little(std::vector<std::uint8_t>& out, std::uint32_t value, int bytes) {
    for (int i = 0; i < bytes; ++i) out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}

std::uint32_t get_little(std::span<const std::uint8_t> bytes, std::size_t at, int count) {
    std::uint32_t value = 0;
    for (int i = count - 1; i >= 0; --i) value = value << 8 | bytes[at + static_cast<std::size_t>(i)];
    return value;
}

} // namespace

std::vector<std::uint8_t> lz_compress(std::span<const std::uint32_t> words, std::size_t max_distance) {
    max_distance = std::min(max_distance, LZ_MAX_DISTANCE);
    std::vector<std::uint8_t> out;
    /* Every earlier place, by the two words that start there. */
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::vector<std::size_t>> seen;
    std::size_t literal_start = 0;

    const auto flush_literals = [&](std::size_t end) {
        while (literal_start < end) {
            const std::size_t run = std::min(end - literal_start, MAX_LITERAL);
            out.push_back(static_cast<std::uint8_t>(run - 1));
            for (std::size_t k = 0; k < run; ++k) put_little(out, words[literal_start + k], 4);
            literal_start += run;
        }
    };
    const auto remember = [&](std::size_t at) {
        if (at + 1 < words.size()) seen[{words[at], words[at + 1]}].push_back(at);
    };

    std::size_t at = 0;
    while (at < words.size()) {
        std::size_t best = 0, best_distance = 0;
        if (at + 1 < words.size()) {
            if (const auto found = seen.find({words[at], words[at + 1]}); found != seen.end()) {
                const auto& places = found->second;
                std::size_t tried = 0;
                for (auto place = places.rbegin(); place != places.rend() && tried < MAX_CHAIN; ++place, ++tried) {
                    if (at - *place > max_distance) break;
                    std::size_t length = 0;
                    while (at + length < words.size() && length < MAX_COPY && words[*place + length] == words[at + length])
                        ++length;
                    if (length > best) {
                        best = length;
                        best_distance = at - *place;
                    }
                }
            }
        }
        if (best >= MIN_COPY) {
            flush_literals(at);
            out.push_back(static_cast<std::uint8_t>(0x80 | (best - MIN_COPY)));
            put_little(out, static_cast<std::uint32_t>(best_distance), 2);
            for (std::size_t k = 0; k < best; ++k) remember(at + k);
            at += best;
            literal_start = at;
        } else {
            remember(at);
            ++at;
        }
    }
    flush_literals(at);
    return out;
}

std::vector<std::uint32_t> lz_decompress(std::span<const std::uint8_t> payload) {
    std::vector<std::uint32_t> words;
    std::size_t at = 0;
    while (at < payload.size()) {
        const std::uint8_t token = payload[at++];
        if (token & 0x80) {
            if (payload.size() - at < 2) throw BrainfunkException("packed image: a copy is cut short");
            const std::size_t distance = get_little(payload, at, 2);
            at += 2;
            if (distance == 0 || distance > words.size())
                throw BrainfunkException("packed image: a copy from before the start, at word " + std::to_string(words.size()));
            /* One word at a time, so a copy may overlap itself. */
            for (std::size_t k = 0; k < (token & 0x7Fu) + MIN_COPY; ++k) {
                const std::uint32_t word = words[words.size() - distance];
                words.push_back(word);
            }
        } else {
            const std::size_t count = token + 1u;
            if (payload.size() - at < 4 * count) throw BrainfunkException("packed image: literal words are cut short");
            for (std::size_t k = 0; k < count; ++k, at += 4) words.push_back(get_little(payload, at, 4));
        }
    }
    return words;
}

std::uint32_t crc32(std::span<const std::uint8_t> bytes) noexcept {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const auto byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
    }
    return ~crc;
}

std::vector<std::uint8_t> pack_image(std::span<const std::uint32_t> words, Compression compression,
                                     std::optional<EofMode> eof) {
    std::vector<std::uint8_t> payload;
    if (compression == Compression::LZ) {
        payload = lz_compress(words);
    } else {
        for (const auto word : words) put_little(payload, word, 4);
    }
    if (words.size() > UINT32_MAX || payload.size() > UINT32_MAX) throw BrainfunkException("image too large");

    std::vector<std::uint8_t> out(IMAGE_MAGIC.begin(), IMAGE_MAGIC.end());
    out.push_back(IMAGE_VERSION);
    const std::uint8_t eof_field = eof ? static_cast<std::uint8_t>(static_cast<std::uint8_t>(*eof) + 1u) : 0;
    out.push_back(static_cast<std::uint8_t>(static_cast<std::uint8_t>(compression) | eof_field << EOF_SHIFT));
    put_little(out, 0, 2); // reserved
    put_little(out, static_cast<std::uint32_t>(payload.size()), 4);
    put_little(out, static_cast<std::uint32_t>(words.size()), 4);
    /* The CRC covers the header after the magic, then the payload. */
    std::vector<std::uint8_t> covered(out.begin() + 4, out.end());
    covered.insert(covered.end(), payload.begin(), payload.end());
    put_little(out, crc32(covered), 4);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::vector<std::uint32_t> unpack_image(std::span<const std::uint8_t> bytes) {
    return unpack_program(bytes).words;
}

Program unpack_program(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 5 || !std::equal(IMAGE_MAGIC.begin(), IMAGE_MAGIC.end(), bytes.begin()))
        throw BrainfunkException("not an image file");
    if (bytes[4] != IMAGE_VERSION)
        throw BrainfunkException("image file version " + std::to_string(bytes[4]) + ", this reads version " +
                                 std::to_string(IMAGE_VERSION));
    if (bytes.size() < IMAGE_HEADER_BYTES) throw BrainfunkException("image file: the header is cut short");
    const std::uint8_t flags = bytes[5];
    const auto compression = flags & COMPRESSION_MASK;
    if ((flags & ~(COMPRESSION_MASK | EOF_MASK)) != 0 || get_little(bytes, 6, 2) != 0)
        throw BrainfunkException("image file: reserved header bits are set");
    if (compression > static_cast<std::uint8_t>(Compression::LZ))
        throw BrainfunkException("image file: unknown compression " + std::to_string(compression));

    const std::size_t payload_size = get_little(bytes, 8, 4), word_count = get_little(bytes, 12, 4);
    if (bytes.size() - IMAGE_HEADER_BYTES != payload_size)
        throw BrainfunkException("image file: the header says " + std::to_string(payload_size) + " bytes of payload, there are " +
                                 std::to_string(bytes.size() - IMAGE_HEADER_BYTES));
    std::vector<std::uint8_t> covered(bytes.begin() + 4, bytes.begin() + 16);
    covered.insert(covered.end(), bytes.begin() + IMAGE_HEADER_BYTES, bytes.end());
    if (crc32(covered) != get_little(bytes, 16, 4)) throw BrainfunkException("image file: wrong CRC");

    const auto payload = bytes.subspan(IMAGE_HEADER_BYTES);
    std::vector<std::uint32_t> words;
    if (compression == static_cast<std::uint8_t>(Compression::LZ)) {
        words = lz_decompress(payload);
    } else {
        if (payload.size() % 4 != 0) throw BrainfunkException("image file: the payload is not whole words");
        for (std::size_t at = 0; at < payload.size(); at += 4) words.push_back(get_little(payload, at, 4));
    }
    if (words.size() != word_count)
        throw BrainfunkException("image file: the header says " + std::to_string(word_count) + " words, the payload holds " +
                                 std::to_string(words.size()));
    const auto eof_field = (flags & EOF_MASK) >> EOF_SHIFT;
    return {std::move(words), eof_field == 0 ? std::nullopt : std::optional(static_cast<EofMode>(eof_field - 1))};
}

std::vector<std::uint32_t> encode_bincode(std::span<const Bitcode> code) {
    std::vector<std::uint32_t> words;
    words.reserve(code.size());
    for (std::size_t pc = 0; pc < code.size(); ++pc) {
        try {
            words.push_back(code[pc].encode());
        } catch (const BrainfunkException& error) {
            throw BrainfunkException("at pc " + std::to_string(pc) + ": " + error.what());
        }
    }
    return words;
}

std::vector<Bitcode> decode_bincode(std::span<const std::uint32_t> words) {
    std::vector<Bitcode> code;
    code.reserve(words.size());
    for (const auto word : words) code.push_back(Bitcode::decode(word));
    return code;
}

void write_listing(std::ostream& os, std::span<const Bitcode> code) {
    for (std::size_t pc = 0; pc < code.size(); ++pc) os << pc << ":\t" << code[pc].to_string() << '\n';
}

namespace {

/* Calls take(line number, line) for each line, the last one without a newline too. */
template <typename Take>
void for_each_line(std::string_view text, Take take) {
    std::size_t number = 0;
    for (std::size_t start = 0; start < text.size();) {
        const auto end = std::min(text.find('\n', start), text.size());
        take(++number, text.substr(start, end - start));
        start = end + 1;
    }
}

bool blank(std::string_view line) {
    return line.find_first_not_of(" \t\r") == std::string_view::npos;
}

} // namespace

std::vector<Bitcode> parse_listing(std::string_view text) {
    std::vector<Bitcode> code;
    for_each_line(text, [&](std::size_t number, std::string_view line) {
        if (blank(line)) return;
        const auto parts = ctre::match<"\\s*+([0-9]++)\\s*+:(.*)">(line);
        if (!parts) throw BrainfunkException("listing, line " + std::to_string(number) + ": no pc: " + std::string(line));
        const auto digits = parts.get<1>().to_view();
        std::size_t pc = 0;
        std::from_chars(digits.data(), digits.data() + digits.size(), pc);
        if (pc != code.size())
            throw BrainfunkException("listing, line " + std::to_string(number) + ": pc " + std::to_string(pc) + " where " +
                                     std::to_string(code.size()) + " comes next");
        try {
            code.push_back(Bitcode::parse(parts.get<2>().to_view()));
        } catch (const BrainfunkException& error) {
            throw BrainfunkException("listing, line " + std::to_string(number) + ": " + error.what());
        }
    });
    if (code.empty()) throw BrainfunkException("empty listing");
    return code;
}

void write_hex(std::ostream& os, std::span<const std::uint32_t> words) {
    const auto flags = os.flags();
    const auto fill = os.fill('0');
    for (const auto word : words) os << std::hex << std::setw(8) << word << '\n';
    os.flags(flags);
    os.fill(fill);
}

std::vector<std::uint32_t> read_image(std::istream& in) {
    return read_program(in).words;
}

Program read_program(std::istream& in) {
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    if (text.starts_with(std::string_view(reinterpret_cast<const char*>(IMAGE_MAGIC.data()), IMAGE_MAGIC.size()))) {
        const auto* data = reinterpret_cast<const std::uint8_t*>(text.data());
        return unpack_program(std::span(data, text.size()));
    }

    /* A listing has a pc and a colon on every line, a hexadecimal image never. */
    const auto first = text.substr(0, text.find('\n'));
    if (first.find(':') != std::string::npos) return {encode_bincode(parse_listing(text)), std::nullopt};

    std::vector<std::uint32_t> words;
    for_each_line(text, [&](std::size_t number, std::string_view line) {
        const auto word = ctre::match<"\\s*+([0-9a-fA-F]{8})\\s*+">(line);
        if (!word) throw BrainfunkException("hexadecimal image, line " + std::to_string(number) + ": " + std::string(line));
        const auto digits = word.get<1>().to_view();
        std::uint32_t value = 0;
        std::from_chars(digits.data(), digits.data() + digits.size(), value, 16);
        words.push_back(value);
    });
    if (words.empty()) throw BrainfunkException("empty image");
    return {std::move(words), std::nullopt};
}

std::vector<std::uint32_t> read_image_file(const std::string& filename) {
    return read_program_file(filename).words;
}

Program read_program_file(const std::string& filename) {
    if (filename == "-") return read_program(std::cin);
    std::ifstream in(filename, std::ios::binary);
    if (!in.is_open()) throw BrainfunkException("cannot open " + filename);
    return read_program(in);
}
