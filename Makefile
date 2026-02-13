CC ?= cc
PREFIX ?= /usr
DESTDIR ?=
BINDIR ?= $(PREFIX)/bin
UNITDIR ?= $(PREFIX)/lib/systemd/system

TARGET := fw16-kbd-uleds
SRC := fw16-kbd-uleds.c

CFLAGS ?= -O2 -pipe
CPPFLAGS ?=
LDFLAGS ?=

.PHONY: all clean install uninstall

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LDFLAGS)

install: $(TARGET)
	install -Dm755 $(TARGET) "$(DESTDIR)$(BINDIR)/$(TARGET)"
	install -Dm644 fw16-kbd-uleds.service "$(DESTDIR)$(UNITDIR)/fw16-kbd-uleds.service"

clean:
	rm -f $(TARGET)

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/$(TARGET)"
	rm -f "$(DESTDIR)$(UNITDIR)/fw16-kbd-uleds.service"
