CC=	cc
CFLAGS=	-O3 -finline -g3 -Wall -Wextra -pipe -fPIE -ansi
EXE=	brainfunk brainfunk-fast
SH?=	/bin/sh

all: brainfunk brainfunk-fast

brainfunk:
	$(CC) $(CFLAGS) $(LDFLAGS) brainfunk.c -o brainfunk

brainfunk-fast:
	$(CC) $(CFLAGS) $(LDFLAGS) -DFAST brainfunk.c -o brainfunk-fast

clean:
	rm -rfv ${EXE}

clean-all:
	rm -rfv ${EXE} test
test:
	$(SH) ./test.sh
