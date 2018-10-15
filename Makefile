CC	= cc
CFLAGS	= -O3 -finline -g3 -Wall -Wextra -pipe -fPIE -ansi -I.
LDFLAGS	= -fPIE
# If you want to make it run faster
# CFLAGS += -DFAST
#
# If you needs LTO
# CFLAGS	+= -flto
# LDFLAGS	+= -flto
SH?=	/bin/sh

all: brainfunk

brainfunk: brainfunk.o libbrainfunk.o
	$(CC) $(LDFLAGS) brainfunk.o libbrainfunk.o -o brainfunk

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
