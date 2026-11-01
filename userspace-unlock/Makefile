# Userspace CMP 50HX unlock tools. Plain C, no external dependencies.
CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=gnu11
LDFLAGS ?=

all: cmp50-regs

cmp50-regs: cmp50-regs.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

install: cmp50-regs
	install -m 0755 cmp50-regs /usr/local/sbin/cmp50-regs

clean:
	rm -f cmp50-regs

.PHONY: all install clean
