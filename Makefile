CC	= cc
CXX	= c++
OPT	= -Ofast -flto -pipe -pipe -fPIC -fPIE -I. -g3 -pedantic -Wall -Wextra -Wno-unused-parameter
#DBG	= -fsanitize=undefined,integer,nullability -fno-omit-frame-pointer
CFLAGS	= $(OPT) $(DBG) -std=c99 -DEXPECT_MACRO -D_POSIX_C_SOURCE=200809L
CXXFLAGS = $(OPT) -std=c++17
LDFLAGS	= -Wl,-O1 -Wl,--as-needed
OBJS	= brainfunk.o libbrainfunk.o

.PHONY: all clean countline test

all: brainfunk bf bit2bin

brainfunk: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS) -o brainfunk

test:	brainfunk
	./test.sh brainfunk

countline:
	wc -l *.h *.c

clean:
	rm -f $(OBJS) brainfunk bf bit2bin
