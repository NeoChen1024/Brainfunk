CC	= cc
CXX	= c++
OPT	= -Ofast -flto=auto -fwhole-program -pipe -fPIC -fPIE
FLAGS	= $(OPT) -I. -g3 -pedantic -Wall -Wextra -Wno-unused-parameter -DEXPECT_MACRO -D_POSIX_C_SOURCE=200809L
#DBG	= -fsanitize=undefined,integer,nullability -fno-omit-frame-pointer
CFLAGS	= $(FLAGS) $(DBG) -std=c99
CXXFLAGS = $(FLAGS) $(DBG) -std=c++20
LDFLAGS	= -Wl,-O1 -Wl,--as-needed
OBJS	= brainfunk.o libbrainfunk.o

.PHONY: all clean countline

all: brainfunk bf bit2bin

brainfunk: $(OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(OBJS) -o brainfunk

countline:
	wc -l *.h *.c

clean:
	rm -f $(OBJS) brainfunk bf bit2bin
