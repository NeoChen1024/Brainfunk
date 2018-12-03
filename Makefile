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

all: brainfunk visualbrainfunk bf2bitcode bf2c

brainfunk: brainfunk.o libbrainfunk.o
	$(CC) $(LDFLAGS) brainfunk.o libbrainfunk.o -o brainfunk

visualbrainfunk: visualbrainfunk.o libvbrainfunk.o
	$(CC) $(LDFLAGS) visualbrainfunk.o libvbrainfunk.o -o visualbrainfunk $(VLIBS)

bf2bitcode: libbrainfunk.o bf2bitcode.o
	$(CC) $(LDFLAGS) bf2bitcode.o libbrainfunk.o -o bf2bitcode

bf2c: libbrainfunk.o bf2c.o
	$(CC) $(LDFLAGS) libbrainfunk.o bf2c.o -o bf2c

libvbrainfunk.o:
	$(CC) $(CFLAGS) -DVISUAL -c libbrainfunk.c -o libvbrainfunk.o

clean:
	rm -fv brainfunk visualbrainfunk bf2bitcode bf2c *.o

clean-all: clean
	rm -rfv test

countline:
	wc -l *.c *.h

test: all
	$(SH) ./test.sh brainfunk
