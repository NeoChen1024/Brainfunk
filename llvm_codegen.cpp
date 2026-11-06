#include "libbrainfunk.hpp"

#include <ostream>
#include <string>
#include <string_view>

namespace {

struct Jump {
    addr_t pc;
    offset_t offset;
    std::size_t instruction_count;
};

addr_t jump_target(Jump jump) {
    const auto base = jump.pc + 1;

    if (jump.offset >= 0) {
        const auto forward = static_cast<addr_t>(jump.offset);
        if (base >= jump.instruction_count || forward >= jump.instruction_count - base)
            throw BrainfunkException("Invalid forward jump in LLVM IR emitter");
        return base + forward;
    }

    const auto backward = static_cast<addr_t>(-(jump.offset + 1)) + 1;
    if (backward > base)
        throw BrainfunkException("Invalid backward jump in LLVM IR emitter");
    return base - backward;
}

void emit_instruction_header(std::ostream& os, addr_t pc, Opcode opcode) {
    os << "\ninst." << pc << ": ; " << opcode_name(opcode) << '\n';
}

void emit_load_base(std::ostream& os, addr_t pc) {
    os << "  %base.ptr." << pc << " = load i64, ptr %base.ptr.addr\n";
}

/* Defines %<name>.ptr.<pc> as &tape[EA(offset)]; %base.ptr.<pc> must be loaded. */
void emit_cell_ptr(std::ostream& os, std::string_view name, addr_t pc, offset_t offset,
                   addr_t tape_size) {
    const auto normalized = effective_address(0, offset, tape_size);
    os << "  %" << name << ".sum." << pc << " = add i64 %base.ptr." << pc << ", " << normalized
       << '\n'
       << "  %" << name << ".index." << pc << " = and i64 %" << name << ".sum." << pc << ", "
       << tape_size - 1 << '\n'
       << "  %" << name << ".ptr." << pc << " = getelementptr i8, ptr %tape, i64 %" << name
       << ".index." << pc << '\n';
}

void emit_next(std::ostream& os, addr_t pc) {
    os << "  br label %inst." << pc + 1 << '\n';
}

} // namespace

void Brainfunk::dump_llvm_ir(std::ostream& os) const {
    if (bitcode_.empty())
        throw BrainfunkException("Cannot emit LLVM IR for empty bitcode");

    os << "; Brainfunk portable LLVM IR\n"
          "; This module intentionally omits target triple and target datalayout.\n"
          "; Runtime I/O and tape allocation are supplied by llvm-ir-wrapper.c.\n\n"
          "define i32 @brainfunk_program(ptr %tape, i64 %tape_size, "
          "ptr %io_context, ptr %read_byte, ptr %write_byte) {\n"
          "entry:\n"
          "  %base.ptr.addr = alloca i64\n"
          "  %tape.not_null = icmp ne ptr %tape, null\n"
          "  %tape.size.valid = icmp eq i64 %tape_size, "
       << memory_.size() << "\n"
          "  %arguments.valid = and i1 %tape.not_null, %tape.size.valid\n"
          "  br i1 %arguments.valid, label %initialize, label %invalid.arguments\n\n"
          "invalid.arguments:\n"
          "  ret i32 1\n\n"
          "initialize:\n"
          "  store i64 0, ptr %base.ptr.addr\n"
          "  br label %inst.0\n";

    for (addr_t pc = 0; pc < bitcode_.size(); ++pc) {
        const auto& instruction = bitcode_[pc];
        emit_instruction_header(os, pc, instruction.opcode());

        switch (instruction.opcode()) {
        case Opcode::X:
            os << "  ret i32 2\n";
            break;

        case Opcode::A: {
            emit_load_base(os, pc);
            emit_cell_ptr(os, "cell", pc, instruction.addr_offset(), memory_.size());
            os << "  %cell.value." << pc << " = load i8, ptr %cell.ptr." << pc << '\n'
               << "  %cell.updated." << pc << " = add i8 %cell.value." << pc << ", "
               << static_cast<unsigned>(instruction.data()) << '\n'
               << "  store i8 %cell.updated." << pc << ", ptr %cell.ptr." << pc << '\n';
            emit_next(os, pc);
            break;
        }

        case Opcode::S: {
            emit_load_base(os, pc);
            emit_cell_ptr(os, "cell", pc, instruction.addr_offset(), memory_.size());
            os << "  store i8 " << static_cast<unsigned>(instruction.data()) << ", ptr %cell.ptr."
               << pc << '\n';
            emit_next(os, pc);
            break;
        }

        case Opcode::MUL: {
            emit_load_base(os, pc);
            emit_cell_ptr(os, "source", pc, instruction.src_offset(), memory_.size());
            emit_cell_ptr(os, "destination", pc, instruction.addr_offset(), memory_.size());
            os << "  %source.value." << pc << " = load i8, ptr %source.ptr." << pc << '\n'
               << "  %destination.value." << pc << " = load i8, ptr %destination.ptr." << pc
               << '\n'
               << "  %product." << pc << " = mul i8 %source.value." << pc << ", "
               << static_cast<unsigned>(instruction.data()) << '\n'
               << "  %destination.updated." << pc << " = add i8 %destination.value." << pc
               << ", %product." << pc << '\n'
               << "  store i8 %destination.updated." << pc << ", ptr %destination.ptr." << pc
               << '\n';
            emit_next(os, pc);
            break;
        }

        case Opcode::F: {
            const auto normalized_step = effective_address(0, instruction.step(), memory_.size());
            os << "  br label %scan.test." << pc << "\n\n"
               << "scan.test." << pc << ":\n";
            emit_load_base(os, pc);
            emit_cell_ptr(os, "scan", pc, instruction.test_offset(), memory_.size());
            os << "  %scan.value." << pc << " = load i8, ptr %scan.ptr." << pc << '\n'
               << "  %scan.done." << pc << " = icmp eq i8 %scan.value." << pc << ", 0\n"
               << "  br i1 %scan.done." << pc << ", label %inst." << pc + 1
               << ", label %scan.move." << pc << "\n\n"
               << "scan.move." << pc << ":\n"
               << "  %scan.moved." << pc << " = add i64 %base.ptr." << pc << ", " << normalized_step
               << '\n'
               << "  %scan.next." << pc << " = and i64 %scan.moved." << pc << ", "
               << memory_.size() - 1 << '\n'
               << "  store i64 %scan.next." << pc << ", ptr %base.ptr.addr\n"
               << "  br label %scan.test." << pc << '\n';
            break;
        }

        case Opcode::M: {
            const auto normalized_move =
                effective_address(0, instruction.move_offset(), memory_.size());
            emit_load_base(os, pc);
            os << "  %base.ptr.moved." << pc << " = add i64 %base.ptr." << pc << ", "
               << normalized_move << '\n'
               << "  %base.ptr.next." << pc << " = and i64 %base.ptr.moved." << pc << ", "
               << memory_.size() - 1 << '\n'
               << "  store i64 %base.ptr.next." << pc << ", ptr %base.ptr.addr\n";
            emit_next(os, pc);
            break;
        }

        case Opcode::JE:
        case Opcode::JN: {
            const auto target = jump_target({
                .pc = pc,
                .offset = instruction.jump_offset(),
                .instruction_count = bitcode_.size(),
            });
            emit_load_base(os, pc);
            emit_cell_ptr(os, "cell", pc, instruction.test_offset(), memory_.size());
            os << "  %cell.value." << pc << " = load i8, ptr %cell.ptr." << pc << '\n'
               << "  %condition." << pc << " = icmp "
               << (instruction.opcode() == Opcode::JE ? "eq" : "ne")
               << " i8 %cell.value." << pc << ", 0\n"
               << "  br i1 %condition." << pc << ", label %inst." << target
               << ", label %inst." << pc + 1 << '\n';
            break;
        }

        case Opcode::IO: {
            emit_load_base(os, pc);
            emit_cell_ptr(os, "cell", pc, instruction.addr_offset(), memory_.size());
            if (instruction.data() == IO_IN) {
                os << "  %input." << pc << " = call i32 %read_byte(ptr %io_context)\n"
                   << "  %input.eof." << pc << " = icmp eq i32 %input." << pc << ", -1\n"
                   << "  %input.value." << pc << " = select i1 %input.eof." << pc
                   << ", i32 0, i32 %input." << pc << '\n'
                   << "  %input.byte." << pc << " = trunc i32 %input.value." << pc << " to i8\n"
                   << "  store i8 %input.byte." << pc << ", ptr %cell.ptr." << pc << '\n';
            } else {
                os << "  %output.byte." << pc << " = load i8, ptr %cell.ptr." << pc << '\n'
                   << "  %output.value." << pc << " = zext i8 %output.byte." << pc << " to i32\n"
                   << "  call void %write_byte(ptr %io_context, i32 %output.value." << pc << ")\n";
            }
            emit_next(os, pc);
            break;
        }

        case Opcode::H:
            os << "  ret i32 0\n";
            break;

        case Opcode::MULC: {
            emit_load_base(os, pc);
            emit_cell_ptr(os, "source", pc, instruction.src_offset(), memory_.size());
            emit_cell_ptr(os, "destination", pc, instruction.addr_offset(), memory_.size());
            os << "  %source.value." << pc << " = load i8, ptr %source.ptr." << pc << '\n'
               << "  %destination.value." << pc << " = load i8, ptr %destination.ptr." << pc
               << '\n'
               << "  %product." << pc << " = mul i8 %source.value." << pc << ", "
               << static_cast<unsigned>(instruction.data()) << '\n'
               << "  %destination.updated." << pc << " = add i8 %destination.value." << pc
               << ", %product." << pc << '\n'
               << "  store i8 %destination.updated." << pc << ", ptr %destination.ptr." << pc
               << '\n'
               << "  store i8 0, ptr %source.ptr." << pc << '\n';
            emit_next(os, pc);
            break;
        }

        case Opcode::DJN: {
            const auto target = jump_target({
                .pc = pc,
                .offset = instruction.jump_offset(),
                .instruction_count = bitcode_.size(),
            });
            emit_load_base(os, pc);
            emit_cell_ptr(os, "cell", pc, instruction.test_offset(), memory_.size());
            os << "  %cell.value." << pc << " = load i8, ptr %cell.ptr." << pc << '\n'
               << "  %cell.updated." << pc << " = add i8 %cell.value." << pc << ", 255\n"
               << "  store i8 %cell.updated." << pc << ", ptr %cell.ptr." << pc << '\n'
               << "  %condition." << pc << " = icmp ne i8 %cell.updated." << pc << ", 0\n"
               << "  br i1 %condition." << pc << ", label %inst." << target
               << ", label %inst." << pc + 1 << '\n';
            break;
        }

        case Opcode::MJN: {
            const auto target = jump_target({
                .pc = pc,
                .offset = instruction.jump_offset(),
                .instruction_count = bitcode_.size(),
            });
            const auto normalized_move =
                effective_address(0, instruction.move_offset(), memory_.size());
            emit_load_base(os, pc);
            os << "  %base.ptr.moved." << pc << " = add i64 %base.ptr." << pc << ", "
               << normalized_move << '\n'
               << "  %base.ptr.next." << pc << " = and i64 %base.ptr.moved." << pc << ", "
               << memory_.size() - 1 << '\n'
               << "  store i64 %base.ptr.next." << pc << ", ptr %base.ptr.addr\n"
               << "  %test.sum." << pc << " = add i64 %base.ptr.next." << pc << ", "
               << effective_address(0, instruction.test_offset(), memory_.size()) << '\n'
               << "  %test.index." << pc << " = and i64 %test.sum." << pc << ", "
               << memory_.size() - 1 << '\n'
               << "  %test.ptr." << pc << " = getelementptr i8, ptr %tape, i64 %test.index." << pc
               << '\n'
               << "  %test.value." << pc << " = load i8, ptr %test.ptr." << pc << '\n'
               << "  %condition." << pc << " = icmp ne i8 %test.value." << pc << ", 0\n"
               << "  br i1 %condition." << pc << ", label %inst." << target
               << ", label %inst." << pc + 1 << '\n';
            break;
        }

        case Opcode::SRUN: {
            emit_load_base(os, pc);
            for (unsigned i = 0; i < instruction.count(); ++i) {
                const auto name = "cell" + std::to_string(i);
                emit_cell_ptr(os, name, pc, instruction.addr_offset() + static_cast<offset_t>(i),
                              memory_.size());
                os << "  store i8 " << static_cast<unsigned>(instruction.data()) << ", ptr %"
                   << name << ".ptr." << pc << '\n';
            }
            emit_next(os, pc);
            break;
        }

        case Opcode::Count:
            throw BrainfunkException("Invalid opcode in LLVM IR emitter");
        }
    }

    os << "}\n";
}
