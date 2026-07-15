CC	= cc
CXX	= c++
OPT	= -O3 -flto=auto -pipe
FLAGS	= $(OPT) -I. -g3 -pedantic -Wall -Wextra -MMD -MP
#DBG	= -fsanitize=address,undefined -fno-omit-frame-pointer
CFLAGS	= $(FLAGS) $(DBG) -std=c99
CXXFLAGS = $(FLAGS) $(DBG) -std=c++20
LDFLAGS	= -Wl,-O1 -Wl,--as-needed
LIBOBJS	= libbrainfunk.o llvm_codegen.o
OBJS	= brainfunk.o $(LIBOBJS)
PRGS	= brainfunk bf bit2bin bfstrip visualbrainfunk
DEPS	= brainfunk.d libbrainfunk.d llvm_codegen.d bf.d bit2bin.d bfstrip.d visualbrainfunk.d

.PHONY: all clean countline test

all: $(PRGS)

brainfunk: $(OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(OBJS) -o brainfunk

visualbrainfunk: visualbrainfunk.cpp $(LIBOBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) visualbrainfunk.cpp $(LIBOBJS) -o visualbrainfunk -lncursesw

countline:
	wc -l *.hpp *.cpp *.c

test: all
	sh tests/run-tests.sh

clean:
	rm -f $(OBJS) $(PRGS) $(DEPS) tests/test-libbrainfunk

-include $(DEPS)
