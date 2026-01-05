PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

APP := sparta-mon
SRC := sparta_mon.c

CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra
LIBS ?= -lncursesw

VERSION ?= 0.1.0
ARCH ?= $(shell dpkg --print-architecture)

DISTDIR := dist
PKGDIR  := $(DISTDIR)/pkg

all: $(APP)

$(APP): $(SRC)
	$(CC) $(CFLAGS) $< -o $@ $(LIBS)

install: $(APP)
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(APP) $(DESTDIR)$(BINDIR)/$(APP)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(APP)

clean:
	rm -f $(APP)
	rm -rf $(DISTDIR)

deb: $(APP)
	rm -rf $(PKGDIR)
	mkdir -p $(PKGDIR)/DEBIAN $(PKGDIR)/usr/bin
	install -m 0755 $(APP) $(PKGDIR)/usr/bin/$(APP)
	printf "Package: sparta-mon\nVersion: $(VERSION)\nSection: utils\nPriority: optional\nArchitecture: $(ARCH)\nMaintainer: $(USER)\nDepends: libc6, libncursesw6 | libncursesw5\nDescription: Sparta Mon - ncurses system monitor\n A compact 3x2 panel live monitor for CPU/MEM/TEMP/DISK/NET and tasks.\n" > $(PKGDIR)/DEBIAN/control
	dpkg-deb --build $(PKGDIR) $(DISTDIR)/sparta-mon_$(VERSION)_$(ARCH).deb
	@echo "Built: $(DISTDIR)/sparta-mon_$(VERSION)_$(ARCH).deb"
