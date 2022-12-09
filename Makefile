CC	= cc
OPT	= -Ofast -flto -pipe -DEXPECT_MACRO
#DBG	= -fsanitize=undefined,integer,nullability -fno-omit-frame-pointer
CFLAGS	= $(OPT) $(DBG) -pipe -fPIC -fPIE -I. -g3 -std=c99 -pedantic -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wno-unused-parameter
LDFLAGS	= -Wl,-O1 -Wl,--as-needed
OBJS	= brainfunk.o libbrainfunk.o

.PHONY: all clean countline test

all: brainfunk bf

brainfunk: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS) -o brainfunk

test:	brainfunk
	./test.sh brainfunk

countline:
	wc -l *.h *.c

clean:
	rm -f $(OBJS) brainfunk bf
