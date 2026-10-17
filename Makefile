CC      ?= cc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra
PROG     = arbiter

$(PROG): arbiter.c tinyjson.c tinyjson.h
	$(CC) $(CFLAGS) -o $(PROG) arbiter.c tinyjson.c

clean:
	rm -f $(PROG)

.PHONY: clean
