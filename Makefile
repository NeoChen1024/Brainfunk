CXX=	c++
CXXFLAGS =	-O3 -g3 -std=c++17 -pedantic -Wall -Wextra -pipe
EXE =	brainfunk
.PHONY: all

all:	$(EXE)

$(EXE):	brainfunk.o libbrainfunk.o
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -static -o $@ $^

clean:
	rm -rf $(EXE) *.o
