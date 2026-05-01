# Makefile for mycp
# Usage:
#   make         build the mycp binary
#   make clean   remove the binary

CC      = cc
CFLAGS  = -Wall -Wextra -std=c99 -D_POSIX_C_SOURCE=200809L -O2

mycp: mycp.c
	$(CC) $(CFLAGS) -o mycp mycp.c

clean:
	rm -f mycp *.o

.PHONY: clean