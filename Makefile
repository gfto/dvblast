VERSION_MAJOR = 3
VERSION_MINOR = 4
TOPDIR = `basename ${PWD}`
GIT_VER = $(shell git describe --tags --dirty --always 2>/dev/null)
uname_S := $(shell sh -c 'uname -s 2>/dev/null || echo not')
deltacast_inc := $(shell sh -c 'test -f /usr/include/StreamMaster.h && echo -n Y')

CFLAGS ?= -O3 -fomit-frame-pointer -g
CFLAGS += -Wall -Wformat-security -Wno-strict-aliasing
CFLAGS += -DVERSION=\"$(VERSION_MAJOR).$(VERSION_MINOR)\"
CFLAGS += -DVERSION_MAJOR=$(VERSION_MAJOR)
CFLAGS += -DVERSION_MINOR=$(VERSION_MINOR)
ifneq "$(GIT_VER)" ""
CFLAGS += -DVERSION_EXTRA=\"git-$(GIT_VER)\"
else
CFLAGS += -DVERSION_EXTRA=\"release\"
endif

ifeq ($(uname_S),Linux)
LDLIBS += -lrt
endif
ifeq ($(uname_S),Darwin)
LDLIBS += -liconv
endif

ifeq ($(deltacast_inc),Y)
CFLAGS += -DHAVE_ASI_DELTACAST_SUPPORT
LDLIBS += -lstreammaster
endif

LDLIBS_DVBLAST += -lpthread -lev

OBJ_DVBLAST = dvblast.o util.o dvb.o udp.o asi.o demux.o output.o en50221.o comm.o mrtg-cnt.o asi-deltacast.o
OBJ_DVBLASTCTL = util.o dvblastctl.o

ifndef V
Q = @
endif

CLEAN_OBJS = dvblast dvblastctl $(OBJ_DVBLAST) $(OBJ_DVBLASTCTL)
INSTALL_BIN = dvblast dvblastctl dvblast_mmi.sh
INSTALL_MAN = dvblast.1

PREFIX ?= /usr/local
BIN = $(subst //,/,$(DESTDIR)/$(PREFIX)/bin)
MAN = $(subst //,/,$(DESTDIR)/$(PREFIX)/share/man/man1)

all: dvblast dvblastctl

.PHONY: clean install uninstall dist

%.o: %.c Makefile config.h dvblast.h en50221.h comm.h asi.h mrtg-cnt.h asi-deltacast.h
	@echo "CC      $<"
	$(Q)$(CROSS)$(CC) $(CFLAGS) $(CPPFLAGS) -c $<

dvblast: $(OBJ_DVBLAST)
	@echo "LINK    $@"
	$(Q)$(CROSS)$(CC) $(LDFLAGS) -o $@ $(OBJ_DVBLAST) $(LDLIBS_DVBLAST) $(LDLIBS)

dvblastctl: $(OBJ_DVBLASTCTL)
	@echo "LINK    $@"
	$(Q)$(CROSS)$(CC) $(LDFLAGS) -o $@ $(OBJ_DVBLASTCTL) $(LDLIBS)

clean:
	@echo "CLEAN   $(CLEAN_OBJS)"
	$(Q)rm -f $(CLEAN_OBJS)

distclean: clean

install: all
	@install -d "$(BIN)"
	@install -d "$(MAN)"
	@echo "INSTALL $(INSTALL_MAN) -> $(MAN)"
	$(Q)install -m 644 dvblast.1 "$(MAN)"
	@echo "INSTALL $(INSTALL_BIN) -> $(BIN)"
	$(Q)install dvblast dvblastctl dvblast_mmi.sh "$(BIN)"

uninstall:
	@-for FILE in $(INSTALL_BIN); do \
		echo "RM       $(BIN)/$$FILE"; \
		rm "$(BIN)/$$FILE"; \
	done
	@-for FILE in $(INSTALL_MAN); do \
		echo "RM       $(MAN)/$$FILE"; \
		rm "$(MAN)/$$FILE"; \
	done

dist: clean
	@echo "ARCHIVE dvblast-$(VERSION_MAJOR).$(VERSION_MINOR).tar.bz2"
	$(Q)git archive --format=tar --prefix=dvblast-$(VERSION_MAJOR).$(VERSION_MINOR)/ master | bzip2 -9 > dvblast-$(VERSION_MAJOR).$(VERSION_MINOR).tar.bz2
	$(Q)ls -l dvblast-$(VERSION_MAJOR).$(VERSION_MINOR).tar.bz2

