CC=	clang
CFLAGS=	-O3 -finline -g2 -Wall -Wextra -pipe -fPIE
EXE=	brainfunk
SH?=	/bin/sh
all:
	@echo "make brainfunk, brainfunk-fast, or test"

brainfunk:
	$(CC) $(CFLAGS) $(LDFLAGS) brainfunk.c -o brainfunk

brainfunk-fast:
	$(CC) $(CFLAGS) $(LDFLAGS) -DFAST brainfunk.c -o brainfunk

clean:
	rm -fv ${EXE}
test:
	$(SH) ./test.sh
