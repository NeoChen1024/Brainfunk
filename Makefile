CC	= cc
CXX	= c++
OPT	= -O3 -flto=auto -pipe
FLAGS	= $(OPT) -I. -g3 -pedantic -Wall -Wextra -MMD -MP
#DBG	= -fsanitize=address,undefined -fno-omit-frame-pointer
CFLAGS	= $(FLAGS) $(DBG) -std=c99
CXXFLAGS = $(FLAGS) $(DBG) -std=c++20
LDFLAGS	= -Wl,-O1 -Wl,--as-needed
LIBOBJS	= libbrainfunk.o optimizer.o llvm_codegen.o image.o
OBJS	= brainfunk.o $(LIBOBJS)
PRGS	= brainfunk bf bfstrip visualbrainfunk bfload
DEPS	= brainfunk.d libbrainfunk.d optimizer.d llvm_codegen.d image.d bf.d bfstrip.d visualbrainfunk.d bfload.d

.PHONY: all clean countline test hw-test formal yadan yadan-load

all: $(PRGS)

brainfunk: $(OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(OBJS) -o brainfunk

bfload: bfload.cpp $(LIBOBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) bfload.cpp $(LIBOBJS) -o bfload

visualbrainfunk: visualbrainfunk.cpp $(LIBOBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) visualbrainfunk.cpp $(LIBOBJS) -o visualbrainfunk -lncursesw

countline:
	wc -l *.hpp *.cpp *.c

test: all
	sh tests/run-tests.sh

hw-test: all
	python3 -m pytest -n auto

formal:
	python3 -m pytest -n auto tests/hw/test_formal.py

yadan:
	sh synth/build-yadan.sh

yadan-load:
	sh synth/load-yadan.sh

clean:
	rm -f $(OBJS) $(PRGS) $(DEPS) tests/test-libbrainfunk
	rm -rf build

-include $(DEPS)
