# axiom-panel — GTK3 + X11 + GIO.
PKGS    := gtk+-3.0 gdk-x11-3.0 x11 gio-unix-2.0
CC      ?= gcc
CFLAGS  += -O2 -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations $(shell pkg-config --cflags $(PKGS))
LDFLAGS += $(shell pkg-config --libs $(PKGS)) -lm

SRC := main.c menu.c windows.c status.c session.c
OBJ := $(SRC:.c=.o)

axiom-panel: $(OBJ)
	$(CC) -o $@ $(OBJ) $(LDFLAGS)

%.o: %.c panel.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) axiom-panel

.PHONY: clean
