CC = gcc
CFLAGS = -Wall -Wextra -O2

all: monitor_cli

monitor_cli: monitor_cli.c
	$(CC) $(CFLAGS) -o monitor_cli monitor_cli.c

clean:
	rm -f monitor_cli

.PHONY: all clean