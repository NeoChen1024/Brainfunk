CC	= cc
AR	= ar
RANLIB	= ranlib
CFLAGS	= -O3 -pipe -fPIE -I./libbrainfunk -std=c99 -pedantic

LIBOBJS	= libbrainfunk/libbrainfunk.o

.PHONY: all clean

all: libbrainfunk.a

libbrainfunk.a: $(LIBOBJS)
	$(AR) rvD libbrainfunk.a $(LIBOBJS)
	$(RANLIB) libbrainfunk.a

clean:
	rm -f $(LIBOBJS) libbrainfunk.a
