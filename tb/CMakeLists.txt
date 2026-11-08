# The cosimulator: the SoC in Verilator, in lock step with the interpreter. See cosim.cpp.
cmake_policy(SET CMP0144 NEW)
find_package(verilator HINTS $ENV{VERILATOR_ROOT})
if(NOT verilator_FOUND)
    message(STATUS "Verilator not found: the cosimulator and the hardware tests are not built")
    return()
endif()

add_executable(cosim cosim.cpp)
target_include_directories(cosim PRIVATE ${PROJECT_SOURCE_DIR}/tests)
target_link_libraries(cosim PRIVATE libbrainfunk)
# Verilator's output is not ours to warn about, and LTO over it only costs build time.
set_target_properties(cosim PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
verilate(cosim
    TOP_MODULE brainfunk_soc
    PREFIX Vbrainfunk_soc
    VERILATOR_ARGS -O3 --x-assign fast --x-initial fast -DBRAINFUNK_RETIRE_PORT
    SOURCES
        ${PROJECT_SOURCE_DIR}/rtl/brainfunk_core.sv ${PROJECT_SOURCE_DIR}/rtl/brainfunk_predict.sv
        ${PROJECT_SOURCE_DIR}/rtl/brainfunk_mem.sv ${PROJECT_SOURCE_DIR}/rtl/brainfunk_cache.sv
        ${PROJECT_SOURCE_DIR}/rtl/sdram_ctrl.sv ${PROJECT_SOURCE_DIR}/tb/sdram_model.sv
        ${PROJECT_SOURCE_DIR}/rtl/brainfunk_soc.sv)
