# Terminator - build with `make`, run ./terminator or ./terminator useProcess=True
CC       ?= gcc
CFLAGS   ?= -O2 -g
CFLAGS   += -Wall -Wextra -D_GNU_SOURCE $(shell pkg-config --cflags x11 2>/dev/null)
LDLIBS   := $(shell pkg-config --libs x11 2>/dev/null || echo -lX11) -lpthread -lrt

SRC  := src/main.c src/gui.c src/shell.c src/history.c src/complete.c src/ipc.c
OBJ  := $(SRC:.c=.o)

all: terminator

terminator: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

src/%.o: src/%.c src/terminator.h
	$(CC) $(CFLAGS) -c -o $@ $<

# headless engine tests (no X11 needed)
tests/engine_cli: tests/engine_cli.c src/shell.c src/ipc.c src/history.c src/terminator.h
	$(CC) $(CFLAGS) -o $@ tests/engine_cli.c src/shell.c src/ipc.c src/history.c -lpthread -lrt

test: tests/engine_cli
	sh tests/run_tests.sh

clean:
	rm -f $(OBJ) terminator tests/engine_cli

.PHONY: all clean test
