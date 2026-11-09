#include "brainfunk/trace_file.hpp"

#include <zstd.h>

#include <array>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <string>
#include <thread>

using nlohmann::json;

namespace {

constexpr std::array<char, 4> TRACE_MAGIC = {'B', 'F', 'T', 'R'};
constexpr std::uint8_t CHUNK_CONTROL = 1, CHUNK_END = 2;
constexpr std::uint32_t CHUNK_EVENTS = 1u << 16;

[[noreturn]] void fail(const std::string& what) { throw BrainfunkException("trace: " + what); }

/* Bytes into one zstd frame on a stream. */
class ZstdOut {
public:
    explicit ZstdOut(std::ostream& out) : out_(out), cctx_(ZSTD_createCCtx()), buffer_(ZSTD_CStreamOutSize()) {
        if (!cctx_) fail("cannot start zstd");
        ZSTD_CCtx_setParameter(cctx_, ZSTD_c_compressionLevel, 6);
        ZSTD_CCtx_setParameter(cctx_, ZSTD_c_checksumFlag, 1);
    }
    ~ZstdOut() { ZSTD_freeCCtx(cctx_); }
    ZstdOut(const ZstdOut&)            = delete;
    ZstdOut& operator=(const ZstdOut&) = delete;

    void write(const void* data, std::size_t size) { pump(data, size, ZSTD_e_continue); }
    void finish() { pump(nullptr, 0, ZSTD_e_end); }

private:
    void pump(const void* data, std::size_t size, ZSTD_EndDirective mode) {
        ZSTD_inBuffer in{data, size, 0};
        for (;;) {
            ZSTD_outBuffer out{buffer_.data(), buffer_.size(), 0};
            const std::size_t left = ZSTD_compressStream2(cctx_, &out, &in, mode);
            if (ZSTD_isError(left)) fail(ZSTD_getErrorName(left));
            out_.write(buffer_.data(), static_cast<std::streamsize>(out.pos));
            if (!out_) fail("cannot write the file");
            if (mode == ZSTD_e_end ? left == 0 : in.pos == in.size) break;
        }
    }

    std::ostream&     out_;
    ZSTD_CCtx*        cctx_;
    std::vector<char> buffer_;
};

/* Bytes out of a zstd frame on a stream, exactly as many as asked for. */
class ZstdIn {
public:
    explicit ZstdIn(std::istream& in) : in_(in), dctx_(ZSTD_createDCtx()), input_(ZSTD_DStreamInSize()) {
        if (!dctx_) fail("cannot start zstd");
    }
    ~ZstdIn() { ZSTD_freeDCtx(dctx_); }
    ZstdIn(const ZstdIn&)            = delete;
    ZstdIn& operator=(const ZstdIn&) = delete;

    void read(void* data, std::size_t size) {
        ZSTD_outBuffer out{data, size, 0};
        while (out.pos < out.size) {
            if (in_buffer_.pos == in_buffer_.size) {
                in_.read(input_.data(), static_cast<std::streamsize>(input_.size()));
                const auto got = static_cast<std::size_t>(in_.gcount());
                if (got == 0) fail("the file ends early");
                in_buffer_ = {input_.data(), got, 0};
            }
            const std::size_t result = ZSTD_decompressStream(dctx_, &out, &in_buffer_);
            if (ZSTD_isError(result)) fail(ZSTD_getErrorName(result));
        }
    }

    template <class T>
    T value() {
        std::array<std::uint8_t, sizeof(T)> bytes{};
        read(bytes.data(), bytes.size());
        T result = 0;
        for (std::size_t i = 0; i < sizeof(T); ++i) result |= static_cast<T>(static_cast<T>(bytes[i]) << (8 * i));
        return result;
    }

private:
    std::istream&     in_;
    ZSTD_DCtx*        dctx_;
    std::vector<char> input_;
    ZSTD_inBuffer     in_buffer_{nullptr, 0, 0};
};

/* Chunks from the thread that runs the program to the one that compresses and writes them. A full
 * queue holds the producer back, so that a fast run cannot outgrow memory; an error in the writer
 * comes back to the producer as the exception it was. */
class ChunkQueue {
public:
    explicit ChunkQueue(std::size_t capacity) : capacity_(capacity) {}

    void push(std::vector<std::uint8_t> chunk) {
        std::unique_lock lock(mutex_);
        not_full_.wait(lock, [&] { return queue_.size() < capacity_ || error_; });
        if (error_) std::rethrow_exception(error_);
        queue_.push_back(std::move(chunk));
        not_empty_.notify_one();
    }

    /* The next chunk, or false once the queue is closed and empty. */
    bool pop(std::vector<std::uint8_t>& chunk) {
        std::unique_lock lock(mutex_);
        not_empty_.wait(lock, [&] { return !queue_.empty() || closed_; });
        if (queue_.empty()) return false;
        chunk = std::move(queue_.front());
        queue_.pop_front();
        not_full_.notify_one();
        return true;
    }

    void close() {
        std::lock_guard lock(mutex_);
        closed_ = true;
        not_empty_.notify_one();
    }

    void fail(std::exception_ptr error) {
        std::lock_guard lock(mutex_);
        error_ = error;
        not_full_.notify_all();
    }

    [[nodiscard]] std::exception_ptr error() {
        std::lock_guard lock(mutex_);
        return error_;
    }

private:
    std::size_t                           capacity_;
    std::mutex                            mutex_;
    std::condition_variable               not_full_, not_empty_;
    std::deque<std::vector<std::uint8_t>> queue_;
    bool                                  closed_ = false;
    std::exception_ptr                    error_;
};

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}

void put_leb128(std::vector<std::uint8_t>& out, std::uint64_t value) {
    do {
        std::uint8_t byte = value & 0x7F;
        value >>= 7;
        if (value) byte |= 0x80;
        out.push_back(byte);
    } while (value);
}

} // namespace

TraceRange TraceHeader::range() const {
    TraceRange range;
    range.skip = meta.at("skip").get<std::uint64_t>();
    if (!meta.at("limit").is_null()) range.limit = meta.at("limit").get<std::uint64_t>();
    return range;
}

json trace_footer(const Brainfunk& bf, const TraceSummary& summary) {
    const auto memory = bf.memory();
    return {{"executed", summary.executed}, {"reported", summary.reported}, {"halted", summary.halted},
            {"trapped", summary.trapped}, {"endless", summary.endless}, {"pc", bf.pc()}, {"base_ptr", bf.base_ptr()},
            {"memory_crc32", crc32(std::span<const std::uint8_t>(memory.data(), memory.size()))}};
}

// ------------------------------------------------------------------
//  TraceWriter
// ------------------------------------------------------------------

struct TraceWriter::Impl {
    ZstdOut                    zstd;
    ChunkQueue                 queue{8};
    std::thread                writer;
    std::vector<std::uint32_t> words;
    std::string                input;
    TraceRange                 range;
    json                       extra;

    std::uint32_t              events = 0, branches = 0, scans = 0;
    std::vector<std::uint8_t>  bits, steps, chunk;

    Impl(std::ostream& out, std::span<const std::uint32_t> w, std::string_view in, TraceRange r, json e)
        : zstd(out), words(w.begin(), w.end()), input(in), range(r), extra(std::move(e)) {
        writer = std::thread([this] {
            try {
                std::vector<std::uint8_t> bytes;
                while (queue.pop(bytes)) zstd.write(bytes.data(), bytes.size());
                zstd.finish();
            } catch (...) {
                queue.fail(std::current_exception());
            }
        });
    }

    ~Impl() {
        queue.close();
        if (writer.joinable()) writer.join();
    }

    /* Closes the file, and reports what went wrong in writing it, if anything. */
    void finish() {
        queue.close();
        writer.join();
        if (const auto error = queue.error()) std::rethrow_exception(error);
    }

    void flush() {
        if (events == 0) return;
        chunk.clear();
        chunk.push_back(CHUNK_CONTROL);
        put_u32(chunk, events);
        put_u32(chunk, branches);
        chunk.insert(chunk.end(), bits.begin(), bits.end());
        put_u32(chunk, scans);
        chunk.insert(chunk.end(), steps.begin(), steps.end());
        queue.push(std::move(chunk));
        chunk = {};
        events = branches = scans = 0;
        bits.clear();
        steps.clear();
    }
};

TraceWriter::TraceWriter(std::ostream& out, std::span<const std::uint32_t> words, std::string_view input,
                         TraceRange range, json extra)
    : impl_(std::make_unique<Impl>(out, words, input, range, std::move(extra))) {}

TraceWriter::~TraceWriter() = default;

void TraceWriter::begin(const Brainfunk& bf) {
    auto& w = *impl_;
    json meta = w.extra;
    meta["memory"] = bf.memory().size();
    meta["eof"] = std::string(eof_mode_name(bf.eof()));
    meta["skip"] = w.range.skip;
    meta["limit"] = w.range.limit == TraceRange{}.limit ? json(nullptr) : json(w.range.limit);
    meta["words"] = w.words.size();
    meta["input"] = w.input.size();
    const std::string text = meta.dump();

    std::vector<std::uint8_t> head(TRACE_MAGIC.begin(), TRACE_MAGIC.end());
    head.push_back(TRACE_VERSION);
    head.insert(head.end(), 3, 0);
    put_u32(head, static_cast<std::uint32_t>(text.size()));
    head.insert(head.end(), text.begin(), text.end());
    for (const auto word : w.words) put_u32(head, word);
    head.insert(head.end(), w.input.begin(), w.input.end());
    w.queue.push(std::move(head));
}

void TraceWriter::event(const TraceEvent& e) {
    auto& w = *impl_;
    if (e.branch) {
        if (w.branches % 8 == 0) w.bits.push_back(0);
        if (e.taken) w.bits.back() |= static_cast<std::uint8_t>(1u << (w.branches % 8));
        ++w.branches;
    }
    if (e.opcode() == Opcode::F) {
        put_leb128(w.steps, e.steps);
        ++w.scans;
    }
    if (++w.events == CHUNK_EVENTS) w.flush();
}

void TraceWriter::end(const Brainfunk& bf, const TraceSummary& summary) {
    auto& w = *impl_;
    w.flush();
    const std::string text = trace_footer(bf, summary).dump();
    std::vector<std::uint8_t> tail{CHUNK_END};
    put_u32(tail, static_cast<std::uint32_t>(text.size()));
    tail.insert(tail.end(), text.begin(), text.end());
    w.queue.push(std::move(tail));
    w.finish();
}

// ------------------------------------------------------------------
//  TraceReader
// ------------------------------------------------------------------

struct TraceReader::Impl {
    ZstdIn      zstd;
    TraceHeader header;
    json        footer;
    bool        ended = false;

    std::vector<std::uint8_t> bits;
    std::uint32_t branches = 0, branch_at = 0;
    std::vector<std::uint64_t> scans;
    std::size_t scan_at = 0;

    explicit Impl(std::istream& in) : zstd(in) {}

    std::string text(std::uint32_t size) {
        std::string s(size, '\0');
        zstd.read(s.data(), size);
        return s;
    }

    /* The next chunk, until the end one; false once that has been read. */
    bool next_chunk() {
        if (ended) return false;
        const auto kind = zstd.value<std::uint8_t>();
        if (kind == CHUNK_END) {
            footer = json::parse(text(zstd.value<std::uint32_t>()));
            ended = true;
            return false;
        }
        if (kind != CHUNK_CONTROL) fail("unknown chunk " + std::to_string(kind));
        (void)zstd.value<std::uint32_t>();   // events, which only a reader that skips chunks needs
        branches = zstd.value<std::uint32_t>();
        branch_at = 0;
        bits.resize((branches + 7) / 8);
        zstd.read(bits.data(), bits.size());
        const auto count = zstd.value<std::uint32_t>();
        scans.clear();
        scan_at = 0;
        for (std::uint32_t i = 0; i < count; ++i) {
            std::uint64_t value = 0;
            for (int shift = 0;; shift += 7) {
                const auto byte = zstd.value<std::uint8_t>();
                if (shift > 63) fail("a stride count is too long");
                value |= static_cast<std::uint64_t>(byte & 0x7F) << shift;
                if (!(byte & 0x80)) break;
            }
            scans.push_back(value);
        }
        return true;
    }
};

TraceReader::TraceReader(std::istream& in) : impl_(std::make_unique<Impl>(in)) {
    auto& r = *impl_;
    std::array<char, 4> magic{};
    r.zstd.read(magic.data(), magic.size());
    if (magic != TRACE_MAGIC) fail("not a trace file");
    const auto version = r.zstd.value<std::uint8_t>();
    if (version != TRACE_VERSION) fail("version " + std::to_string(version) + " is not known");
    std::array<std::uint8_t, 3> reserved{};
    r.zstd.read(reserved.data(), reserved.size());
    r.header.meta = json::parse(r.text(r.zstd.value<std::uint32_t>()));
    r.header.words.resize(r.header.meta.at("words").get<std::size_t>());
    for (auto& word : r.header.words) word = r.zstd.value<std::uint32_t>();
    r.header.input = r.text(r.header.meta.at("input").get<std::uint32_t>());
}

TraceReader::~TraceReader() = default;

const TraceHeader& TraceReader::header() const noexcept { return impl_->header; }

bool TraceReader::next_branch() {
    auto& r = *impl_;
    while (r.branch_at == r.branches) {
        if (!r.next_chunk()) fail("more branches than the file has");
    }
    const auto at = r.branch_at++;
    return (r.bits[at / 8] >> (at % 8)) & 1;
}

std::uint64_t TraceReader::next_scan() {
    auto& r = *impl_;
    while (r.scan_at == r.scans.size()) {
        if (!r.next_chunk()) fail("more F than the file has");
    }
    return r.scans[r.scan_at++];
}

const json& TraceReader::footer() {
    auto& r = *impl_;
    while (r.next_chunk()) {
        if (r.branches != 0 || !r.scans.empty()) fail("the run ended before the file did");
    }
    if (r.branch_at != r.branches || r.scan_at != r.scans.size()) fail("the run ended before the file did");
    return r.footer;
}

// ------------------------------------------------------------------
//  TraceCheck
// ------------------------------------------------------------------

void TraceCheck::event(const TraceEvent& e) {
    if (e.branch && reader_.next_branch() != e.taken) {
        fail("branch at pc " + std::to_string(e.pc) + ", instruction " + std::to_string(e.index) + ", went the other way");
    }
    if (e.opcode() == Opcode::F && reader_.next_scan() != e.steps) {
        fail("F at pc " + std::to_string(e.pc) + ", instruction " + std::to_string(e.index) + ", went another distance");
    }
    ++events_;
}

void TraceCheck::end(const Brainfunk& bf, const TraceSummary& summary) {
    const auto& recorded = reader_.footer();
    const auto now = trace_footer(bf, summary);
    if (recorded != now) fail("the run ended as " + now.dump() + ", not as recorded, " + recorded.dump());
}
