CC	= cc
AR	= ar
RANLIB	= ranlib
CFLAGS	= -O3 -pipe -fPIE -I./libbrainfunk -gdwarf-4 -g3 -std=c99 -pedantic

LIBOBJS	= libbrainfunk/libbrainfunk.o libbrainfunk/handler.o
BFOBJS	= brainfunk.o

.PHONY: all clean

all: libbrainfunk.a brainfunk

brainfunk: $(BFOBJS) libbrainfunk.a
	$(CC) $(CFLAGS) $(LDFLAGS) $(BFOBJS) libbrainfunk.a -o brainfunk

libbrainfunk.a: $(LIBOBJS)
	$(AR) rvD libbrainfunk.a $(LIBOBJS)
	$(RANLIB) libbrainfunk.a

clean:
	rm -f $(LIBOBJS) libbrainfunk.a
