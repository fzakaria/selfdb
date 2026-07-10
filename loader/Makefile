CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=gnu11
PKGS    := sqlite3
CFLAGS  += $(shell pkg-config --cflags $(PKGS))
LDLIBS  += $(shell pkg-config --libs $(PKGS))

PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin
LIBDIR  ?= $(PREFIX)/lib

OBJ := self-exec.o native.o selfld.o image.o
AUDIT_LIB := libself-audit.so

all: self-exec $(AUDIT_LIB)

self-exec: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

# rtld-audit library (M3a): shares the row->ELF builder in image.c
$(AUDIT_LIB): audit.c image.c image.h
	$(CC) $(CFLAGS) -fPIC -shared -o $@ audit.c image.c $(LDLIBS)

%.o: %.c self.h image.h
	$(CC) $(CFLAGS) -c -o $@ $<

install: all
	install -Dm755 self-exec $(DESTDIR)$(BINDIR)/self-exec
	install -Dm755 $(AUDIT_LIB) $(DESTDIR)$(LIBDIR)/$(AUDIT_LIB)

clean:
	rm -f self-exec $(AUDIT_LIB) $(OBJ)

.PHONY: all install clean
