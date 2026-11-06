/* Bitcode optimization passes.  See doc/optimizer.md for what each one does and
 * why it is correct. */
#include "libbrainfunk.hpp"

#include <span>
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
    relink_jumps(code);
    bitcode_ = std::move(code);
}
