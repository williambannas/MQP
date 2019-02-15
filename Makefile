CFLAGS?=-O2 -g -Wall -W -enable-checking
LDLIBS+=-liio -lpthread -lm -lad9361 -lfftw3
PROGNAME=sampling

all: sampling

%.o: %.c
	$(CC) $(CFLAGS) -c $<

sampling: sampling.o
	$(CC) -g -o sampling sampling.o $(LDFLAGS) $(LDLIBS)

sampling.o: sampling.c sampling.h
	$(CC) $(CFLAGS) -c sampling.c

clean:
	rm -f *.o sampling