CC=	gcc
CFLAGS=	-O2 -g2 -Wall -Wextra -pipe -fPIE
EXE=	brainfunk
SH?=	/bin/sh
all: $(EXE)

brainfunk:
	$(CC) $(CFLAGS) $(LDFLAGS) brainfunk.c -o brainfunk

clean:
	rm -fv ${EXE}
test:
	$(SH) ./test.sh
