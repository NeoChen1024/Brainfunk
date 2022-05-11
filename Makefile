CXX=	c++
CXXFLAGS =	-Ofast -march=native -flto -Wl,-O1 -Wl,--as-needed -g3 -std=c++20 -pedantic -Wall -Wextra -pipe -Wno-unused-parameter
EXE =	brainfunk
.PHONY: all

all:	$(EXE)

$(EXE):	brainfunk.o libbrainfunk.o
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -static -o $@ $^

clean:
	rm -rf $(EXE) *.o
