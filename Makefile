CC	= cc
CFLAGS	= -O3 -finline -g3 -Wall -Wextra -pipe -fPIE -ansi -I.
LDFLAGS	= -fPIE
VLIBS	= -lncurses
# If you want to make it run faster
# CFLAGS += -DFAST
#
# If you needs LTO
# CFLAGS	+= -flto
# LDFLAGS	+= -flto
SH?=	/bin/sh

all: brainfunk visualbrainfunk

brainfunk: brainfunk.o libbrainfunk.o
	$(CC) $(LDFLAGS) brainfunk.o libbrainfunk.o -o brainfunk

visualbrainfunk: visualbrainfunk.o libbrainfunk.o
	$(CC) $(LDFLAGS) visualbrainfunk.o libbrainfunk.o -o visualbrainfunk $(VLIBS)

brainfunk.o:
	$(CC) $(CFLAGS) $(LDFLAGS) -c brainfunk.c

visualbrainfunk.o:
	$(CC) $(CFLAGS) $(LDFLAGS) -c visualbrainfunk.c

libbrainfunk.o:
	$(CC) $(CFLAGS) $(LDFLAGS) -c libbrainfunk.c

clean:
	rm -fv brainfunk visualbrainfunk *.o

clean-all: clean
	rm -rfv test
test:
	$(SH) ./test.sh
