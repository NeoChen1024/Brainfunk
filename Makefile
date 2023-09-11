CC	= cc
CXX	= c++
OPT	= -Ofast -flto=auto -fwhole-program -pipe -fPIC -fPIE
FLAGS	= $(OPT) -I. -g3 -pedantic -Wall -Wextra -Wno-unused-parameter -DEXPECT_MACRO
#DBG	= -fsanitize=undefined,integer,nullability -fno-omit-frame-pointer
CFLAGS	= $(FLAGS) $(DBG) -std=c99
CXXFLAGS = $(FLAGS) $(DBG) -std=c++20
LDFLAGS	= -Wl,-O1 -Wl,--as-needed
OBJS	= brainfunk.o libbrainfunk.o
PRGS	= brainfunk bf bit2bin bfstrip

.PHONY: all clean countline

all: $(PRGS)

brainfunk: $(OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(OBJS) -o brainfunk

countline:
	wc -l *.h *.c

clean:
	rm -f $(OBJS) $(PRGS)
