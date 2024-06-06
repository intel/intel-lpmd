CFLAGS_DBUS_GLIB = $(shell pkg-config --cflags --libs dbus-glib-1)

bindir ?= /usr/bin

CFLAGS ?= -g -Wall -Werror

all: intel_lpmd_control

intel_lpmd_control: intel_lpmd_control.c
	gcc $< -o $@ $(CFLAGS) $(CFLAGS_DBUS_GLIB) $(LDFLAGS)

clean:
	rm -f intel_lpmd_control

install: $(ALL_PROGRAMS)
	install -d -m 755 $(DESTDIR)$(bindir);          \
	install intel_lpmd_control $(DESTDIR)$(bindir);

.PHONY: all clean
