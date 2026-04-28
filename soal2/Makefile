CC = gcc
CFLAGS = -Wall -pthread
LDFLAGS = -lrt

all: server client

server: orion.c arena.h
	$(CC) $(CFLAGS) orion.c -o orion $(LDFLAGS)

client: eternal.c arena.h
	$(CC) $(CFLAGS) eternal.c -o eternal $(LDFLAGS)

clean:
	rm -f orion eternal

clear_ipc:
	-ipcs -q | grep 0x0001234 | awk '{print $$2}' | xargs -r ipcrm -q
	-ipcs -m | grep 0x0005678 | awk '{print $$2}' | xargs -r ipcrm -m
	-ipcs -s | grep 0x0009012 | awk '{print $$2}' | xargs -r ipcrm -s
