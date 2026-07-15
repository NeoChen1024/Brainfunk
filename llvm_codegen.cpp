#include "libbrainfunk.hpp"

#include <ostream>

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

void emit_current_cell(std::ostream& os, addr_t pc) {
    os << "  %data.ptr." << pc << " = load i64, ptr %data.ptr.addr\n"
          "  %cell.ptr." << pc
       << " = getelementptr i8, ptr %tape, i64 %data.ptr." << pc << '\n';
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
          "  %data.ptr.addr = alloca i64\n"
          "  %tape.not_null = icmp ne ptr %tape, null\n"
          "  %tape.size.valid = icmp eq i64 %tape_size, "
       << memory_.size() << "\n"
          "  %arguments.valid = and i1 %tape.not_null, %tape.size.valid\n"
          "  br i1 %arguments.valid, label %initialize, label %invalid.arguments\n\n"
          "invalid.arguments:\n"
          "  ret i32 1\n\n"
          "initialize:\n"
          "  store i64 0, ptr %data.ptr.addr\n"
          "  br label %inst.0\n";

    for (addr_t pc = 0; pc < bitcode_.size(); ++pc) {
        const auto& instruction = bitcode_[pc];
        emit_instruction_header(os, pc, instruction.opcode_);

        switch (instruction.opcode_) {
        case Opcode::X:
            os << "  ret i32 2\n";
            break;

        case Opcode::A: {
            emit_current_cell(os, pc);
            const auto value = std::get<memory_t>(instruction.operand_);
            os << "  %cell.value." << pc << " = load i8, ptr %cell.ptr." << pc << '\n'
               << "  %cell.updated." << pc << " = add i8 %cell.value." << pc << ", "
               << static_cast<unsigned>(value) << '\n'
               << "  store i8 %cell.updated." << pc << ", ptr %cell.ptr." << pc << '\n';
            emit_next(os, pc);
            break;
        }

        case Opcode::S: {
            emit_current_cell(os, pc);
            const auto value = std::get<memory_t>(instruction.operand_);
            os << "  store i8 " << static_cast<unsigned>(value) << ", ptr %cell.ptr." << pc
               << '\n';
            emit_next(os, pc);
            break;
        }

        case Opcode::MUL: {
            const auto operand = std::get<DualOperand>(instruction.operand_);
            const auto normalized_offset = wrap_offset(0, operand.offset, memory_.size());
            emit_current_cell(os, pc);
            os << "  %source.value." << pc << " = load i8, ptr %cell.ptr." << pc << '\n'
               << "  %destination.unwrapped." << pc << " = add i64 %data.ptr." << pc << ", "
               << normalized_offset << '\n'
               << "  %destination.index." << pc << " = urem i64 %destination.unwrapped." << pc
               << ", %tape_size\n"
               << "  %destination.ptr." << pc
               << " = getelementptr i8, ptr %tape, i64 %destination.index." << pc << '\n'
               << "  %destination.value." << pc << " = load i8, ptr %destination.ptr." << pc
               << '\n'
               << "  %product." << pc << " = mul i8 %source.value." << pc << ", "
               << static_cast<unsigned>(operand.mul) << '\n'
               << "  %destination.updated." << pc << " = add i8 %destination.value." << pc
               << ", %product." << pc << '\n'
               << "  store i8 %destination.updated." << pc << ", ptr %destination.ptr." << pc
               << '\n';
            emit_next(os, pc);
            break;
        }

        case Opcode::F: {
            const auto offset = std::get<offset_t>(instruction.operand_);
            const auto normalized_offset = wrap_offset(0, offset, memory_.size());
            os << "  br label %scan.test." << pc << "\n\n"
               << "scan.test." << pc << ":\n"
               << "  %scan.data.ptr." << pc << " = load i64, ptr %data.ptr.addr\n"
               << "  %scan.cell.ptr." << pc
               << " = getelementptr i8, ptr %tape, i64 %scan.data.ptr." << pc << '\n'
               << "  %scan.value." << pc << " = load i8, ptr %scan.cell.ptr." << pc << '\n'
               << "  %scan.done." << pc << " = icmp eq i8 %scan.value." << pc << ", 0\n"
               << "  br i1 %scan.done." << pc << ", label %inst." << pc + 1
               << ", label %scan.move." << pc << "\n\n"
               << "scan.move." << pc << ":\n"
               << "  %scan.unwrapped." << pc << " = add i64 %scan.data.ptr." << pc << ", "
               << normalized_offset << '\n'
               << "  %scan.next." << pc << " = urem i64 %scan.unwrapped." << pc
               << ", %tape_size\n"
               << "  store i64 %scan.next." << pc << ", ptr %data.ptr.addr\n"
               << "  br label %scan.test." << pc << '\n';
            break;
        }

        case Opcode::M: {
            const auto offset = std::get<offset_t>(instruction.operand_);
            const auto normalized_offset = wrap_offset(0, offset, memory_.size());
            os << "  %data.ptr." << pc << " = load i64, ptr %data.ptr.addr\n"
               << "  %data.ptr.unwrapped." << pc << " = add i64 %data.ptr." << pc << ", "
               << normalized_offset << '\n'
               << "  %data.ptr.next." << pc << " = urem i64 %data.ptr.unwrapped." << pc
               << ", %tape_size\n"
               << "  store i64 %data.ptr.next." << pc << ", ptr %data.ptr.addr\n";
            emit_next(os, pc);
            break;
        }

        case Opcode::JE:
        case Opcode::JN: {
            const auto offset = std::get<offset_t>(instruction.operand_);
            const auto target = jump_target({
                .pc = pc,
                .offset = offset,
                .instruction_count = bitcode_.size(),
            });
            emit_current_cell(os, pc);
            os << "  %cell.value." << pc << " = load i8, ptr %cell.ptr." << pc << '\n'
               << "  %condition." << pc << " = icmp "
               << (instruction.opcode_ == Opcode::JE ? "eq" : "ne")
               << " i8 %cell.value." << pc << ", 0\n"
               << "  br i1 %condition." << pc << ", label %inst." << target
               << ", label %inst." << pc + 1 << '\n';
            break;
        }

        case Opcode::IO: {
            const auto operation = std::get<memory_t>(instruction.operand_);
            emit_current_cell(os, pc);
            if (operation == IO_IN) {
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

        case Opcode::Count:
            throw BrainfunkException("Invalid opcode in LLVM IR emitter");
        }
    }

    os << "}\n";
}
