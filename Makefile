CC	= cc
CFLAGS	= -O3 -finline -g3 -Wall -Wextra -pipe -fPIE -I.
LDFLAGS	= -fPIE
VLIBS	= -lncurses
# If you want to make it run faster
# CFLAGS += -DFAST
#
# If you needs LTO
# CFLAGS	+= -flto
# LDFLAGS	+= -flto
SH?=	/bin/sh

all: brainfunk visualbrainfunk bitfunk visualbitfunk

brainfunk: brainfunk.o libbrainfunk.o
	$(CC) $(LDFLAGS) brainfunk.o libbrainfunk.o -o brainfunk

visualbrainfunk: visualbrainfunk.o libvbrainfunk.o
	$(CC) $(LDFLAGS) visualbrainfunk.o libvbrainfunk.o -o visualbrainfunk $(VLIBS)

bitfunk: bitfunk.o libbrainfunk.o libbitcode.o
	$(CC) $(LDFLAGS) bitfunk.o libbrainfunk.o libbitcode.o -o bitfunk

visualbitfunk: libbitcode.o libvbrainfunk.o visualbitfunk.o
	$(CC) $(LDFLAGS) visualbitfunk.o libvbrainfunk.o libbitcode.o -o visualbitfunk $(VLIBS)

brainfunk.o:
	$(CC) $(CFLAGS) $(LDFLAGS) -c brainfunk.c

bitfunk.o:
	$(CC) $(CFLAGS) -DBITCODE -c brainfunk.c -o bitfunk.o

visualbitfunk.o:
	$(CC) $(CFLAGS) -DBITCODE -c visualbrainfunk.c -o visualbitfunk.o

visualbrainfunk.o:
	$(CC) $(CFLAGS) $(LDFLAGS) -c visualbrainfunk.c

libbrainfunk.o:
	$(CC) $(CFLAGS) $(LDFLAGS) -c libbrainfunk.c

libvbrainfunk.o:
	$(CC) $(CFLAGS) $(LDFLAGS) -DVISUAL -c libbrainfunk.c -o libvbrainfunk.o

libbitcode.o:
	$(CC) $(CFLAGS) $(LDFLAGS) -c libbitcode.c

clean:
	rm -fv brainfunk visualbrainfunk bitfunk visualbitfunk *.o

clean-all: clean
	rm -rfv test

countline:
	wc -l *.c *.h

test: all
	$(SH) ./test.sh brainfunk bitfunk
