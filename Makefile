CC=	cc
CFLAGS=	-O3 -finline -g3 -Wall -Wextra -pipe -fPIE -ansi -I.
# CFLAGS += -DFAST
SH?=	/bin/sh

all: brainfunk

brainfunk: brainfunk.o libbrainfunk.o
	$(CC) $(CFLAGS) $(LDFLAGS) brainfunk.o libbrainfunk.o -o brainfunk

brainfunk.o:
	$(CC) $(CFLAGS) $(LDFLAGS) -c brainfunk.c

libbrainfunk.o:
	$(CC) $(CFLAGS) $(LDFLAGS) -c libbrainfunk.c

clean:
	rm -rfv brainfunk *.o

clean-all:
	rm -rfv brainfunk *.o test
test:
	$(SH) ./test.sh
