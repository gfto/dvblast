# DVBlast Makefile
# Customise the path of your kernel

VERSION = 2.0.0
TOPDIR = `basename ${PWD}`

CFLAGS += -Wall -Wformat-security -O3 -fomit-frame-pointer
CFLAGS += -g
LDLIBS += -lrt
LDLIBS_DVBLAST += -lpthread

OBJ_DVBLAST = dvblast.o util.o dvb.o udp.o asi.o demux.o output.o en50221.o comm.o mrtg-cnt.o
OBJ_DVBLASTCTL = util.o dvblastctl.o

PREFIX ?= /usr/local
BIN = $(DESTDIR)/$(PREFIX)/bin
MAN = $(DESTDIR)/$(PREFIX)/share/man/man1

all: dvblast dvblastctl

$(OBJ_DVBLAST) $(OBJ_DVBLASTCTL): Makefile dvblast.h en50221.h comm.h version.h asi.h

dvblast: $(OBJ_DVBLAST)
	$(CC) -o $@ $(OBJ_DVBLAST) $(LDLIBS_DVBLAST) $(LDLIBS)

dvblastctl: $(OBJ_DVBLASTCTL)

clean:
	@rm -f dvblast dvblastctl $(OBJ_DVBLAST) $(OBJ_DVBLASTCTL)

install: all
	@install -d $(BIN)
	@install -d $(MAN)
	@install dvblast dvblastctl dvblast_mmi.sh $(BIN)
	@install -m 644 dvblast.1 $(MAN)

uninstall:
	@rm $(BIN)/dvblast $(BIN)/dvblastctl $(BIN)/dvblast_mmi.sh $(MAN)/dvblast.1

dist:
	( cd ../ && \
	  tar -cj --exclude-vcs --exclude $(TOPDIR)/*.tar.bz2 $(TOPDIR)/ > $(TOPDIR)/dvblast-$(VERSION).tar.bz2 )

