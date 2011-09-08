# DVBlast Makefile
# Customise the path of your kernel

VERSION = 2.0.0
TOPDIR = `basename ${PWD}`
GIT_VER = $(shell git describe --tags --dirty --always 2>/dev/null)

CFLAGS += -Wall -Wformat-security -O3 -fomit-frame-pointer
CFLAGS += -g
ifneq "$(GIT_VER)" ""
CFLAGS += -DVERSION_EXTRA=\"git-$(GIT_VER)\"
else
CFLAGS += -DVERSION_EXTRA=\"release\"
endif
LDLIBS += -lrt
LDLIBS_DVBLAST += -lpthread

OBJ_DVBLAST = dvblast.o util.o dvb.o udp.o asi.o demux.o output.o en50221.o comm.o mrtg-cnt.o
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

%.o: %.c Makefile dvblast.h en50221.h comm.h version.h asi.h mrtg-cnt.h
	@echo "CC      $<"
	$(Q)$(CC) $(CFLAGS) -c $<

dvblast: $(OBJ_DVBLAST)
	@echo "LINK    $@"
	$(Q)$(CC) -o $@ $(OBJ_DVBLAST) $(LDLIBS_DVBLAST) $(LDLIBS)

dvblastctl: $(OBJ_DVBLASTCTL)
	@echo "LINK    $@"
	$(Q)$(CC) -o $@ $(OBJ_DVBLASTCTL) $(LDLIBS_DVBLAST) $(LDLIBS)

clean:
	@echo "CLEAN   $(CLEAN_OBJS)"
	$(Q)rm -f $(CLEAN_OBJS)

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
	@echo "ARCHIVE dvblast-$(VERSION).tar.bz2"
	$(Q)( cd ../ && \
	  tar -cj --exclude-vcs --exclude $(TOPDIR)/*.tar.bz2 $(TOPDIR)/ > $(TOPDIR)/dvblast-$(VERSION).tar.bz2 )
	$(Q)ls -l dvblast-$(VERSION).tar.bz2

