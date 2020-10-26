CC	= cc
OPT	= -Ofast -finline -flto -DEXPECT_MACRO
CFLAGS	= $(OPT) -pipe -fPIC -fPIE -I. -g -std=c99 -pedantic -D_POSIX_C_SOURCE=2 -Wall -Wextra -Wno-unused-parameter
OBJS	= brainfunk.o libbrainfunk.o

.PHONY: all clean countline test

all: brainfunk

brainfunk: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS) -o brainfunk

test:	brainfunk
	./test.sh brainfunk

countline:
	wc -l *.h *.c

clean:
	rm -f $(OBJS) brainfunk
