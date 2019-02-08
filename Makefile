CFLAGS?=-O2 -g -Wall -W -enable-checking
LDLIBS+=-liio -lpthread -lm -lad9361 -lfftw3
PROGNAME=sampling

all: sampling tcp_client

%.o: %.c
	$(CC) $(CFLAGS) -c $<

sampling: sampling.o
	$(CC) -g -o sampling sampling.o $(LDFLAGS) $(LDLIBS)

sampling.o: sampling.c sampling.h 
	$(CC) $(CFLAGS) -c sampling.c

tcp_client: tcp_client.o
	$(CC) -g -o tcp_client tcp_client.o $(LDFLAGS) $(LDLIBS)

tcp_client.o: tcp_client.c 
	$(CC) $(CFLAGS) -c tcp_client.c

clean:
	rm -f *.o sampling tcp_client