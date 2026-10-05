CC ?= cc
CFLAGS ?= -O2
CPPFLAGS ?=
LDFLAGS ?=
LDLIBS ?= -lX11

all: xdf

xdf: xdf.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Wpedantic $(LDFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f xdf

.PHONY: all clean
