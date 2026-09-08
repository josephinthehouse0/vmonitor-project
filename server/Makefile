CC = gcc
CFLAGS = -Wall -Wextra -O2 -I../include

all: monitor_server

monitor_server: monitor_server.c
	$(CC) $(CFLAGS) -o monitor_server monitor_server.c

clean:
	rm -f monitor_server

.PHONY: all clean