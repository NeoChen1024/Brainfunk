# Brainfunk: the interpreter and its tools, the cosimulator, and the FPGA flow.
#
#   cmake --workflow --preset default     configure, build, and run the tests that need no board
#   cmake --build build --target yadan    bitstream, with TD_HOME set; yadan-load puts it on the board
#
# Everything is built under build/, programs in build/bin. See tests/CMakeLists.txt for the tests.
cmake_minimum_required(VERSION 3.25)
project(Brainfunk LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(BRAINFUNK_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

include(CheckIPOSupported)
check_ipo_supported(RESULT brainfunk_ipo OUTPUT brainfunk_ipo_error LANGUAGES CXX)
if(brainfunk_ipo AND NOT BRAINFUNK_SANITIZE)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
endif()

add_compile_options(-pipe)
# Warnings for our own code; Verilator's generated code does not take them.
add_library(brainfunk_warnings INTERFACE)
target_compile_options(brainfunk_warnings INTERFACE -Wall -Wextra -pedantic)
if(BRAINFUNK_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
add_subdirectory(3rdparty/json EXCLUDE_FROM_ALL)
find_package(zstd REQUIRED)
find_package(Threads REQUIRED)

# The library every program links: interpreter, optimizer, code generators, image formats, traces.
add_library(libbrainfunk STATIC libbrainfunk.cpp optimizer.cpp llvm_codegen.cpp image.cpp trace_file.cpp analysis.cpp)
set_target_properties(libbrainfunk PROPERTIES OUTPUT_NAME brainfunk)
target_include_directories(libbrainfunk PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(libbrainfunk PUBLIC nlohmann_json::nlohmann_json PRIVATE brainfunk_warnings zstd::libzstd_shared Threads::Threads)

add_executable(brainfunk brainfunk.cpp)
target_link_libraries(brainfunk PRIVATE libbrainfunk brainfunk_warnings)

add_executable(bfload bfload.cpp)
target_link_libraries(bfload PRIVATE libbrainfunk brainfunk_warnings)

# The naive interpreter and the text tool stand alone.
add_executable(bf bf.cpp)
add_executable(bfstrip bfstrip.cpp)
target_link_libraries(bf PRIVATE brainfunk_warnings)
target_link_libraries(bfstrip PRIVATE brainfunk_warnings)

set(CURSES_NEED_WIDE TRUE)
find_package(Curses)
if(CURSES_FOUND)
    add_executable(visualbrainfunk visualbrainfunk.cpp)
    target_include_directories(visualbrainfunk PRIVATE ${CURSES_INCLUDE_DIRS})
    target_link_libraries(visualbrainfunk PRIVATE libbrainfunk brainfunk_warnings ${CURSES_LIBRARIES})
else()
    message(STATUS "ncursesw not found: visualbrainfunk is not built")
endif()

add_subdirectory(tb)
add_subdirectory(synth)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
