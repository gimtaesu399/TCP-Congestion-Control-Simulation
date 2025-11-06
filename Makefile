CC = cc
CFLAGS = -O2 -Wall -Wextra -std=c11
LDFLAGS =

BINARIES = sender receiver

all: $(BINARIES)

sender: sender.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

receiver: receiver.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(BINARIES) *.o

.PHONY: all clean


