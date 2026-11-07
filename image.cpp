/* Program images: bit2bin's hexadecimal words, and packed images with their word-level LZ.
 * doc/bincode.txt describes both. */
#include "libbrainfunk.hpp"
#include "ctre.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t MAX_LITERAL = 128, MIN_COPY = 2, MAX_COPY = 129;
/* How many earlier places with the same two words the compressor tries. */
constexpr std::size_t MAX_CHAIN = 64;
constexpr std::size_t HEADER_BYTES = 14;
constexpr std::size_t FORMAT_LIMIT = std::size_t{1} << 24; // sizes are 3 bytes

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

std::vector<std::uint8_t> pack_image(std::span<const std::uint32_t> words) {
    if (words.size() >= FORMAT_LIMIT) throw BrainfunkException("image too large to pack: " + std::to_string(words.size()) + " words");
    const auto payload = lz_compress(words);
    if (payload.size() >= FORMAT_LIMIT) throw BrainfunkException("packed image too large");
    std::vector<std::uint8_t> out(PACKED_MAGIC.begin(), PACKED_MAGIC.end());
    put_little(out, static_cast<std::uint32_t>(payload.size()), 3);
    put_little(out, static_cast<std::uint32_t>(words.size()), 3);
    put_little(out, crc32(payload), 4);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::vector<std::uint32_t> unpack_image(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < HEADER_BYTES || !std::equal(PACKED_MAGIC.begin(), PACKED_MAGIC.end(), bytes.begin()))
        throw BrainfunkException("not a packed image");
    const std::size_t payload_size = get_little(bytes, 4, 3), word_count = get_little(bytes, 7, 3);
    if (bytes.size() - HEADER_BYTES != payload_size)
        throw BrainfunkException("packed image: the header says " + std::to_string(payload_size) + " bytes of payload, there are " +
                                 std::to_string(bytes.size() - HEADER_BYTES));
    const auto payload = bytes.subspan(HEADER_BYTES);
    if (crc32(payload) != get_little(bytes, 10, 4)) throw BrainfunkException("packed image: wrong CRC");
    auto words = lz_decompress(payload);
    if (words.size() != word_count)
        throw BrainfunkException("packed image: the header says " + std::to_string(word_count) + " words, the payload holds " +
                                 std::to_string(words.size()));
    return words;
}

std::vector<std::uint32_t> read_image(std::istream& in) {
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    if (text.starts_with(std::string_view(reinterpret_cast<const char*>(PACKED_MAGIC.data()), PACKED_MAGIC.size()))) {
        const auto* data = reinterpret_cast<const std::uint8_t*>(text.data());
        return unpack_image(std::span(data, text.size()));
    }

    std::vector<std::uint32_t> words;
    std::size_t line_number = 0;
    for (std::size_t start = 0; start < text.size();) {
        const auto end = std::min(text.find('\n', start), text.size());
        const std::string_view line(text.data() + start, end - start);
        start = end + 1;
        ++line_number;
        const auto word = ctre::match<"\\s*+([0-9a-fA-F]{8})\\s*+">(line);
        if (!word) throw BrainfunkException("malformed image, line " + std::to_string(line_number) + ": " + std::string(line));
        const auto digits = word.get<1>().to_view();
        std::uint32_t value = 0;
        std::from_chars(digits.data(), digits.data() + digits.size(), value, 16);
        words.push_back(value);
    }
    if (words.empty()) throw BrainfunkException("empty image");
    return words;
}

std::vector<std::uint32_t> read_image_file(const std::string& filename) {
    if (filename == "-") return read_image(std::cin);
    std::ifstream in(filename, std::ios::binary);
    if (!in.is_open()) throw BrainfunkException("cannot open " + filename);
    return read_image(in);
}
