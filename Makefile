CC	= cc
AR	= ar
RANLIB	= ranlib
OPT	= -O3
CFLAGS	= $(OPT) -pipe -fPIE -I./libbrainfunk -gdwarf-4 -g3 -std=c99 -pedantic -D_DEFAULT_SOURCE -Wall -Wextra -Wno-unused-parameter

LIBOBJS	= libbrainfunk/libbrainfunk.o libbrainfunk/handler.o
BFOBJS	= brainfunk.o

.PHONY: all clean countline

all: libbrainfunk.a brainfunk

brainfunk: $(BFOBJS) libbrainfunk.a
	$(CC) $(CFLAGS) $(LDFLAGS) $(BFOBJS) libbrainfunk.a -o brainfunk

libbrainfunk.a: $(LIBOBJS)
	$(AR) rvD libbrainfunk.a $(LIBOBJS)
	$(RANLIB) libbrainfunk.a

test:	brainfunk
	./test.sh brainfunk

countline:
	wc -l */*.c */*.h *.c

clean:
	rm -f $(LIBOBJS) libbrainfunk.a
