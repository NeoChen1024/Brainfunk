/* Bitcode optimization passes.  See doc/optimizer.md for what each one does and
 * why it is correct. */
#include "libbrainfunk.hpp"

#include <map>
#include <span>
#include <utility>
#include <vector>

namespace {

/* ------------------------------------------------------------------
 *  lazy_move: stop emitting M where an operand offset can say the same
 *
 *  `pending` is how far the pointer of the original program is ahead of
 *  base_ptr: original pointer == base_ptr + pending, at every point.  Each label
 *  needs one value of `pending` on all incoming edges, which structured loops
 *  make simple: a loop only has to end with the value it started with.
 * ------------------------------------------------------------------ */
std::vector<Bitcode> lazy_move(std::span<const Bitcode> code) {
    std::vector<Bitcode> out;
    out.reserve(code.size());

    offset_t pending = 0;
    std::vector<offset_t> loop_entry_pending;

    const auto flush = [&] {
        if (pending != 0) {
            out.push_back(Bitcode::move(pending));
            pending = 0;
        }
    };

    for (const auto& instruction : code) {
        const auto opcode = instruction.opcode();

        switch (opcode) {
        case Opcode::M:
            if (!fits_signed(pending + instruction.move_offset(), LONG_OFFSET_BITS)) flush();
            pending += instruction.move_offset();
            break;

        case Opcode::A:
        case Opcode::S:
        case Opcode::IO:
            if (!fits_signed(instruction.addr_offset() + pending, ADDR_OFFSET_BITS)) flush();
            out.push_back(Bitcode::data_op(opcode, instruction.addr_offset() + pending,
                                           instruction.data()));
            break;

        case Opcode::MUL:
            if (!fits_signed(instruction.addr_offset() + pending, ADDR_OFFSET_BITS) ||
                !fits_signed(instruction.src_offset() + pending, SRC_OFFSET_BITS)) {
                flush();
            }
            out.push_back(Bitcode::mul(instruction.addr_offset() + pending,
                                       instruction.src_offset() + pending, instruction.data()));
            break;

        case Opcode::JE:
            if (!fits_signed(instruction.test_offset() + pending, TEST_OFFSET_BITS)) flush();
            loop_entry_pending.push_back(pending);
            out.push_back(Bitcode::branch(opcode, instruction.test_offset() + pending, 0));
            break;

        case Opcode::JN: {
            /* The back edge re-enters the body, so restore the value it was
             * entered with.  A non-zero difference is real pointer motion. */
            const auto entry = loop_entry_pending.back();
            loop_entry_pending.pop_back();
            if (pending != entry) {
                out.push_back(Bitcode::move(pending - entry));
                pending = entry;
            }
            out.push_back(Bitcode::branch(opcode, instruction.test_offset() + pending, 0));
            break;
        }

        case Opcode::F:
            if (!fits_signed(instruction.test_offset() + pending, TEST_OFFSET_BITS)) flush();
            out.push_back(Bitcode::scan(instruction.test_offset() + pending, instruction.step()));
            break;

        case Opcode::H:
            /* Leaves the architectural state identical to the unoptimized program. */
            flush();
            out.push_back(instruction);
            break;

        case Opcode::X:
        case Opcode::Count:
            throw BrainfunkException("Invalid instruction in optimizer");
        }
    }

    return out;
}

/* ------------------------------------------------------------------
 *  fold: within a block, know what cells hold and write each cell once
 *
 *  Between two control-flow boundaries every cell is named by a constant offset
 *  from base_ptr, so whether two instructions touch the same cell is known at
 *  compile time.  Writes wait in `pending` until something reads the cell or the
 *  block ends; `known` holds the cells whose exact value is known.
 *
 *  Cells are keyed by their address modulo memory size, because two different
 *  offsets can name the same cell on a small tape.
 * ------------------------------------------------------------------ */
class Folder {
public:
    Folder(std::span<const Bitcode> code, addr_t memory_size)
        : code_(code), memory_size_(memory_size) {
        out_.reserve(code.size());
    }

    std::vector<Bitcode> run() {
        const auto loop_ends = find_loop_ends();

        for (std::size_t pc = 0; pc < code_.size(); ++pc) {
            const auto& instruction = code_[pc];

            switch (instruction.opcode()) {
            case Opcode::A:
                add(instruction.addr_offset(), instruction.data());
                break;

            case Opcode::S:
                set(instruction.addr_offset(), instruction.data());
                break;

            case Opcode::IO:
                if (instruction.data() == IO_IN) {
                    /* Input always stores, even at end of input: earlier writes are dead. */
                    pending_.erase(key(instruction.addr_offset()));
                    known_.erase(key(instruction.addr_offset()));
                } else {
                    flush_cell(instruction.addr_offset());
                }
                out_.push_back(instruction);
                break;

            case Opcode::MUL:
                multiply(instruction);
                break;

            case Opcode::JE:
                if (is_known_zero(instruction.test_offset())) {
                    pc = loop_ends[pc];   // the loop never runs
                } else {
                    flush_all();
                    out_.push_back(instruction);
                    known_.clear();
                }
                break;

            case Opcode::JN:
                flush_all();
                out_.push_back(instruction);
                learn_exit_condition(instruction.test_offset());
                break;

            case Opcode::F:
                if (!is_known_zero(instruction.test_offset())) {
                    flush_all();
                    out_.push_back(instruction);
                    learn_exit_condition(instruction.test_offset());
                }
                break;

            case Opcode::M:
                flush_all();
                out_.push_back(instruction);
                rebase_known(instruction.move_offset());
                break;

            case Opcode::H:
                flush_all();
                out_.push_back(instruction);
                break;

            case Opcode::X:
            case Opcode::Count:
                throw BrainfunkException("Invalid instruction in optimizer");
            }
        }

        return std::move(out_);
    }

private:
    struct PendingWrite {
        bool     is_set;   // otherwise an add
        memory_t value;
        offset_t offset;   // as written by the program; keeps the field in range
    };

    std::span<const Bitcode>       code_;
    addr_t                         memory_size_;
    std::vector<Bitcode>           out_;
    std::map<addr_t, PendingWrite> pending_;
    std::map<addr_t, memory_t>     known_;

    [[nodiscard]] addr_t key(offset_t offset) const {
        return effective_address(0, offset, memory_size_);
    }

    [[nodiscard]] bool is_known_zero(offset_t offset) const {
        const auto found = known_.find(key(offset));
        return found != known_.end() && found->second == 0;
    }

    [[nodiscard]] std::vector<std::size_t> find_loop_ends() const {
        std::vector<std::size_t> ends(code_.size(), 0);
        std::vector<std::size_t> open_loops;
        for (std::size_t pc = 0; pc < code_.size(); ++pc) {
            if (code_[pc].opcode() == Opcode::JE) {
                open_loops.push_back(pc);
            } else if (code_[pc].opcode() == Opcode::JN) {
                ends[open_loops.back()] = pc;
                open_loops.pop_back();
            }
        }
        return ends;
    }

    void add(offset_t offset, memory_t value) {
        const auto cell = key(offset);
        if (const auto found = known_.find(cell); found != known_.end()) {
            found->second = static_cast<memory_t>(found->second + value);
            pending_[cell] = {.is_set = true, .value = found->second, .offset = offset};
            return;
        }
        if (const auto found = pending_.find(cell); found != pending_.end()) {
            found->second.value = static_cast<memory_t>(found->second.value + value);
            if (found->second.value == 0) pending_.erase(found);
            return;
        }
        if (value != 0) {
            pending_[cell] = {.is_set = false, .value = value, .offset = offset};
        }
    }

    void set(offset_t offset, memory_t value) {
        const auto cell = key(offset);
        const auto found = known_.find(cell);
        const bool already_stored =
            !pending_.contains(cell) && found != known_.end() && found->second == value;
        if (already_stored) return;

        known_[cell] = value;
        pending_[cell] = {.is_set = true, .value = value, .offset = offset};
    }

    void multiply(const Bitcode& instruction) {
        const auto source = known_.find(key(instruction.src_offset()));
        if (source != known_.end()) {
            add(instruction.addr_offset(),
                static_cast<memory_t>(source->second * instruction.data()));
            return;
        }
        flush_cell(instruction.src_offset());
        flush_cell(instruction.addr_offset());
        out_.push_back(instruction);
        known_.erase(key(instruction.addr_offset()));
    }

    void emit_write(const PendingWrite& write) {
        out_.push_back(Bitcode::data_op(write.is_set ? Opcode::S : Opcode::A, write.offset,
                                        write.value));
    }

    void flush_cell(offset_t offset) {
        const auto found = pending_.find(key(offset));
        if (found != pending_.end()) {
            emit_write(found->second);
            pending_.erase(found);
        }
    }

    void flush_all() {
        for (const auto& [cell, write] : pending_) emit_write(write);
        pending_.clear();
    }

    /* A loop or scan only ends once the cell it tests is zero. */
    void learn_exit_condition(offset_t test_offset) {
        known_.clear();
        known_[key(test_offset)] = 0;
    }

    /* The same cells, named relative to the moved base_ptr. */
    void rebase_known(offset_t move_offset) {
        std::map<addr_t, memory_t> rebased;
        for (const auto& [cell, value] : known_) {
            rebased[effective_address(cell, -move_offset, memory_size_)] = value;
        }
        known_ = std::move(rebased);
    }
};

/* Recomputes jump offsets after instructions were added or removed.  Every
 * branch comes from a matched bracket, so a stack recovers the pairing. */
void relink_jumps(std::vector<Bitcode>& code) {
    std::vector<std::size_t> open_loops;

    for (std::size_t pc = 0; pc < code.size(); ++pc) {
        if (code[pc].opcode() == Opcode::JE) {
            open_loops.push_back(pc);
        } else if (code[pc].opcode() == Opcode::JN) {
            const auto open = open_loops.back();
            open_loops.pop_back();
            const auto distance = static_cast<offset_t>(pc) - static_cast<offset_t>(open);
            code[open] = Bitcode::branch(Opcode::JE, code[open].test_offset(), distance);
            code[pc] = Bitcode::branch(Opcode::JN, code[pc].test_offset(), -distance);
        }
    }
}

} // anonymous namespace

void Brainfunk::optimize(int level) {
    if (level < OPT_NONE || level > OPT_MAX) {
        throw BrainfunkException("Invalid optimization level");
    }
    if (level == OPT_NONE || bitcode_.empty()) return;

    reset_state();
    auto code = lazy_move(bitcode_);
    if (level >= OPT_FOLD) code = Folder(code, memory_.size()).run();
    relink_jumps(code);
    bitcode_ = std::move(code);
}
