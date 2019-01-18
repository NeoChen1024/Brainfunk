CC	= cc
CFLAGS	= -O3 -finline -g3 -Wall -Wextra -pipe -fPIE -I.
LDFLAGS	= -fPIE
VLIBS	= -lncurses
SH	?= /bin/sh
# If you needs LTO
# CFLAGS	+= -flto
# LDFLAGS	+= -flto
# gperf Profiling
# CFLAGS	+= -pg
# LDFLAGS	+= -pg

PROG= brainfunk visualbrainfunk bf2bitcode bf2c bfstrip
all: $(PROG)

brainfunk: brainfunk.o libbrainfunk.o
	$(CC) $(LDFLAGS) brainfunk.o libbrainfunk.o -o brainfunk

visualbrainfunk: visualbrainfunk.o libvbrainfunk.o
	$(CC) $(LDFLAGS) visualbrainfunk.o libvbrainfunk.o -o visualbrainfunk $(VLIBS)

bf2bitcode: libbrainfunk.o bf2bitcode.o
	$(CC) $(LDFLAGS) libbrainfunk.o bf2bitcode.o -o bf2bitcode

bf2c: libbrainfunk.o bf2c.o
	$(CC) $(LDFLAGS) libbrainfunk.o bf2c.o -o bf2c

bfstrip: bfstrip.o libbrainfunk.o
	$(CC) $(LDFLAGS) bfstrip.o libbrainfunk.o -o bfstrip

libvbrainfunk.o:
	$(CC) $(CFLAGS) -DVISUAL -c libbrainfunk.c -o libvbrainfunk.o

clean:
	rm -fv $(PROG) *.o

clean-all: clean
	rm -rfv test

countline:
	wc -l *.c *.h

test: all
	$(SH) ./test.sh brainfunk
