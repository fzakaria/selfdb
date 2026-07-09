CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=gnu11
PKGS    := sqlite3
CFLAGS  += $(shell pkg-config --cflags $(PKGS))
LDLIBS  += $(shell pkg-config --libs $(PKGS))

PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin

OBJ := self-exec.o native.o

self-exec: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

%.o: %.c self.h
	$(CC) $(CFLAGS) -c -o $@ $<

install: self-exec
	install -Dm755 self-exec $(DESTDIR)$(BINDIR)/self-exec

clean:
	rm -f self-exec $(OBJ)

.PHONY: install clean
