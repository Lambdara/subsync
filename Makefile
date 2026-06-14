CC ?= cc
CFLAGS ?= -Wall -Wextra -Wpedantic -std=c17
CPPFLAGS += $(shell pkg-config --cflags ncursesw)
LDFLAGS += $(shell pkg-config --libs ncursesw)

SOURCES = main.c tree.c ops.c ui.c
OBJECTS = $(SOURCES:.c=.o)

.PHONY: all clean

all: subsync

subsync: $(OBJECTS)
	$(CC) $(CFLAGS) -o $@ $(OBJECTS) $(LDFLAGS)

clean:
	rm -f $(OBJECTS) subsync
