# Every test runs under ctest; labels pick a part of them:
#
#   unit       the library, in Catch2: one ctest test per TEST_CASE
#   sw         the programs end to end, in pytest (tests/sw)
#   hw         cosimulation and the board's testbenches, in pytest (tests/hw)
#   formal     SymbiYosys
#   mutation   broken designs the tests must catch, with BRAINFUNK_TEST_MUTATION
#   board      the real board, with BRAINFUNK_TEST_BOARD; flash with BRAINFUNK_TEST_FLASH as well
#
#   ctest --preset sw        or hw, or default for everything enabled
#   ctest --test-dir build -L formal
option(BRAINFUNK_TEST_FULL "Also run whole long programs: mandel.b and the like" OFF)
option(BRAINFUNK_TEST_MUTATION "Register the mutation tests, which take about 10 minutes on 12 cores" OFF)
option(BRAINFUNK_TEST_BOARD "Register the tests on the real board, over JTAG and its serial port" OFF)
option(BRAINFUNK_TEST_FLASH "With the board tests, also program the board's flash, which wears it" OFF)

find_package(Catch2 3 REQUIRED)
include(Catch)
add_executable(test-libbrainfunk test-libbrainfunk.cpp)
target_include_directories(test-libbrainfunk PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test-libbrainfunk PRIVATE libbrainfunk brainfunk_warnings Catch2::Catch2WithMain)
catch_discover_tests(test-libbrainfunk TEST_PREFIX "unit." PROPERTIES LABELS unit TIMEOUT 300)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# A pytest run as one ctest test, told where the build is. Within it, pytest-xdist spreads its
# cases over PROCESSORS workers, which ctest counts against -j.
function(brainfunk_pytest name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "SERIAL" "LABEL;PROCESSORS;TIMEOUT" "FILES;ARGS")
    if(NOT arg_PROCESSORS)
        set(arg_PROCESSORS 1)
    endif()
    if(NOT arg_TIMEOUT)
        set(arg_TIMEOUT 1800)
    endif()
    set(workers -n 0)
    if(arg_PROCESSORS GREATER 1)
        set(workers -n ${arg_PROCESSORS})
    endif()
    add_test(NAME ${name}
             COMMAND ${Python3_EXECUTABLE} -m pytest -q -rfEs -p no:cacheprovider ${workers} ${arg_ARGS} ${arg_FILES}
             WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(${name} PROPERTIES
        LABELS ${arg_LABEL}
        PROCESSORS ${arg_PROCESSORS}
        TIMEOUT ${arg_TIMEOUT}
        ENVIRONMENT "BRAINFUNK_BUILD=${PROJECT_BINARY_DIR};BRAINFUNK_CTEST=1;TD_HOME=${TD_HOME}")
    if(arg_SERIAL)
        set_tests_properties(${name} PROPERTIES RUN_SERIAL ON)
    endif()
endfunction()

set(full)
if(BRAINFUNK_TEST_FULL)
    set(full --full)
endif()

brainfunk_pytest(sw.codegen LABEL sw PROCESSORS 8 FILES tests/sw/test_codegen.py)
brainfunk_pytest(sw.listing LABEL sw PROCESSORS 2 FILES tests/sw/test_listing.py)
brainfunk_pytest(sw.programs LABEL sw PROCESSORS 2 FILES tests/sw/test_programs.py)

if(NOT TARGET cosim)
    return()
endif()

brainfunk_pytest(hw.cosim LABEL hw PROCESSORS 8 FILES tests/hw/test_cosim.py ARGS ${full})
brainfunk_pytest(hw.loader LABEL hw PROCESSORS 4 FILES tests/hw/test_loader.py)
brainfunk_pytest(hw.uart LABEL hw PROCESSORS 2 FILES tests/hw/test_uart_tb.py)
brainfunk_pytest(formal LABEL formal PROCESSORS 5 FILES tests/hw/test_formal.py)

if(BRAINFUNK_TEST_MUTATION)
    brainfunk_pytest(mutation LABEL mutation PROCESSORS 12 TIMEOUT 7200 FILES tests/hw/test_mutation.py ARGS --mutation)
endif()

if(BRAINFUNK_TEST_BOARD)
    set(flash)
    if(BRAINFUNK_TEST_FLASH)
        set(flash --flash)
    endif()
    # One board: nothing else may use it meanwhile.
    brainfunk_pytest(board LABEL board SERIAL FILES tests/hw/test_board.py ARGS --board ${flash} ${full})
endif()

# The formal proofs on their own, as `make formal` was.
add_custom_target(formal
    COMMAND ${CMAKE_CTEST_COMMAND} --test-dir ${PROJECT_BINARY_DIR} -L formal --output-on-failure
    USES_TERMINAL)
